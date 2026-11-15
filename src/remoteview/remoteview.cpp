#include "remoteview.h"

#include "log.h"
#include "profile.h"
#include "remoteviewcvars.h"
#include "videoconvert.h"
#include "videopage.h"
#include "videopattern.h"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>

namespace {

using Json = nlohmann::json;

constexpr uint64_t grabTimeoutNs = 5'000'000'000ull;
// A watcher whose page has said nothing for this long is gone: its browser was closed, or put away on a phone. The connection itself
// takes far longer to notice, and until it does every frame is still captured, encoded and sent to nobody.
constexpr uint64_t watcherSilenceNs = 5'000'000'000ull;

// Where the watch page is reached: at the address it listens on, or with 0.0.0.0 at each of this machine's IPv4 addresses but loopback
// (a browser doesn't connect its video to that one).
auto watchUrls(const std::string& address, uint16_t port) -> std::vector<std::string> {
    std::vector<std::string> urls;
    if (address != "0.0.0.0") {
        urls.push_back(std::format("http://{}:{}/", address, port));
        return urls;
    }
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        return urls;
    }
    for (const ifaddrs* interface = interfaces; interface != nullptr; interface = interface->ifa_next) {
        if (interface->ifa_addr == nullptr || interface->ifa_addr->sa_family != AF_INET || (interface->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(interface->ifa_addr)->sin_addr, text, sizeof(text));
        urls.push_back(std::format("http://{}:{}/", text, port));
    }
    freeifaddrs(interfaces);
    return urls;
}

auto unixMs() -> uint64_t {
    return (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

auto milliseconds(uint64_t fromNs, uint64_t toNs) -> double {
    return (double) (toNs - fromNs) / 1e6;
}

// A second's worth of one timing: summed as frames go by, read as their mean.
struct Mean {
    double sum = 0.0;
    uint32_t count = 0;

    auto add(double value) -> void {
        sum += value;
        count++;
    }

    auto take() -> double {
        double mean = count > 0 ? sum / count : 0.0;
        sum = 0.0;
        count = 0;
        return mean;
    }
};

} // namespace

RemoteView::RemoteView() = default;

RemoteView::~RemoteView() {
    shutdown();
}

auto RemoteView::fail(const std::string& message) -> void {
    std::lock_guard lock(mutex);
    if (live.lastError == message) {
        return;
    }
    live.lastError = message;
    live.events.push_back([message] {
        LOG_WARNING("RemoteView", "RemoteViewFailed", "remoteview").text(std::format("remote view: {}", message));
    });
}

auto RemoteView::update() -> void {
    Applied wanted = {
        .enabled = remoteviewcvars::enabled.get(),
        .address = remoteviewcvars::address.get(),
        .port = (uint16_t) remoteviewcvars::port.get(),
    };
    bool pattern = remoteviewcvars::source.get() == remoteviewcvars::sourcePattern;

    bool listenerChanged = wanted.enabled != applied.enabled || wanted.address != applied.address || wanted.port != applied.port;
    if (listenerChanged) {
        bool wasListening = http.listening();
        http.stop();
        sender.close();
        if (wasListening) {
            LOG_EVENT("RemoteView", "RemoteViewStopped", "remoteview").text("remote view: stopped listening");
        }
        if (wanted.enabled) {
            auto started = http.start(wanted.address, wanted.port, [this](const VideoHttpRequest& request) {
                return serve(request);
            });
            if (started) {
                watchPageUrls = watchUrls(wanted.address, wanted.port);
                std::string all;
                for (const auto& url : watchPageUrls) {
                    all += (all.empty() ? "" : ", ") + url;
                }
                LOG_EVENT("RemoteView", "RemoteViewListening", "remoteview")
                    .text(std::format("remote view: watch page at {}{}",
                                      all.empty() ? std::format("port {} (this machine has no address but loopback)", wanted.port) : all,
                                      remoteviewcvars::control.get() ? ", with control of the view" : ""))
                    .field("control", remoteviewcvars::control.get())
                    .field("address", wanted.address)
                    .field("port", (int64_t) wanted.port)
                    .field("url", watchPageUrls.empty() ? std::string() : watchPageUrls.front());
                std::lock_guard lock(mutex);
                live.lastError.clear();
            } else {
                fail(started.error());
            }
            startThread();
        } else {
            stopThread();
        }
        applied = wanted;
    }

    std::vector<std::function<void()>> events;
    std::optional<Grab> finishedGrab;
    std::optional<std::expected<void, std::string>> grabResult;
    uint64_t now = profile::now();
    uint64_t watcherReportNs = 0;
    {
        std::lock_guard lock(mutex);
        watcherReportNs = live.watcherReportNs;
        VideoSettings settings = {
            .width = 0,
            .height = (uint32_t) remoteviewcvars::height.get() & ~1u,
            .framesPerSecond = (uint32_t) remoteviewcvars::fps.get(),
            .bitrate = (uint32_t) remoteviewcvars::bitrateKbps.get() * 1000,
        };
        if (settings.framesPerSecond != live.settings.framesPerSecond || settings.bitrate != live.settings.bitrate) {
            live.restartEncoder = true;
        }
        live.settings = settings;
        live.pattern = pattern;
        live.bindAddress = wanted.address;
        live.control = remoteviewcvars::control.get();
        live.pageDir = remoteviewcvars::pageDir.get();
        events.swap(live.events);

        if (live.grab && live.grabResult) {
            finishedGrab = std::move(live.grab);
            grabResult = std::move(live.grabResult);
            live.grab.reset();
            live.grabResult.reset();
        } else if (live.grab && now - live.grab->askedNs > grabTimeoutNs) {
            finishedGrab = std::move(live.grab);
            grabResult = std::unexpected(std::string("no watch page sent a frame within five seconds"));
            live.grab.reset();
        }

        current.enabled = applied.enabled;
        current.listening = http.listening();
        current.source = pattern ? "pattern" : "frame";
        current.address = applied.address;
        current.port = applied.port;
        current.urls = current.listening ? watchPageUrls : std::vector<std::string>();
        current.url = current.urls.empty() ? std::string() : current.urls.front();
        current.width = live.width;
        current.height = live.height;
        current.framesPerSecond = settings.framesPerSecond;
        current.bitrate = settings.bitrate;
        current.encoder = live.encoder;
        current.frameWidth = live.frameWidth;
        current.frameHeight = live.frameHeight;
        current.captured = live.captured;
        current.replaced = live.replaced;
        current.encoded = live.encoded;
        current.sent = live.sent;
        current.sentBytes = live.sentBytes;
        current.keyframes = live.keyframes;
        current.copyMs = live.copyMs;
        current.convertMs = live.convertMs;
        current.encodeMs = live.encodeMs;
        current.captureToSendMs = live.captureToSendMs;
        current.report = live.report;
        current.lastError = live.lastError;
    }
    wake.notify_one();

    current.watcher = sender.state();
    current.watcherAddress = sender.watcherAddress();
    current.connects = sender.connects();
    current.disconnects = sender.disconnects();
    current.keyframeRequests = sender.keyframeRequests();

    for (auto& event : events) {
        event();
    }
    if (current.watcher == VideoWatcherState::Connected && current.connects == seenConnects && now - std::max(connectedNs, watcherReportNs) > watcherSilenceNs) {
        LOG_EVENT("RemoteView", "RemoteViewWatcherSilent", "remoteview").text("remote view: the watch page has not reported for five seconds; dropping the watcher");
        sender.close();
        current.watcher = sender.state();
        current.disconnects = sender.disconnects();
    }
    if (current.connects != seenConnects) {
        seenConnects = current.connects;
        connectedNs = now;
        sentAtConnect = current.sent;
        LOG_EVENT("RemoteView", "RemoteViewWatcherConnected", "remoteview")
            .text(std::format("remote view: watcher connected from {}", current.watcherAddress.empty() ? "an unknown address" : current.watcherAddress))
            .field("address", current.watcherAddress);
    }
    if (current.disconnects != seenDisconnects) {
        seenDisconnects = current.disconnects;
        double seconds = (double) (now - connectedNs) / 1e9;
        uint64_t frames = current.sent - sentAtConnect;
        LOG_EVENT("RemoteView", "RemoteViewWatcherDisconnected", "remoteview")
            .text(std::format("remote view: watcher gone after {:.1f} s and {} frames", seconds, frames))
            .field("seconds", seconds)
            .field("frames", (int64_t) frames);
    }
    if (finishedGrab && finishedGrab->done) {
        finishedGrab->done(*grabResult);
    }
}

auto RemoteView::takeFrameDue() -> std::optional<uint32_t> {
    if (!applied.enabled || current.source != "frame" || sender.state() != VideoWatcherState::Connected) {
        return std::nullopt;
    }
    uint64_t now = profile::now();
    uint64_t interval = 1'000'000'000ull / (uint64_t) std::max<int64_t>(1, remoteviewcvars::fps.get());
    // A frame that comes a little early still counts, or a view at twice the rate would send two frames in three.
    if (now + interval / 4 < nextCaptureNs) {
        return std::nullopt;
    }
    nextCaptureNs = std::max(nextCaptureNs + interval, now + interval / 2);
    return (uint32_t) remoteviewcvars::height.get() & ~1u;
}

auto RemoteView::submit(VideoImage&& image, double copyMs) -> void {
    {
        std::lock_guard lock(mutex);
        if (live.pending) {
            live.replaced++;
        }
        live.frameWidth = image.sourceWidth;
        live.frameHeight = image.sourceHeight;
        live.pending = std::move(image);
        live.captured++;
        live.copySum += copyMs;
        live.copyCount++;
    }
    wake.notify_one();
}

auto RemoteView::requestKeyframe() -> void {
    keyframeAsked = true;
}

auto RemoteView::disconnect() -> void {
    sender.close();
}

auto RemoteView::requestGrab(std::string path, std::function<void(std::expected<void, std::string>)> done) -> void {
    std::optional<Grab> replaced;
    {
        std::lock_guard lock(mutex);
        replaced = std::move(live.grab);
        live.grab = Grab{.path = std::move(path), .done = std::move(done), .askedNs = profile::now()};
        live.grabResult.reset();
    }
    if (replaced && replaced->done) {
        replaced->done(std::unexpected(std::string("replaced by a newer request")));
    }
}

auto RemoteView::shareFile(std::string name, std::string contentType, std::string bytes) -> void {
    std::lock_guard lock(mutex);
    live.fileName = std::move(name);
    live.fileType = std::move(contentType);
    live.fileBytes = std::move(bytes);
}

auto RemoteView::shutdown() -> void {
    http.stop();
    stopThread();
    sender.close();
    applied = {};
}

auto RemoteView::startThread() -> void {
    if (encodeThread.joinable()) {
        return;
    }
    {
        std::lock_guard lock(mutex);
        live.stop = false;
    }
    encodeThread = std::thread([this] {
        encodeLoop();
    });
}

auto RemoteView::stopThread() -> void {
    if (!encodeThread.joinable()) {
        return;
    }
    {
        std::lock_guard lock(mutex);
        live.stop = true;
    }
    wake.notify_one();
    encodeThread.join();
    std::lock_guard lock(mutex);
    live.pending.reset();
}

// The encode thread: sleeps until the renderer hands it an image or, with the pattern as the source, until the next one is due; then
// converts, encodes and sends. It holds one image at most, so a slow encoder or network drops frames and never adds delay. With no
// watcher it does nothing and the encoder is stopped.
auto RemoteView::encodeLoop() -> void {
    profile::registerThread("Remote View Encode");
    auto encoder = makeVideoEncoderOpenH264();
    bool encoding = false;
    VideoSettings started;
    VideoPlanes planes;
    VideoPacket packet;
    VideoImage patternImage;
    uint64_t nextPatternNs = 0;
    uint64_t secondStartNs = profile::now();
    Mean convertMean;
    Mean encodeMean;
    Mean sendMean;

    while (true) {
        VideoImage image;
        bool haveImage = false;
        VideoSettings settings;
        bool pattern = false;
        bool restart = false;
        {
            std::unique_lock lock(mutex);
            auto timeout = std::chrono::nanoseconds(profile::idleSlice);
            if (live.pattern && encoding) {
                uint64_t now = profile::now();
                uint64_t untilNext = nextPatternNs > now ? nextPatternNs - now : 0;
                timeout = std::min(timeout, std::chrono::nanoseconds(untilNext));
            }
            {
                PROFILE_ZONE_DETAIL("RemoteViewWait", Wait);
                wake.wait_for(lock, timeout, [this] {
                    return live.stop || live.pending.has_value();
                });
            }
            if (live.stop) {
                break;
            }
            settings = live.settings;
            pattern = live.pattern;
            restart = live.restartEncoder;
            live.restartEncoder = false;
            if (live.pending) {
                image = std::move(*live.pending);
                live.pending.reset();
                haveImage = true;
            }
        }

        if (sender.state() != VideoWatcherState::Connected) {
            if (encoding) {
                encoder->stop();
                encoding = false;
            }
            continue;
        }

        uint64_t now = profile::now();
        if (pattern) {
            if (now < nextPatternNs) {
                continue;
            }
            uint64_t interval = 1'000'000'000ull / std::max(1u, settings.framesPerSecond);
            nextPatternNs = std::max(nextPatternNs + interval, now);
            patternImage.height = settings.height;
            patternImage.width = (settings.height * 16 / 9) & ~1u;
            patternImage.captureNs = now;
            videopattern::draw(patternImage, unixMs());
            {
                std::lock_guard lock(mutex);
                live.captured++;
            }
        } else if (!haveImage) {
            continue;
        }
        const VideoImage& source = pattern ? patternImage : image;

        uint64_t convertStart = profile::now();
        {
            PROFILE_ZONE("RemoteViewConvert", Upload);
            convertToPlanes(source, planes);
        }
        uint64_t encodeStart = profile::now();

        bool keyframe = sender.takeKeyframeRequest();
        if (keyframeAsked.exchange(false)) {
            keyframe = true;
        }
        if (!encoding || restart || planes.width != started.width || planes.height != started.height) {
            VideoSettings next = settings;
            next.width = planes.width;
            next.height = planes.height;
            auto result = encoder->start(next);
            if (!result) {
                encoding = false;
                fail(result.error());
                continue;
            }
            encoding = true;
            started = next;
            keyframe = true;
            std::string name = encoder->name();
            std::lock_guard lock(mutex);
            live.encoder = name;
            live.width = next.width;
            live.height = next.height;
            live.events.push_back([name, next] {
                LOG_EVENT("RemoteView", "RemoteViewEncoderStarted", "remoteview")
                    .text(std::format("remote view: {} at {}x{}, {} frames a second, {} kbit/s", name, next.width, next.height, next.framesPerSecond, next.bitrate / 1000))
                    .field("encoder", name)
                    .field("width", (int64_t) next.width)
                    .field("height", (int64_t) next.height)
                    .field("fps", (int64_t) next.framesPerSecond)
                    .field("bitrate", (int64_t) next.bitrate);
            });
        }

        std::expected<void, std::string> encoded;
        {
            PROFILE_ZONE("RemoteViewEncode", Upload);
            encoded = encoder->encode(planes, keyframe, packet);
        }
        if (!encoded) {
            fail(encoded.error());
            continue;
        }
        if (packet.data.empty()) {
            continue;
        }
        packet.captureNs = source.captureNs;
        bool sent = false;
        {
            PROFILE_ZONE("RemoteViewSend", Io);
            sent = sender.send(packet);
        }
        uint64_t done = profile::now();
        convertMean.add(milliseconds(convertStart, encodeStart));
        encodeMean.add(milliseconds(encodeStart, done));
        sendMean.add(milliseconds(source.captureNs, done));

        std::lock_guard lock(mutex);
        live.encoded++;
        if (packet.keyframe) {
            live.keyframes++;
        }
        if (sent) {
            live.sent++;
            live.sentBytes += packet.data.size();
        }
        if (done - secondStartNs >= 1'000'000'000ull) {
            secondStartNs = done;
            live.convertMs = convertMean.take();
            live.encodeMs = encodeMean.take();
            live.captureToSendMs = sendMean.take();
            live.copyMs = live.copyCount > 0 ? live.copySum / live.copyCount : 0.0;
            live.copySum = 0.0;
            live.copyCount = 0;
        }
    }
    encoder->stop();
}

// The listener's thread. What the watch page asks: the page itself, an offer and the place for its answer, the engine's clock, the place
// for its once-a-second report, and the place for a frame the engine asked it for.
auto RemoteView::serve(const VideoHttpRequest& request) -> VideoHttpResponse {
    if (request.method == "GET") {
        if (const auto* file = watchPageFile(request.path)) {
            std::string pageDir;
            {
                std::lock_guard lock(mutex);
                pageDir = live.pageDir;
            }
            if (pageDir.empty()) {
                return {.status = 200, .contentType = std::string(file->contentType), .body = std::string(file->body)};
            }
            // The page's files from a directory, so the page is changed without a rebuild.
            std::ifstream in(pageDir + "/" + std::string(file->name), std::ios::binary);
            if (!in) {
                return {.status = 404, .body = std::format("no {} in {} (remoteview.pageDir)\n", file->name, pageDir)};
            }
            std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return {.status = 200, .contentType = std::string(file->contentType), .body = std::move(body)};
        }
    }
    if (request.method == "GET" && request.path.starts_with("/file/")) {
        std::lock_guard lock(mutex);
        if (live.fileName.empty() || request.path.substr(6) != live.fileName) {
            return {.status = 404, .body = "no such file\n"};
        }
        return {.status = 200, .contentType = live.fileType, .body = live.fileBytes};
    }
    if (request.method == "GET" && request.path == "/time") {
        return {.status = 200, .body = std::to_string(unixMs())};
    }
    if (request.method == "POST" && request.path == "/offer") {
        std::string bindAddress;
        bool control = false;
        {
            std::lock_guard lock(mutex);
            bindAddress = live.bindAddress;
            control = live.control;
        }
        auto offer = sender.offer(bindAddress, control);
        if (!offer) {
            fail(offer.error());
            return {.status = 503, .body = offer.error() + "\n"};
        }
        Json answer = {{"session", sender.session()}, {"sdp", *offer}, {"control", control}};
        return {.status = 200, .contentType = "application/json", .body = answer.dump()};
    }
    if (request.method == "POST" && request.path == "/answer") {
        auto accepted = sender.accept(request.body);
        if (!accepted) {
            fail(accepted.error());
            return {.status = 400, .body = accepted.error() + "\n"};
        }
        return {.status = 204};
    }
    if (request.method == "POST" && request.path == "/report") {
        auto json = Json::parse(request.body, nullptr, false);
        if (!json.is_object()) {
            return {.status = 400, .body = "expected a JSON object\n"};
        }
        RemoteViewReport report;
        report.received = true;
        report.receivedNs = profile::now();
        report.agent = json.value("agent", std::string());
        report.state = json.value("state", std::string());
        report.framesDecoded = json.value("framesDecoded", (uint64_t) 0);
        report.framesDropped = json.value("framesDropped", (uint64_t) 0);
        report.framesPerSecond = json.value("framesPerSecond", 0.0);
        report.width = json.value("width", 0u);
        report.height = json.value("height", 0u);
        report.layout = json.value("layout", std::string());
        report.pointer = json.value("pointer", std::string());
        if (json.contains("test") && !json["test"].is_null()) {
            report.test = json["test"].dump();
        }
        if (json.contains("latencyMs") && json["latencyMs"].is_number()) {
            report.latencyMs = json["latencyMs"].get<double>();
        }
        if (json.contains("bars") && json["bars"].is_array()) {
            for (const auto& bar : json["bars"]) {
                if (bar.is_array() && bar.size() == 3 && bar[0].is_number() && bar[1].is_number() && bar[2].is_number()) {
                    report.patternBars.push_back({bar[0].get<int>(), bar[1].get<int>(), bar[2].get<int>()});
                }
            }
        }
        // Whether the page that reports is the watcher, or was dropped (it connects again), or another page took its place (it stops).
        uint64_t session = sender.session();
        uint64_t reporting = json.value("session", (uint64_t) 0);
        Json answer;
        answer["watcher"] = session == 0 ? "none" : (session == reporting ? "you" : "another");
        {
            std::lock_guard lock(mutex);
            // A page that another replaced goes on reporting that it stopped: what the record shows is the watcher's report, so only
            // the watcher's is kept, or anyone's while there is no watcher.
            if (session == 0 || session == reporting) {
                live.report = std::move(report);
            }
            if (session != 0 && session == reporting) {
                live.watcherReportNs = profile::now();
            }
            answer["source"] = live.pattern ? "pattern" : "frame";
            answer["grab"] = live.grab.has_value() && !live.grabResult.has_value() && (session == 0 || session == reporting);
            answer["control"] = live.control;
        }
        return {.status = 200, .contentType = "application/json", .body = answer.dump()};
    }
    if (request.method == "POST" && request.path == "/frame") {
        std::string path;
        {
            std::lock_guard lock(mutex);
            if (!live.grab || live.grabResult) {
                return {.status = 404, .body = "no frame was asked for\n"};
            }
            path = live.grab->path;
        }
        std::expected<void, std::string> result;
        std::ofstream out(path, std::ios::binary);
        out.write(request.body.data(), (std::streamsize) request.body.size());
        out.close();
        if (!out) {
            result = std::unexpected(std::format("cannot write {}", path));
        }
        std::lock_guard lock(mutex);
        live.grabResult = result;
        return {.status = 204};
    }
    return {.status = 404, .body = "not found\n"};
}
