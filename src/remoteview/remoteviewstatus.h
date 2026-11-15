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
    std::string layout;                          // the layout the page chose: narrow, wide, landscape
    std::string pointer;                         // the page's main pointer: coarse (touch) or fine (mouse)
    std::string test;                            // the page's self-test, as JSON; empty when it ran none
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
    // The frame the video is scaled from, in pixels: what view.pick's coordinates are in. Zero until a frame was captured.
    uint32_t frameWidth = 0;
    uint32_t frameHeight = 0;

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

// Remote control as the main thread sees it, refreshed by RemoteControl::drain: the `remoteview` record's `control` and the Remote
// View window.
struct RemoteControlStatus {
    bool enabled = false;       // remoteview.control
    bool open = false;          // the watcher's control channel is up
    uint64_t calls = 0;         // calls run since the view started
    uint64_t failed = 0;        // of those, the ones answered with an error
    uint64_t tooLarge = 0;      // answers that were over the channel's message limit
    uint64_t dropped = 0;       // messages dropped because too many were waiting
    uint64_t messageLimit = 0;  // the largest message the watcher takes, in bytes
    std::string lastMethod;     // the last method called
    bool logSubscribed = false; // the watcher asked for the log
    uint64_t logEvents = 0;     // log events sent
};
