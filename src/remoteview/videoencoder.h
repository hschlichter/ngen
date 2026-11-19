#pragma once

#include "videotypes.h"

#include <expected>
#include <memory>
#include <string>

// An H.264 encoder that takes pictures from the CPU. One implementation, OpenH264 in software; a hardware encoder fed from memory
// (VA-API) would be a second. An encoder that reads the GPU's image itself replaces capture and this together, and hands VideoPackets on.
class VideoEncoder {
public:
    VideoEncoder() = default;
    VideoEncoder(const VideoEncoder&) = delete;
    auto operator=(const VideoEncoder&) -> VideoEncoder& = delete;
    virtual ~VideoEncoder() = default;

    virtual auto name() const -> const char* = 0;
    // Ready for pictures of the settings' size. Starting a started encoder restarts it.
    virtual auto start(const VideoSettings& settings) -> std::expected<void, std::string> = 0;
    // `keyframe`: the watcher has nothing to decode against (it just connected, or it asked). An empty packet means the encoder
    // made nothing of this picture.
    virtual auto encode(const VideoPlanes& planes, bool keyframe, VideoPacket& packet) -> std::expected<void, std::string> = 0;
    virtual auto stop() -> void = 0;
};

auto makeVideoEncoderOpenH264() -> std::unique_ptr<VideoEncoder>;
