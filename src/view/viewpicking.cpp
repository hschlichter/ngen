#include "viewpicking.h"

#include <algorithm>
#include <cstring>

auto ViewPicking::request(uint32_t x, uint32_t y, std::function<void(std::optional<uint32_t> instance)> done) -> void {
    CaptureWatch watch = {
        .id = nextId++,
        .pass = "GeometryPass",
        .resource = "gbuffer.instance",
        .trigger = 1,
        .regionX = x,
        .regionY = y,
        .regionWidth = 1,
        .regionHeight = 1,
    };
    pending.push_back({.watch = watch, .done = std::move(done)});
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
    std::optional<uint32_t> instance;
    if (result.error.empty() && result.bytes && result.bytes->size() >= 4) {
        uint32_t value = 0;
        std::memcpy(&value, result.bytes->data(), 4);
        if (value != 0) {
            instance = value - 1;
        }
    }
    auto done = std::move(pick->done);
    pending.erase(pick);
    done(instance);
    return true;
}
