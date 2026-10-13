#include "assetserver.h"

#include "assethash.h"
#include "packer.h"
#include "rpcdiscovery.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <print>
#include <set>
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

// Whether `text` matches `pattern`, where '*' matches any run of characters (slashes too).
auto globMatch(std::string_view pattern, std::string_view text) -> bool {
    size_t p = 0;
    size_t t = 0;
    size_t starAt = std::string_view::npos;
    size_t starText = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            starAt = p++;
            starText = t;
        } else if (p < pattern.size() && pattern[p] == text[t]) {
            p++;
            t++;
        } else if (starAt != std::string_view::npos) {
            p = starAt + 1;
            t = ++starText;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        p++;
    }
    return p == pattern.size();
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
    for (const auto& delay : options.delays) {
        traceWarning("test aid: answers for {} are held {} ms (--delay)", delay.pattern, delay.ms);
    }
    auto workers = std::max(1u, std::thread::hardware_concurrency());
    jobs = std::make_unique<PackJobs>(workers);

    addRecords();
    watcher.start([this](std::vector<fs::path> files) { onFilesChanged(std::move(files)); }, [](const std::string& message) { traceWarning("watch: {}", message); }, options.pollOnly);
    traceLine("watching the sources of what clients receive, {}", options.pollOnly ? "polled once a second" : "through inotify");
    traceStream = std::make_unique<trace::TraceStream>(rpc, std::format("asset:{}", getpid()));
    rpc.setRequestHandler([this](RpcServer::ConnectionId connection, const rpc::Json& message, std::vector<std::byte>) {
        onMessage(connection, message);
    });
    rpc.setConnectionHandler([this](RpcServer::ConnectionId connection, bool connected) {
        traceLine("client {} {}", connection, connected ? "connected" : "disconnected");
        if (!connected) {
            traceStream->disconnected(connection);
            forget(connection);
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
    // First: the watcher's thread sends notices through rpc.
    watcher.stop();
    if (traceStream) {
        traceStream->finish(std::chrono::seconds(1));
    }
    rpc.stop();
    traceStream.reset();
    jobs.reset();
    cache.flush();
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
    bool force = params.value("force", false);
    auto request = nextRequest++;
    if (ids.empty()) {
        traceLine("client {} request {}: no assets", connection, request);
    } else if (ids.size() == 1) {
        traceLine("client {} request {}: {}{}", connection, request, ids.front(), force ? ", forced repack" : "");
    } else {
        traceLine("client {} request {}: {} assets{}", connection, request, ids.size(), force ? ", forced repack" : "");
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
        subscribe(asset, subscriber, force ? request : 0);
    }
}

auto AssetServer::addRecords() -> void {
    records.add("status", "pid, variant, directory, clients connected, pack tasks running and queued, packer runs", [this](RpcResponder responder) {
        responder.respond(status());
    });
    records.add("rules", "the pack rules from pack.cpp: extensions, packer, parameters, version", [this](RpcResponder responder) {
        responder.respond(rulesRecord());
    });
    records.add("cache", "every packed asset: version, packer, inputs, the packed file and its secondary outputs", [this](RpcResponder responder) {
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
        {"watchedDirectories", watcher.watchedDirectories()},
        {"polledDirectories", watcher.polledDirectories()},
        {"changeNotices", noticesSent.load()},
    };
}

auto AssetServer::clientsRecord() -> rpc::Json {
    auto clients = rpc::Json::array();
    std::unordered_map<RpcServer::ConnectionId, size_t> receivedCount;
    {
        std::lock_guard lock(watchMutex);
        for (const auto& [connection, ids] : receivedBy) {
            receivedCount[connection] = ids.size();
        }
    }
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
            {"received", receivedCount[connection]},
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
        auto secondaries = rpc::Json::array();
        for (const auto& [secondaryId, secondaryFile] : record.secondaries) {
            secondaries.push_back({{"id", secondaryId}, {"version", hashText(secondaryFile.hash)}, {"output", fileJson(secondaryFile)}});
        }
        assets.push_back({
            {"id", id},
            {"version", hashText(record.output.hash)},
            {"packer", fileJson(record.packer)},
            {"inputs", inputs},
            {"output", fileJson(record.output)},
            {"secondaries", secondaries},
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

auto AssetServer::subscribe(const std::string& id, Subscriber subscriber, int64_t forceRequest) -> void {
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
    jobs->submit([this, id, forceRequest] {
        auto started = std::chrono::steady_clock::now();
        auto outcome = pack(id, forceRequest);
        outcome.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        for (const auto& delay : options.delays) {
            if (globMatch(delay.pattern, id)) {
                traceLine("  {}  answer held {} ms (--delay)", id, delay.ms);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay.ms));
                break;
            }
        }
        deliver(id, outcome);
    });
}

auto AssetServer::pack(const std::string& id, int64_t forceRequest) -> Outcome {
    Outcome outcome;
    // "<file>#<fragment>" names a secondary output of <file>'s job: a mesh or material the file defines.
    auto hash = id.find('#');
    auto primary = id.substr(0, hash);
    if (!validId(primary)) {
        outcome.errors.push_back(std::format("{} is not an asset id: a relative path below the server's directory", id));
        traceWarning("  {}  refused: not a relative path below the server's directory", id);
        return outcome;
    }
    auto extension = fs::path(primary).extension().string();
    auto ruleIt = ruleByExtension.find(extension);
    if (ruleIt == ruleByExtension.end()) {
        outcome.errors.push_back(std::format("{}: no pack rule for '{}' files", id, extension));
        traceWarning("  {}  failed: no pack rule for '{}' files", id, extension);
        return outcome;
    }
    const auto& rule = rules[ruleIt->second];
    std::error_code ec;
    if (!fs::is_regular_file(primary, ec)) {
        outcome.errors.push_back(std::format("{}: no such file", primary));
        traceWarning("  {}  failed: no such file", id);
        return outcome;
    }
    auto key = jobKey(rule);

    // One job per file at a time: the outputs of one file, requested together, pack it once.
    std::shared_ptr<std::mutex> fileLock;
    {
        std::lock_guard lock(fileLocksMutex);
        auto& slot = fileLocks[primary];
        if (!slot) {
            slot = std::make_shared<std::mutex>();
        }
        fileLock = slot;
    }
    std::lock_guard jobLock(*fileLock);

    auto upToDate = [&]() -> bool {
        auto version = cache.upToDate(id, key);
        if (!version) {
            return false;
        }
        outcome.ok = true;
        outcome.version = *version;
        outcome.path = cache.outputPath(id).value_or((cacheDirectory / id).string());
        return true;
    };
    // A forcing request packs each file once: the first of its ids for the file forces the job, the others find what it wrote.
    bool forced = false;
    bool packedByThisRequest = false;
    if (forceRequest != 0) {
        std::lock_guard lock(fileLocksMutex);
        auto& last = forcedBy[primary];
        packedByThisRequest = last == forceRequest;
        forced = !packedByThisRequest;
        last = forceRequest;
    }
    if (!forced && upToDate()) {
        outcome.packed = packedByThisRequest;
        traceLine("  {}  {} ({})", id, packedByThisRequest ? "packed by this forced request" : "up to date", hashText(outcome.version));
        return outcome;
    }
    if (forced) {
        traceLine("  {}  repack forced", id);
    }
    auto ran = runPacker(primary, rule, key);
    if (!ran.ok) {
        outcome.errors = std::move(ran.errors);
        return outcome;
    }
    if (!upToDate()) {
        outcome.errors.push_back(std::format("{}: packing {} wrote no such output", id, primary));
        traceWarning("  {}  failed: packing {} wrote no such output", id, primary);
        return outcome;
    }
    outcome.packed = true;
    return outcome;
}

auto AssetServer::runPacker(const std::string& primary, const PackRule& rule, uint64_t key) -> Outcome {
    Outcome outcome;
    auto packer = (options.binDirectory / rule.packer).string();
    auto output = (cacheDirectory / primary).string();
    auto depfile = output + ".d";
    std::vector<std::string> argv = {
        packer,
        "--rule",
        rule.name,
        "--rule-version",
        std::to_string(rule.version),
        "--asset",
        primary,
        "--source",
        primary,
        "--out",
        output,
        "--depfile",
        depfile,
    };
    for (const auto& [name, value] : rule.params) {
        argv.push_back("--param");
        argv.push_back(name + "=" + value);
    }
    // A job writes its secondary outputs afresh; an earlier run's list would name files that may no longer exist.
    clearSecondaryOutputs(output);
    traceLine("  {}  packing with {}", primary, rule.packer);
    packerRuns++;
    auto started = std::chrono::steady_clock::now();
    auto result = runProcess(argv);
    if (result.exitCode != 0) {
        outcome.errors = splitLines(result.output);
        outcome.errors.push_back(std::format("{} exited with {}", rule.packer, result.exitCode));
        traceWarning("  {}  failed after {} ms: {} exited with {}", primary, millisSince(started), rule.packer, result.exitCode);
        for (const auto& line : splitLines(result.output)) {
            traceWarning("  {}    {}", primary, line);
        }
        return outcome;
    }
    // A packer's output on success is its warnings: shown, the job still succeeds.
    for (const auto& line : splitLines(result.output)) {
        traceWarning("  {}    {}", primary, line);
    }
    std::vector<std::string> inputs = {primary};
    for (const auto& dep : parseDepfile(depfile)) {
        if (std::find(inputs.begin(), inputs.end(), dep) == inputs.end()) {
            inputs.push_back(dep);
        }
    }
    auto secondaries = readSecondaryOutputs(output);
    auto version = cache.record(primary, key, packer, inputs, output, secondaries);
    if (!version) {
        outcome.errors.push_back(std::format("{}: cannot hash the packer's inputs or outputs", primary));
        traceWarning("  {}  failed: cannot hash the packer's inputs or outputs", primary);
        return outcome;
    }
    outcome.ok = true;
    outcome.packed = true;
    outcome.version = *version;
    outcome.path = output;
    std::error_code ec;
    auto size = fs::file_size(output, ec);
    if (secondaries.empty()) {
        traceLine("  {}  packed in {} ms, {}, {} inputs ({})", primary, millisSince(started), bytesText(ec ? 0 : size), inputs.size(), hashText(*version));
    } else {
        traceLine("  {}  packed in {} ms, {} and {} more outputs, {} inputs ({})",
                  primary,
                  millisSince(started),
                  bytesText(ec ? 0 : size),
                  secondaries.size(),
                  inputs.size(),
                  hashText(*version));
    }
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
        // Answered, sent, held or failed: a failed repack leaves the client with what it had, and a fix to the source is a change too.
        received(subscriber.connection, id, !outcome.ok);
        if (!outcome.ok) {
            rpc.send(subscriber.connection, rpc::makeNotification("asset.failed", {{"request", subscriber.request}, {"id", id}, {"errors", outcome.errors}, {"ms", outcome.ms}}));
            answered(subscriber.request, Answer::Failed, 0);
            continue;
        }
        if (subscriber.held == version) {
            std::error_code ec;
            auto size = fs::file_size(outcome.path, ec);
            rpc.send(subscriber.connection,
                     rpc::makeNotification("asset.ready", {{"request", subscriber.request}, {"id", id}, {"version", version}, {"size", ec ? 0 : size}, {"sent", false}, {"packed", outcome.packed}, {"ms", outcome.ms}}));
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
                .ms = outcome.ms,
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
                                           {{"request", outgoing.request}, {"id", outgoing.id}, {"version", outgoing.version}, {"size", bytes.size()}, {"sent", true}, {"packed", outgoing.packed}, {"ms", outgoing.ms}}));
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

auto AssetServer::jobKeyFor(const std::string& id) const -> std::optional<uint64_t> {
    auto primary = id.substr(0, id.find('#'));
    auto ruleIt = ruleByExtension.find(fs::path(primary).extension().string());
    if (ruleIt == ruleByExtension.end()) {
        return std::nullopt;
    }
    return jobKey(rules[ruleIt->second]);
}

auto AssetServer::received(RpcServer::ConnectionId connection, const std::string& id, bool failed) -> void {
    auto files = cache.filesOf(id);
    std::lock_guard lock(watchMutex);
    {
        std::lock_guard streamLock(streamMutex);
        if (!connections.contains(connection)) {
            return;
        }
    }
    if (receivedBy[connection].insert(id).second) {
        holders[id]++;
    }
    if (failed) {
        failedFor[connection].insert(id);
    } else if (auto it = failedFor.find(connection); it != failedFor.end()) {
        it->second.erase(id);
    }
    watchFiles(id, std::move(files));
}

auto AssetServer::forget(RpcServer::ConnectionId connection) -> void {
    std::lock_guard lock(watchMutex);
    auto it = receivedBy.find(connection);
    if (it == receivedBy.end()) {
        return;
    }
    for (const auto& id : it->second) {
        auto holder = holders.find(id);
        if (holder == holders.end() || --holder->second > 0) {
            continue;
        }
        holders.erase(holder);
        watchFiles(id, {});
    }
    receivedBy.erase(it);
    failedFor.erase(connection);
}

auto AssetServer::watchFiles(const std::string& id, std::vector<std::string> files) -> void {
    std::ranges::sort(files);
    auto& current = filesOfAsset[id];
    std::vector<fs::path> added;
    std::vector<fs::path> removed;
    for (const auto& file : current) {
        if (std::ranges::binary_search(files, file)) {
            continue;
        }
        auto& assets = assetsOfFile[file];
        assets.erase(id);
        if (assets.empty()) {
            assetsOfFile.erase(file);
            removed.emplace_back(file);
        }
    }
    for (const auto& file : files) {
        if (std::ranges::binary_search(current, file)) {
            continue;
        }
        auto& assets = assetsOfFile[file];
        if (assets.empty()) {
            added.emplace_back(file);
        }
        assets.insert(id);
    }
    if (files.empty()) {
        filesOfAsset.erase(id);
    } else {
        current = std::move(files);
    }
    watcher.unwatch(removed);
    watcher.watch(added);
}

auto AssetServer::onFilesChanged(std::vector<fs::path> files) -> void {
    std::set<std::string> candidates;
    {
        std::lock_guard lock(watchMutex);
        for (const auto& file : files) {
            auto it = assetsOfFile.find(file.string());
            if (it != assetsOfFile.end()) {
                candidates.insert(it->second.begin(), it->second.end());
            }
        }
    }
    // A touch, or a save of the same bytes, leaves the cache up to date: nothing changed, except to a client whose last answer for the id
    // was a failure (an edit that broke the source, undone).
    std::vector<std::string> changed;
    std::vector<std::string> restored;
    for (const auto& id : candidates) {
        auto key = jobKeyFor(id);
        if (!key || !cache.upToDate(id, *key)) {
            changed.push_back(id);
        } else {
            restored.push_back(id);
        }
    }
    std::string fileList;
    for (const auto& file : files) {
        fileList += (fileList.empty() ? "" : ", ") + file.string();
    }
    std::vector<std::pair<RpcServer::ConnectionId, std::vector<std::string>>> notices;
    size_t told = changed.size();
    {
        std::lock_guard lock(watchMutex);
        std::set<std::string> restoredTold;
        for (const auto& [connection, ids] : receivedBy) {
            std::vector<std::string> held;
            for (const auto& id : changed) {
                if (ids.contains(id)) {
                    held.push_back(id);
                }
            }
            if (auto failedIt = failedFor.find(connection); failedIt != failedFor.end()) {
                for (const auto& id : restored) {
                    if (failedIt->second.contains(id)) {
                        held.push_back(id);
                        restoredTold.insert(id);
                    }
                }
            }
            if (!held.empty()) {
                notices.emplace_back(connection, std::move(held));
            }
        }
        told += restoredTold.size();
    }
    if (notices.empty()) {
        traceLine("changed: {}; content unchanged, nothing to tell", fileList);
        return;
    }
    for (const auto& [connection, ids] : notices) {
        rpc.send(connection, rpc::makeNotification("asset.changed", {{"ids", ids}}));
        noticesSent++;
    }
    TRACE_EVENT("Asset", "AssetChanged", "asset-server")
        .text(std::format("changed: {}; {} asset{} out of date, {} client{} told", fileList, told, told == 1 ? "" : "s", notices.size(), notices.size() == 1 ? "" : "s"))
        .field("files", (int64_t) files.size())
        .field("assets", (int64_t) told)
        .field("clients", (int64_t) notices.size());
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
