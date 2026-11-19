#pragma once

#include "remoteviewstatus.h"
#include "videoencoder.h"
#include "videohttp.h"
#include "videosender.h"
#include "videotypes.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// The remote view (src/remoteview/README.md): ngen-view's image as live H.264 video for one browser on another device. It owns the
// listener that serves the watch page, the connection to the watcher and the thread that encodes. The main thread drives it with
// update once a frame; the render thread hands it images (RemoteViewCapture); nothing in a frame waits for it.
class RemoteView {
public:
    RemoteView();
    ~RemoteView();
    RemoteView(const RemoteView&) = delete;
    auto operator=(const RemoteView&) -> RemoteView& = delete;

    // Main thread, once a frame: reads the remoteview.* cvars, starts and stops the listener and the encode thread as they changed,
    // logs what happened since the last frame, and refreshes status().
    auto update() -> void;
    // Main thread: the height to capture the frame being built at, when a watcher is connected, the source is the frame and one is due.
    auto takeFrameDue() -> std::optional<uint32_t>;
    // Render thread: a captured image. It replaces one the encode thread hasn't taken yet. `copyMs` is what the copy cost.
    auto submit(VideoImage&& image, double copyMs) -> void;
    auto status() const -> const RemoteViewStatus& { return current; }

    // The next frame sent is a keyframe.
    auto requestKeyframe() -> void;
    // Drops the watcher. The watch page connects again by itself.
    auto disconnect() -> void;
    // Asks the watch page for the frame it is showing, as a PNG written to `path`. `done` runs on the main thread, in update: once the
    // file is written, or with why not (no watcher's page answered within five seconds).
    auto requestGrab(std::string path, std::function<void(std::expected<void, std::string>)> done) -> void;

    // The connection, for the remote control that shares it.
    auto videoSender() -> VideoSender& { return sender; }
    // A file for the watch page to fetch at /file/<name>: a screenshot it asked for. One at a time; the next replaces it.
    auto shareFile(std::string name, std::string contentType, std::string bytes) -> void;

    auto shutdown() -> void;

private:
    struct Applied {
        bool enabled = false;
        std::string address;
        uint16_t port = 0;
    };

    struct Grab {
        std::string path;
        std::function<void(std::expected<void, std::string>)> done;
        uint64_t askedNs = 0;
    };

    // What the threads share, under `mutex`.
    struct Live {
        VideoSettings settings; // width is unused: it follows the image
        bool pattern = false;
        std::string bindAddress;
        bool control = false;
        std::string pageDir;
        uint32_t frameWidth = 0;
        uint32_t frameHeight = 0;
        std::string fileName; // the shared file
        std::string fileType;
        std::string fileBytes;
        std::optional<VideoImage> pending;
        bool stop = false;
        bool restartEncoder = false;

        uint64_t captured = 0;
        uint64_t replaced = 0;
        uint64_t encoded = 0;
        uint64_t sent = 0;
        uint64_t sentBytes = 0;
        uint64_t keyframes = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        std::string encoder;
        double copyMs = 0.0;
        double convertMs = 0.0;
        double encodeMs = 0.0;
        double captureToSendMs = 0.0;
        double copySum = 0.0;
        uint32_t copyCount = 0;

        RemoteViewReport report;
        uint64_t watcherReportNs = 0; // when the page that is the watcher last reported
        std::string lastError;
        std::optional<Grab> grab;
        std::optional<std::expected<void, std::string>> grabResult;
        // What the threads have to say, logged by the main thread in update.
        std::vector<std::function<void()>> events;
    };

    auto startThread() -> void;
    auto stopThread() -> void;
    auto encodeLoop() -> void;
    auto serve(const VideoHttpRequest& request) -> VideoHttpResponse;
    auto fail(const std::string& message) -> void;

    Applied applied;
    RemoteViewStatus current;
    std::vector<std::string> watchPageUrls; // while listening
    uint64_t nextCaptureNs = 0;
    uint64_t seenConnects = 0;
    uint64_t seenDisconnects = 0;
    uint64_t connectedNs = 0;
    uint64_t sentAtConnect = 0;

    mutable std::mutex mutex;
    std::condition_variable wake;
    Live live;
    std::atomic<bool> keyframeAsked{false};

    VideoSender sender;
    VideoHttp http;
    std::thread encodeThread;
};
