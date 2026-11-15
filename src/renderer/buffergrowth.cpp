#include "buffergrowth.h"

#include "log.h"
#include "profile.h"
#include "rendercvars.h"
#include "rhidevice.h"

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

BufferGrowth::State::State(std::vector<GrowthBufferDesc> buffers) : descs(std::move(buffers)) {
    for (const auto& desc : descs) {
        names.emplace_back(desc.desc.debugName != nullptr ? desc.desc.debugName : "");
    }
}

auto BufferGrowth::complete(const std::vector<GrownBuffer>& made) -> bool {
    return std::ranges::all_of(made, [](const GrownBuffer& buffer) { return buffer.buffer != nullptr; });
}

auto BufferGrowth::destroyBuffers(RhiDevice* device, std::vector<GrownBuffer>& made) -> void {
    for (auto& buffer : made) {
        if (buffer.buffer != nullptr) {
            device->destroyBuffer(buffer.buffer);
        }
    }
    made.clear();
}

auto BufferGrowth::failed(uint64_t needed, uint64_t capacity, uint64_t wanted) -> void {
    // The reason is in the log, from the device. Starting again at once would ask it for the same buffers every frame.
    failedFor = std::max<uint64_t>(needed, 1);
    LOG_ERROR("Render", "BufferGrowthFailed", status.name)
        .text(std::format("{}: the buffers for {} couldn't all be made; it stays at {} until more than {} is needed", status.name, wanted, capacity, failedFor))
        .field("capacity", (int64_t) capacity)
        .field("wanted", (int64_t) wanted);
}

auto BufferGrowth::quiet(uint64_t needed, uint64_t capacity) const -> bool {
    if (state != nullptr || waiting) {
        return false;
    }
    if (policy == Behind::Sized) {
        return needed == 0 || needed == capacity;
    }
    if (needed > capacity) {
        return false;
    }
    auto share = ahead();
    bool startsAhead = aheadOn && share > 0.0 && needed > failedFor && (double) needed > (double) capacity * share;
    return !startsAhead;
}

namespace rendercvars {

cvar::Float growthAhead{"render.growth.ahead",
                        0.75,
                        "How full a GPU table (instances, draw lists, LOD groups, the geometry pool, meshlet work) gets before its next, larger "
                        "buffers are made on a background job, ready before it fills. 0: grow only when full, on the render thread.",
                        {.min = 0.0, .max = 1.0}};

} // namespace rendercvars

auto BufferGrowth::makeNow(RhiDevice* device, const std::vector<GrowthBufferDesc>& descs) -> std::vector<GrownBuffer> {
    State now(descs);
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
// kept per frame context (draw records, meshlet work) pay three times over.
auto BufferGrowth::step(uint64_t capacity) -> uint64_t {
    return capacity + std::max<uint64_t>(capacity / 2, 1);
}

auto BufferGrowth::updateNow(RhiDevice* device, uint64_t needed, uint64_t capacity, const std::function<uint64_t(uint64_t)>& grownCapacity, const Describe& describe, const Install& install)
    -> bool {
    bool replaced = false;
    if (state != nullptr && fence.ready()) {
        auto taken = std::move(state);
        state = nullptr;
        fence = {};
        auto tookMs = (double) (profile::now() - startedNs) * 1e-6;
        if (!complete(taken->made)) {
            // The device couldn't make one of them: keep the buffers there are.
            destroyBuffers(device, taken->made);
            failed(needed, capacity, growingCapacity);
        } else if (growingCapacity > capacity || (policy == Behind::Sized && growingCapacity != capacity)) {
            install(growingCapacity, taken->made);
            LOG_EVENT("Render", "BufferGrown", status.name)
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
            destroyBuffers(device, taken->made);
        }
        growingCapacity = 0;
    }
    auto start = [&](uint64_t grown) {
        startedFor = needed;
        auto next = std::make_shared<State>(describe(grown));
        auto locationId = profile::registerSourceLocation(status.name, profile::Group::Upload);
        fence = JobSystem::submit(
            [device, next, locationId] {
                profile::ScopedZone zone(locationId);
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
            LOG_EVENT("Render", "GrowthOnNeed", status.name)
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
            // Small enough to make here without a spike. One that failed isn't made again each frame.
            if (needed > failedFor) {
                PROFILE_ZONE("GrowNow", Upload);
                PROFILE_ZONE_VALUE(grown);
                auto made = makeNow(device, describe(grown));
                if (complete(made)) {
                    install(grown, made);
                    capacity = grown;
                    failedFor = 0;
                    status.grownNow++;
                    replaced = true;
                } else {
                    destroyBuffers(device, made);
                    failed(needed, capacity, grown);
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
                LOG_EVENT("Render", "GrowthOwnBuffer", status.name)
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
                LOG_WARNING("Render", "GrowthBehind", status.name)
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
        LOG_EVENT("Render", "GrowthReady", status.name)
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
    destroyBuffers(device, state->made);
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
