#include "viewpicking.h"

#include <algorithm>
#include <cstring>

auto ViewPicking::add(const char* resource, uint32_t x, uint32_t y, std::function<void(const CaptureResult&)> done) -> void {
    CaptureWatch watch = {
        .id = nextId++,
        .pass = "GeometryPass",
        .resource = resource,
        .trigger = 1,
        .regionX = x,
        .regionY = y,
        .regionWidth = 1,
        .regionHeight = 1,
    };
    pending.push_back({.watch = watch, .done = std::move(done)});
}

auto ViewPicking::request(uint32_t x, uint32_t y, std::function<void(std::optional<uint32_t> instance)> done) -> void {
    add("gbuffer.instance", x, y, [done = std::move(done)](const CaptureResult& result) {
        std::optional<uint32_t> instance;
        if (result.error.empty() && result.bytes && result.bytes->size() >= 4) {
            uint32_t value = 0;
            std::memcpy(&value, result.bytes->data(), 4);
            if (value != 0) {
                instance = value - 1;
            }
        }
        done(instance);
    });
}

auto ViewPicking::requestDepth(uint32_t x, uint32_t y, std::function<void(std::optional<float> depth)> done) -> void {
    add("depth", x, y, [done = std::move(done)](const CaptureResult& result) {
        std::optional<float> depth;
        if (result.error.empty() && result.bytes && result.bytes->size() >= 4) {
            float value = 0.0f;
            std::memcpy(&value, result.bytes->data(), 4);
            if (value > 0.0f) {
                depth = value;
            }
        }
        done(depth);
    });
}

auto ViewPicking::appendWatches(std::vector<CaptureWatch>& watches) const -> void {
    for (const auto& pick : pending) {
        watches.push_back(pick.watch);
    }
}

auto ViewPicking::onCaptureResult(const CaptureResult& result) -> bool {
    auto pick = std::ranges::find_if(pending, [&](const Pending& p) { return p.watch.id == result.id; });
    if (pick == pending.end()) {
        return false;
    }
    auto done = std::move(pick->done);
    pending.erase(pick);
    done(result);
    return true;
}
