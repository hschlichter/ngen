#include "buffergrowth.h"

#include "profile.h"
#include "renderconfig.h"
#include "rhidevice.h"
#include "trace.h"

#include <algorithm>
#include <format>

BufferGrowth::~BufferGrowth() {
    // The growable discards a growth in flight before it goes; one left is waited for, so its job doesn't outlive what it writes.
    if (state != nullptr) {
        JobSystem::wait(fence);
    }
}

auto BufferGrowth::make(RhiDevice* device, State& state) -> void {
    state.made.resize(state.descs.size());
    for (size_t i = 0; i < state.descs.size(); i++) {
        auto desc = state.descs[i].desc;
        desc.debugName = state.names[i].c_str();
        auto* buffer = device->createBuffer(desc);
        state.made[i].buffer = buffer;
        if (buffer != nullptr && state.descs[i].mapped) {
            state.made[i].mapped = device->mapBuffer(buffer);
        }
    }
}

auto BufferGrowth::destroyMade(RhiDevice* device, State& state) -> void {
    for (auto& made : state.made) {
        if (made.buffer != nullptr) {
            device->destroyBuffer(made.buffer);
        }
    }
    state.made.clear();
}

namespace renderconfig {

config::Float growthAhead{"render.growth.ahead",
                          0.75,
                          "How full a GPU table (instances, draw lists, LOD groups, the geometry pool, meshlet work) gets before its next, larger "
                          "buffers are made on a background job, ready before it fills. 0: grow only when full, on the render thread.",
                          {.min = 0.0, .max = 1.0}};

} // namespace renderconfig

auto BufferGrowth::makeNow(RhiDevice* device, const std::vector<GrowthBufferDesc>& descs) -> std::vector<GrownBuffer> {
    State now;
    now.descs = descs;
    for (const auto& desc : now.descs) {
        now.names.emplace_back(desc.desc.debugName != nullptr ? desc.desc.debugName : "");
    }
    make(device, now);
    return std::move(now.made);
}

auto BufferGrowth::setAhead(double share) -> void {
    aheadShare.store(share, std::memory_order_relaxed);
}

auto BufferGrowth::ahead() -> double {
    return aheadShare.load(std::memory_order_relaxed);
}

// The next capacity at least: half as much again. Doubling would hold twice the memory a table needs right after it grows, which the tables
// kept per frame slot (draw records, meshlet work) pay three times over.
auto BufferGrowth::step(uint64_t capacity) -> uint64_t {
    return capacity + std::max<uint64_t>(capacity / 2, 1);
}

auto BufferGrowth::update(RhiDevice* device, uint64_t needed, uint64_t capacity, const std::function<uint64_t(uint64_t)>& grownCapacity, const Describe& describe, const Install& install)
    -> bool {
    bool replaced = false;
    if (state != nullptr && fence.ready()) {
        auto taken = std::move(state);
        state = nullptr;
        fence = {};
        auto tookMs = (double) (profile::now() - startedNs) * 1e-6;
        bool complete = std::ranges::all_of(taken->made, [](const GrownBuffer& made) { return made.buffer != nullptr; });
        if (!complete) {
            // The device couldn't make one of them (the reason is in the trace): keep the buffers there are, and try again once more is
            // needed than now. Starting again at once would ask the device for the same buffers every time the job ends.
            destroyMade(device, *taken);
            failedFor = std::max<uint64_t>(needed, 1);
            TRACE_ERROR("Render", "BufferGrowthFailed", status.name)
                .text(std::format("{}: the buffers for {} couldn't all be made; it stays at {} until more than {} is needed", status.name, growingCapacity, capacity, failedFor))
                .field("capacity", (int64_t) capacity)
                .field("wanted", (int64_t) growingCapacity);
        } else if (growingCapacity > capacity || (policy == Behind::Sized && growingCapacity != capacity)) {
            install(growingCapacity, taken->made);
            TRACE_EVENT("Render", "BufferGrown", status.name)
                .text(std::format("{} {} from {} to {} on a background job, started when {} were needed, ready {:.1f} ms after",
                                  status.name,
                                  growingCapacity > capacity ? "grown" : "made smaller",
                                  capacity,
                                  growingCapacity,
                                  startedFor,
                                  tookMs))
                .field("startedFor", (int64_t) startedFor)
                .field("capacity", (int64_t) growingCapacity)
                .field("before", (int64_t) capacity)
                .field("ms", tookMs);
            capacity = growingCapacity;
            failedFor = 0;
            status.grownAhead++;
            replaced = true;
        } else {
            // A growth on this thread got there first and is larger.
            destroyMade(device, *taken);
        }
        growingCapacity = 0;
    }
    auto start = [&](uint64_t grown) {
        startedFor = needed;
        auto next = std::make_shared<State>();
        next->descs = describe(grown);
        for (const auto& desc : next->descs) {
            next->names.emplace_back(desc.desc.debugName != nullptr ? desc.desc.debugName : "");
        }
        auto siteId = profile::registerSite(status.name, profile::Group::Upload);
        fence = JobSystem::submit(
            [device, next, siteId] {
                profile::ScopedZone zone(siteId);
                make(device, *next);
            },
            "BufferGrowth",
            JobKind::Background);
        state = std::move(next);
        growingCapacity = grown;
        startedNs = profile::now();
    };
    // After a failed growth, the next starts only once more is needed than when it failed.
    bool mayStart = state == nullptr && needed > failedFor;
    auto share = ahead();
    if (policy == Behind::Sized) {
        // Made at the size needed, larger or smaller; none while nothing is needed. One in flight for another size lands first.
        auto sized = grownCapacity(needed);
        if (state == nullptr && sized != 0 && sized != capacity && needed != failedFor) {
            start(sized);
        }
        if (needed > capacity && !waiting) {
            // How it works, not a table that fell behind: no warning.
            waiting = true;
            waitingSinceNs = profile::now();
            status.waits++;
            TRACE_EVENT("Render", "GrowthOnNeed", status.name)
                .text(std::format("{} needs {} and has {}: a buffer for {} is being made on a background job, and what needs it waits", status.name, needed, capacity, growingCapacity))
                .field("needed", (int64_t) needed)
                .field("capacity", (int64_t) capacity)
                .field("growing", (int64_t) growingCapacity);
        }
    } else if (needed > capacity) {
        // Room for `needed` with the headroom growing ahead keeps, so the growth isn't full (and growing again) the frame it lands.
        auto roomy = share > 0.0 && aheadOn ? (uint64_t) ((double) needed / share) + 1 : needed;
        auto grown = grownCapacity(std::max(policy == Behind::GrowNow ? needed : roomy, step(capacity)));
        if (grown > capacity && policy == Behind::GrowNow) {
            // Small enough to make here without a spike.
            PROFILE_ZONE("GrowNow", Upload);
            PROFILE_ZONE_VALUE(grown);
            auto made = makeNow(device, describe(grown));
            if (std::ranges::all_of(made, [](const GrownBuffer& buffer) { return buffer.buffer != nullptr; })) {
                install(grown, made);
                capacity = grown;
                status.grownNow++;
                replaced = true;
            } else {
                for (auto& buffer : made) {
                    if (buffer.buffer != nullptr) {
                        device->destroyBuffer(buffer.buffer);
                    }
                }
            }
        } else if (grown > capacity && policy == Behind::OwnBuffer) {
            // This frame makes do with a buffer of its own; the frames after get the growth.
            if (mayStart) {
                start(grown);
                mayStart = false;
            }
            // Once for the run of frames that need more, as a table that waits is reported once.
            if (!waiting) {
                waiting = true;
                waitingSinceNs = profile::now();
                status.waits++;
                TRACE_EVENT("Render", "GrowthOwnBuffer", status.name)
                    .text(std::format("{} needs {}, past its capacity of {}: its frames make a buffer of their own until it fits, and a growth to {} is on its way",
                                      status.name,
                                      needed,
                                      capacity,
                                      growingCapacity))
                    .field("needed", (int64_t) needed)
                    .field("capacity", (int64_t) capacity)
                    .field("growing", (int64_t) growingCapacity);
            }
        } else if (grown > capacity) {
            // What doesn't fit waits for the growth: the frame goes on without it.
            if (mayStart) {
                start(grown);
                mayStart = false;
            }
            if (!waiting) {
                waiting = true;
                waitingSinceNs = profile::now();
                status.waits++;
                TRACE_WARNING("Render", "GrowthBehind", status.name)
                    .text(std::format("{} needs {}, past its capacity of {}, before its growth (to {}) is ready: what doesn't fit waits for it", status.name, needed, capacity, growingCapacity))
                    .field("needed", (int64_t) needed)
                    .field("capacity", (int64_t) capacity)
                    .field("growing", (int64_t) growingCapacity);
            }
        }
    }
    if (waiting && needed <= capacity) {
        waiting = false;
        status.lastWaitMs = (double) (profile::now() - waitingSinceNs) * 1e-6;
        TRACE_EVENT("Render", "GrowthReady", status.name)
            .text(std::format("{} holds what it needs again ({} of {}), after {:.1f} ms {}",
                              status.name,
                              needed,
                              capacity,
                              status.lastWaitMs,
                              policy == Behind::OwnBuffer ? "of frames making their own buffer" : "of waiting"))
            .field("needed", (int64_t) needed)
            .field("capacity", (int64_t) capacity)
            .field("ms", status.lastWaitMs);
    }
    if (mayStart && aheadOn && share > 0.0 && (double) needed > (double) capacity * share) {
        auto grown = grownCapacity(std::max((uint64_t) ((double) needed / share) + 1, step(capacity)));
        if (grown > capacity) {
            start(grown);
        }
    }
    return replaced;
}

auto BufferGrowth::discard(RhiDevice* device) -> void {
    if (state == nullptr) {
        return;
    }
    JobSystem::wait(fence);
    destroyMade(device, *state);
    state = nullptr;
    fence = {};
    growingCapacity = 0;
}

auto BufferGrowth::statusOf(uint64_t capacity, uint64_t used) const -> GrowthStatus {
    auto out = status;
    out.capacity = capacity;
    out.used = used;
    out.growing = growingCapacity;
    out.growingMs = state != nullptr ? (double) (profile::now() - startedNs) * 1e-6 : 0.0;
    out.waiting = waiting;
    return out;
}
