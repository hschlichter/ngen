#include "assetserver.h"

#include "assethash.h"
#include "rpcdiscovery.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <print>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// The job key: everything about a rule that changes what its packer writes.
auto jobKey(const PackRule& rule) -> uint64_t {
    auto hash = fnv1a64(rule.name);
    hash = fnv1a64(std::to_string(rule.version), fnv1a64(std::string_view("\0", 1), hash));
    hash = fnv1a64(rule.packer, fnv1a64(std::string_view("\0", 1), hash));
    for (const auto& [key, value] : rule.params) {
        hash = fnv1a64(key + "=" + value, fnv1a64(std::string_view("\0", 1), hash));
    }
    return hash;
}

// An asset id is a relative path below the server's directory, with no ".." segment.
auto validId(const std::string& id) -> bool {
    if (id.empty()) {
        return false;
    }
    fs::path path(id);
    if (path.is_absolute()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

auto readBytes(const std::string& path) -> std::optional<std::vector<std::byte>> {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }
    auto size = (size_t) in.tellg();
    in.seekg(0);
    std::vector<std::byte> bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), (std::streamsize) size);
    if (!in) {
        return std::nullopt;
    }
    return bytes;
}

auto splitLines(const std::string& text) -> std::vector<std::string> {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        if (end > start) {
            lines.push_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return lines;
}

} // namespace

auto AssetServer::start(const Options& startOptions) -> std::expected<void, std::string> {
    options = startOptions;
    rules = packRules(options.config);
    for (size_t i = 0; i < rules.size(); i++) {
        for (const auto& extension : rules[i].extensions) {
            auto [it, added] = ruleByExtension.emplace(extension, i);
            if (!added) {
                return std::unexpected(std::format("pack rules '{}' and '{}' both claim {}", rules[it->second].name, rules[i].name, extension));
            }
        }
    }
    // The cache is in the server's working directory, which is also the root asset ids are relative to; one folder per variant,
    // since packed assets differ by configuration.
    cacheDirectory = fs::path(".ngen-assets") / options.platform / options.config;
    cache.load(cacheDirectory / ".ngen-assetcache");
    for (const auto& rule : rules) {
        std::string extensions;
        for (const auto& extension : rule.extensions) {
            extensions += (extensions.empty() ? "" : " ") + extension;
        }
        traceLine("rule {}: {} -> {}", rule.name, extensions, rule.packer);
    }
    traceLine("cache {}: {} assets recorded", cacheDirectory.string(), cache.size());
    auto workers = std::max(1u, std::thread::hardware_concurrency());
    jobs = std::make_unique<PackJobs>(workers);

    addRecords();
    traceStream = std::make_unique<trace::TraceStream>(rpc, std::format("asset:{}", getpid()));
    rpc.setRequestHandler([this](RpcServer::ConnectionId connection, const rpc::Json& message, std::vector<std::byte>) {
        onMessage(connection, message);
    });
    rpc.setConnectionHandler([this](RpcServer::ConnectionId connection, bool connected) {
        traceLine("client {} {}", connection, connected ? "connected" : "disconnected");
        if (!connected) {
            traceStream->disconnected(connection);
        }
        std::lock_guard lock(streamMutex);
        if (connected) {
            connections.insert(connection);
        } else {
            connections.erase(connection);
            streams.erase(connection);
        }
    });
    rpc.setDrainHandler([this](RpcServer::ConnectionId connection) { pump(connection); }, packWindow / 2);
    auto port = rpc.start(0);
    if (!port) {
        return std::unexpected(port.error());
    }
    boundPort = *port;

    RpcEndpointInfo info;
    info.kind = "asset";
    info.pid = getpid();
    info.port = boundPort;
    info.label = variant();
    info.protocol = rpc::protocolVersion;
    info.startedUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto file = writeRpcEndpoint(info);
    if (!file) {
        rpc.stop();
        return std::unexpected(file.error());
    }
    discoveryFile = *file;
    return {};
}

auto AssetServer::stop() -> void {
    if (traceStream) {
        traceStream->finish(std::chrono::seconds(1));
    }
    rpc.stop();
    traceStream.reset();
    jobs.reset();
    if (!discoveryFile.empty()) {
        removeRpcEndpoint(discoveryFile);
        discoveryFile.clear();
    }
}

auto AssetServer::onMessage(RpcServer::ConnectionId connection, const rpc::Json& message) -> void {
    if (traceStream->handle(connection, message)) {
        return;
    }
    if (rpc::messageKind(message) != rpc::MessageKind::Request) {
        return;
    }
    const auto& id = message["id"];
    auto method = message["method"].get<std::string>();
    auto params = message.contains("params") ? message["params"] : rpc::Json::object();
    RpcResponder responder([this, connection, id](const RpcReply& reply) {
        if (reply.ok) {
            rpc.send(connection, rpc::makeResult(id, *reply.result));
        } else {
            rpc.send(connection, rpc::makeError(id, reply.code, reply.message));
        }
    });
    if (method == "asset.request") {
        onAssetRequest(connection, id, params);
    } else if (serveRecordsCall(records, method, params, responder)) {
        return;
    } else {
        traceWarning("client {}: unknown method {}", connection, method);
        rpc.send(connection, rpc::makeError(id, rpc::methodNotFound, std::format("no method {}", method)));
    }
}

auto AssetServer::onAssetRequest(RpcServer::ConnectionId connection, const rpc::Json& id, const rpc::Json& params) -> void {
    if (!params.contains("ids") || !params["ids"].is_array()) {
        traceWarning("client {}: asset.request without an \"ids\" array", connection);
        rpc.send(connection, rpc::makeError(id, rpc::invalidParams, "asset.request needs \"ids\", an array of asset ids"));
        return;
    }
    std::vector<std::string> ids;
    for (const auto& value : params["ids"]) {
        if (!value.is_string()) {
            rpc.send(connection, rpc::makeError(id, rpc::invalidParams, "asset ids are strings"));
            return;
        }
        ids.push_back(value.get<std::string>());
    }
    rpc::Json held = params.contains("have") && params["have"].is_object() ? params["have"] : rpc::Json::object();
    auto request = nextRequest++;
    if (ids.empty()) {
        traceLine("client {} request {}: no assets", connection, request);
    } else if (ids.size() == 1) {
        traceLine("client {} request {}: {}", connection, request, ids.front());
    } else {
        traceLine("client {} request {}: {} assets", connection, request, ids.size());
    }
    if (!ids.empty()) {
        std::lock_guard lock(requestMutex);
        requests[request] = RequestTrace{.connection = connection, .assets = ids.size(), .remaining = ids.size(), .start = std::chrono::steady_clock::now()};
    }
    rpc.send(connection, rpc::makeResult(id, {{"request", request}}));
    for (const auto& asset : ids) {
        Subscriber subscriber;
        subscriber.connection = connection;
        subscriber.request = request;
        if (held.contains(asset) && held[asset].is_string()) {
            subscriber.held = held[asset].get<std::string>();
        }
        subscribe(asset, subscriber);
    }
}

auto AssetServer::addRecords() -> void {
    records.add("status", "pid, variant, directory, clients connected, pack tasks running and queued, packer runs", [this](RpcResponder responder) {
        responder.respond(status());
    });
    records.add("rules", "the pack rules from pack.cpp: extensions, packer, parameters, version", [this](RpcResponder responder) {
        responder.respond(rulesRecord());
    });
    records.add("cache", "every packed asset: version, packer, inputs and the packed file", [this](RpcResponder responder) {
        responder.respond(cacheRecord());
    });
    records.add("requests", "asset requests in flight, and the most recent finished ones", [this](RpcResponder responder) {
        responder.respond(requestsRecord());
    });
    records.add("clients", "connected clients, the bytes queued to each and the assets waiting to stream", [this](RpcResponder responder) {
        responder.respond(clientsRecord());
    });
}

auto AssetServer::status() -> rpc::Json {
    size_t clients = 0;
    {
        std::lock_guard lock(streamMutex);
        clients = connections.size();
    }
    return {
        {"pid", getpid()},
        {"variant", variant()},
        {"directory", fs::current_path().string()},
        {"clients", clients},
        {"tasksRunning", jobs->running()},
        {"tasksQueued", jobs->queued()},
        {"packerRuns", packerRuns.load()},
    };
}

auto AssetServer::clientsRecord() -> rpc::Json {
    auto clients = rpc::Json::array();
    std::lock_guard lock(streamMutex);
    for (auto connection : connections) {
        size_t peak = 0;
        size_t waiting = 0;
        auto it = streams.find(connection);
        if (it != streams.end()) {
            peak = it->second.peakQueued;
            waiting = it->second.queue.size();
        }
        clients.push_back({
            {"connection", connection},
            {"queuedBytes", rpc.queuedBytes(connection)},
            {"peakQueuedBytes", peak},
            {"assetsWaiting", waiting},
        });
    }
    return clients;
}

auto AssetServer::requestsRecord() -> rpc::Json {
    auto now = std::chrono::steady_clock::now();
    auto inFlightRequests = rpc::Json::array();
    std::lock_guard lock(requestMutex);
    for (const auto& [request, entry] : requests) {
        inFlightRequests.push_back({
            {"request", request},
            {"connection", entry.connection},
            {"assets", entry.assets},
            {"remaining", entry.remaining},
            {"sent", entry.sent},
            {"held", entry.held},
            {"failed", entry.failed},
            {"bytes", entry.bytes},
            {"ms", std::chrono::duration<double, std::milli>(now - entry.start).count()},
        });
    }
    std::sort(inFlightRequests.begin(), inFlightRequests.end(), [](const rpc::Json& a, const rpc::Json& b) { return a["request"].get<int64_t>() < b["request"].get<int64_t>(); });
    auto finished = rpc::Json::array();
    for (const auto& entry : finishedRequests) {
        finished.push_back({
            {"request", entry.request},
            {"connection", entry.trace.connection},
            {"assets", entry.trace.assets},
            {"sent", entry.trace.sent},
            {"held", entry.trace.held},
            {"failed", entry.trace.failed},
            {"bytes", entry.trace.bytes},
            {"ms", entry.ms},
        });
    }
    return {{"inFlight", inFlightRequests}, {"finished", finished}};
}

auto AssetServer::cacheRecord() -> rpc::Json {
    auto fileJson = [](const AssetCache::File& file) -> rpc::Json {
        return {{"path", file.path}, {"size", file.size}, {"hash", hashText(file.hash)}};
    };
    auto assets = rpc::Json::array();
    for (const auto& [id, record] : cache.entries()) {
        auto inputs = rpc::Json::array();
        for (const auto& input : record.inputs) {
            inputs.push_back(fileJson(input));
        }
        assets.push_back({
            {"id", id},
            {"version", hashText(record.output.hash)},
            {"packer", fileJson(record.packer)},
            {"inputs", inputs},
            {"output", fileJson(record.output)},
        });
    }
    return {{"directory", cacheDirectory.string()}, {"assets", assets}};
}

auto AssetServer::rulesRecord() const -> rpc::Json {
    auto result = rpc::Json::array();
    for (const auto& rule : rules) {
        auto params = rpc::Json::object();
        for (const auto& [key, value] : rule.params) {
            params[key] = value;
        }
        result.push_back({
            {"name", rule.name},
            {"extensions", rule.extensions},
            {"packer", rule.packer},
            {"params", params},
            {"version", rule.version},
        });
    }
    return result;
}

auto AssetServer::subscribe(const std::string& id, Subscriber subscriber) -> void {
    {
        std::lock_guard lock(inFlightMutex);
        auto it = inFlight.find(id);
        if (it != inFlight.end()) {
            // Already being packed: the result goes to this request too.
            traceLine("  {}  already in progress; request {} waits for it", id, subscriber.request);
            it->second.push_back(subscriber);
            return;
        }
        inFlight[id].push_back(subscriber);
    }
    jobs->submit([this, id] {
        auto outcome = pack(id);
        deliver(id, outcome);
    });
}

auto AssetServer::pack(const std::string& id) -> Outcome {
    Outcome outcome;
    if (!validId(id)) {
        outcome.errors.push_back(std::format("{} is not an asset id: a relative path below the server's directory", id));
        traceWarning("  {}  refused: not a relative path below the server's directory", id);
        return outcome;
    }
    auto extension = fs::path(id).extension().string();
    auto ruleIt = ruleByExtension.find(extension);
    if (ruleIt == ruleByExtension.end()) {
        outcome.errors.push_back(std::format("{}: no pack rule for '{}' files", id, extension));
        traceWarning("  {}  failed: no pack rule for '{}' files", id, extension);
        return outcome;
    }
    const auto& rule = rules[ruleIt->second];
    std::error_code ec;
    if (!fs::is_regular_file(id, ec)) {
        outcome.errors.push_back(std::format("{}: no such file", id));
        traceWarning("  {}  failed: no such file", id);
        return outcome;
    }
    auto key = jobKey(rule);
    auto packer = (options.binDirectory / rule.packer).string();
    auto output = (cacheDirectory / id).string();
    auto depfile = output + ".d";
    outcome.path = output;
    if (auto version = cache.upToDate(id, key)) {
        outcome.ok = true;
        outcome.version = *version;
        traceLine("  {}  up to date ({})", id, hashText(*version));
        return outcome;
    }

    std::vector<std::string> argv = {
        packer,
        "--rule",
        rule.name,
        "--rule-version",
        std::to_string(rule.version),
        "--asset",
        id,
        "--source",
        id,
        "--out",
        output,
        "--depfile",
        depfile,
    };
    for (const auto& [name, value] : rule.params) {
        argv.push_back("--param");
        argv.push_back(name + "=" + value);
    }
    traceLine("  {}  packing with {}", id, rule.packer);
    packerRuns++;
    auto started = std::chrono::steady_clock::now();
    auto result = runProcess(argv);
    if (result.exitCode != 0) {
        outcome.errors = splitLines(result.output);
        outcome.errors.push_back(std::format("{} exited with {}", rule.packer, result.exitCode));
        traceWarning("  {}  failed after {} ms: {} exited with {}", id, millisSince(started), rule.packer, result.exitCode);
        for (const auto& line : splitLines(result.output)) {
            traceWarning("  {}    {}", id, line);
        }
        return outcome;
    }
    std::vector<std::string> inputs = {id};
    for (const auto& dep : parseDepfile(depfile)) {
        if (std::find(inputs.begin(), inputs.end(), dep) == inputs.end()) {
            inputs.push_back(dep);
        }
    }
    auto version = cache.record(id, key, packer, inputs, output);
    if (!version) {
        outcome.errors.push_back(std::format("{}: cannot hash the packer's inputs or output", id));
        traceWarning("  {}  failed: cannot hash the packer's inputs or output", id);
        return outcome;
    }
    outcome.ok = true;
    outcome.packed = true;
    outcome.version = *version;
    auto size = fs::file_size(output, ec);
    traceLine("  {}  packed in {} ms, {}, {} inputs ({})", id, millisSince(started), bytesText(ec ? 0 : size), inputs.size(), hashText(*version));
    return outcome;
}

auto AssetServer::deliver(const std::string& id, const Outcome& outcome) -> void {
    std::vector<Subscriber> subscribers;
    {
        std::lock_guard lock(inFlightMutex);
        subscribers = std::move(inFlight[id]);
        inFlight.erase(id);
    }
    auto version = hashText(outcome.version);
    std::shared_ptr<const std::vector<std::byte>> bytes;
    for (const auto& subscriber : subscribers) {
        if (!outcome.ok) {
            rpc.send(subscriber.connection, rpc::makeNotification("asset.failed", {{"request", subscriber.request}, {"id", id}, {"errors", outcome.errors}}));
            answered(subscriber.request, Answer::Failed, 0);
            continue;
        }
        if (subscriber.held == version) {
            std::error_code ec;
            auto size = fs::file_size(outcome.path, ec);
            rpc.send(subscriber.connection,
                     rpc::makeNotification("asset.ready", {{"request", subscriber.request}, {"id", id}, {"version", version}, {"size", ec ? 0 : size}, {"sent", false}, {"packed", outcome.packed}}));
            answered(subscriber.request, Answer::Held, 0);
            continue;
        }
        if (!bytes) {
            auto read = readBytes(outcome.path);
            if (!read) {
                rpc.send(subscriber.connection,
                         rpc::makeNotification("asset.failed", {{"request", subscriber.request}, {"id", id}, {"errors", {std::format("cannot read {}", outcome.path)}}}));
                traceWarning("  {}  failed: cannot read {}", id, outcome.path);
                answered(subscriber.request, Answer::Failed, 0);
                continue;
            }
            bytes = std::make_shared<const std::vector<std::byte>>(std::move(*read));
        }
        {
            std::lock_guard lock(streamMutex);
            if (!connections.contains(subscriber.connection)) {
                std::lock_guard requestLock(requestMutex);
                requests.erase(subscriber.request);
                continue;
            }
            streams[subscriber.connection].queue.push_back(Outgoing{
                .request = subscriber.request,
                .id = id,
                .version = version,
                .packed = outcome.packed,
                .bytes = bytes,
                .offset = 0,
            });
        }
        pump(subscriber.connection);
    }
}

auto AssetServer::pump(RpcServer::ConnectionId connection) -> void {
    std::lock_guard lock(streamMutex);
    auto it = streams.find(connection);
    if (it == streams.end()) {
        return;
    }
    auto& stream = it->second;
    // A chunk goes out only when it fits in the window, so no more than packWindow bytes are ever queued (plus a
    // few hundred bytes of JSON).
    while (!stream.queue.empty() && rpc.queuedBytes(connection) + packChunk <= packWindow) {
        auto& outgoing = stream.queue.front();
        const auto& bytes = *outgoing.bytes;
        auto count = std::min(packChunk, bytes.size() - outgoing.offset);
        if (count > 0) {
            rpc.send(connection,
                     rpc::makeNotification("asset.data",
                                           {
                                               {"request", outgoing.request},
                                               {"id", outgoing.id},
                                               {"version", outgoing.version},
                                               {"offset", outgoing.offset},
                                               {"total", bytes.size()},
                                           }),
                     std::span(bytes.data() + outgoing.offset, count));
            outgoing.offset += count;
            stream.peakQueued = std::max(stream.peakQueued, rpc.queuedBytes(connection));
        }
        if (outgoing.offset == bytes.size()) {
            rpc.send(connection,
                     rpc::makeNotification("asset.ready",
                                           {{"request", outgoing.request}, {"id", outgoing.id}, {"version", outgoing.version}, {"size", bytes.size()}, {"sent", true}, {"packed", outgoing.packed}}));
            answered(outgoing.request, Answer::Sent, bytes.size());
            stream.queue.pop_front();
            continue;
        }
        // Interleave: the next chunk comes from the next asset in the queue.
        if (stream.queue.size() > 1) {
            auto rest = std::move(stream.queue.front());
            stream.queue.pop_front();
            stream.queue.push_back(std::move(rest));
        }
    }
}

auto AssetServer::answered(int64_t request, Answer answer, uint64_t bytes) -> void {
    std::lock_guard lock(requestMutex);
    auto it = requests.find(request);
    if (it == requests.end()) {
        return;
    }
    auto& entry = it->second;
    if (answer == Answer::Sent) {
        entry.sent++;
        entry.bytes += bytes;
    } else if (answer == Answer::Held) {
        entry.held++;
    } else {
        entry.failed++;
    }
    if (--entry.remaining > 0) {
        return;
    }
    traceLine("client {} request {}: done in {} ms; {} sent ({}), {} already held, {} failed", entry.connection, request, millisSince(entry.start), entry.sent, bytesText(entry.bytes), entry.held, entry.failed);
    finishedRequests.push_back({
        .request = request,
        .trace = entry,
        .ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - entry.start).count(),
    });
    if (finishedRequests.size() > finishedRequestsKept) {
        finishedRequests.pop_front();
    }
    requests.erase(it);
}
