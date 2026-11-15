#pragma once

#include <cstdint>
#include <vector>

// What moves between the remote view's parts (src/remoteview/README.md): an image as it was shown, the same image as the encoder takes
// it, and the encoded frame that is sent.

// One image on its way to the encoder: RGBA as displayed, top row first.
struct VideoImage {
    std::vector<uint8_t> rgba;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t captureNs = 0; // profile::now() when the frame that made it was recorded
    // The size of the frame it was scaled from, in pixels: what view.pick's coordinates are in. Zero for the test pattern.
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
};

// Planar Y'CbCr 4:2:0, BT.709, limited range. Width and height are even.
struct VideoPlanes {
    std::vector<uint8_t> y;
    std::vector<uint8_t> cb;
    std::vector<uint8_t> cr;
    uint32_t width = 0;
    uint32_t height = 0;
};

// One encoded frame: an H.264 access unit, its NAL units behind start codes.
struct VideoPacket {
    std::vector<uint8_t> data;
    bool keyframe = false;
    uint64_t captureNs = 0;
};

enum class VideoWatcherState : uint8_t {
    None,
    Connecting,
    Connected,
};

inline auto videoWatcherStateName(VideoWatcherState state) -> const char* {
    switch (state) {
        case VideoWatcherState::None:
            return "none";
        case VideoWatcherState::Connecting:
            return "connecting";
        case VideoWatcherState::Connected:
            return "connected";
    }
    return "unknown";
}

struct VideoSettings {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t framesPerSecond = 30;
    uint32_t bitrate = 4'000'000; // bits a second
};
