#pragma once

#include "videotypes.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// What the watch page last told the engine about itself: what the browser decoded, and what it read off the test pattern.
struct RemoteViewReport {
    bool received = false;
    uint64_t receivedNs = 0; // profile::now()
    std::string agent;       // the browser's user agent
    std::string state;       // the page's connection state
    uint64_t framesDecoded = 0;
    uint64_t framesDropped = 0;
    double framesPerSecond = 0.0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::optional<double> latencyMs;             // capture to display, with the pattern as the source
    std::vector<std::array<int, 3>> patternBars; // the pattern's colour bars as the browser shows them
};

// The remote view as the main thread sees it, refreshed by RemoteView::update: the `remoteview` record and the Remote View window.
struct RemoteViewStatus {
    bool enabled = false;
    bool listening = false;
    std::string source;
    std::string address;
    uint16_t port = 0;
    std::string url;               // the watch page; with 0.0.0.0 as the address, at this machine's first address
    std::vector<std::string> urls; // the watch page at every address it listens on

    VideoWatcherState watcher = VideoWatcherState::None;
    std::string watcherAddress;
    uint64_t connects = 0;
    uint64_t disconnects = 0;

    // The video as it is encoded now; zero until a frame has been.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t framesPerSecond = 0;
    uint32_t bitrate = 0;
    std::string encoder;

    // Since the remote view was turned on.
    uint64_t captured = 0; // images handed over by the renderer, or patterns drawn
    uint64_t replaced = 0; // images a newer one replaced before the encoder took them
    uint64_t encoded = 0;
    uint64_t sent = 0;
    uint64_t sentBytes = 0;
    uint64_t keyframes = 0;
    uint64_t keyframeRequests = 0;

    // Means over the last second with frames, in milliseconds.
    double copyMs = 0.0;    // the render thread's copy out of the readback buffer
    double convertMs = 0.0; // RGBA to Y'CbCr
    double encodeMs = 0.0;
    double captureToSendMs = 0.0; // from the frame being recorded to its packet handed to the network

    RemoteViewReport report;
    std::string lastError;
};
