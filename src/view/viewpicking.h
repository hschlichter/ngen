#pragma once

#include "capture.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

// Picking on the GPU: the geometry pass writes each pixel's instance index + 1 into gbuffer.instance (0 where nothing is drawn); a pick
// copies the one texel under a pixel after that pass, through the capture service, and hands back the instance a few frames later.
class ViewPicking {
public:
    // A pick at a framebuffer pixel; `done` gets the instance index there, or nullopt for nothing drawn (or a failed readback).
    auto request(uint32_t x, uint32_t y, std::function<void(std::optional<uint32_t> instance)> done) -> void;

    // Per frame, from the main loop, as the other capture watches.
    auto appendWatches(std::vector<CaptureWatch>& watches) const -> void;
    // True when the result was a pick's.
    auto onCaptureResult(const CaptureResult& result) -> bool;

private:
    struct Pending {
        CaptureWatch watch;
        std::function<void(std::optional<uint32_t>)> done;
    };
    std::vector<Pending> pending;
    uint32_t nextId = pickWatchBase;

    static constexpr uint32_t pickWatchBase = 50000;
};
