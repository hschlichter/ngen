#pragma once

#include "jobsystem.h"
#include "rhitypes.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class RhiBuffer;
class RhiDevice;

// A buffer a growth makes: its description, and whether it is mapped once made.
struct GrowthBufferDesc {
    RhiBufferDesc desc;
    bool mapped = false;
};

struct GrownBuffer {
    RhiBuffer* buffer = nullptr;
    void* mapped = nullptr;
};

// What the gpuscene record shows of a growable.
struct GrowthStatus {
    const char* name = "";
    uint64_t capacity = 0;
    uint64_t used = 0;
    uint64_t growing = 0;    // the capacity a growth in flight makes; 0 none
    double growingMs = 0.0;  // how long it has been in flight
    bool waiting = false;    // it needs more than its capacity: what doesn't fit waits for the growth
    uint64_t grownAhead = 0; // growths taken from a background job
    uint64_t grownNow = 0;   // growths made on the render thread (Behind::GrowNow)
    uint64_t waits = 0;      // times it needed more than it had before a growth was ready
    double lastWaitMs = 0.0; // how long the last of those lasted
};

// One growable's next, larger buffers (src/renderer/README.md, "Growth"). A table that has filled past ahead() of its capacity starts its
// growth: a background job (src/jobs/README.md) makes and maps the buffers while frames go on, and the render thread swaps them in the frame
// it finds them ready. A table that needs more than its capacity before then waits for that growth (GrowthBehind, then GrowthReady): what
// doesn't fit isn't drawn until it comes, and the render thread never makes the large buffers itself. A small table (Behind::GrowNow) grows
// on the render thread instead, where that costs less than a frame's share and waiting would need every reader to bound its reads.
class BufferGrowth {
public:
    enum class Behind : uint8_t {
        Wait,      // what doesn't fit waits for the growth
        GrowNow,   // grown on the render thread: small enough not to be a spike
        OwnBuffer, // the frame that needs more makes a buffer of its own (staging): the growth is for the frames after
    };

    // The buffers for a capacity, in the order install receives them.
    using Describe = std::function<std::vector<GrowthBufferDesc>(uint64_t capacity)>;
    // Swaps in buffers made for `capacity`; the growable retires its old ones.
    using Install = std::function<void(uint64_t capacity, std::vector<GrownBuffer>& buffers)>;

    // `growsAhead` false: a buffer sized to a setting (the upload ring) grows only when it needs to.
    explicit BufferGrowth(const char* name, Behind behind = Behind::Wait, bool growsAhead = true) : policy(behind), aheadOn(growsAhead), status{.name = name} {}
    BufferGrowth(const BufferGrowth&) = delete;
    auto operator=(const BufferGrowth&) -> BufferGrowth& = delete;
    ~BufferGrowth();

    // Each frame, before the growable is used: takes a finished growth; when `needed` is over the capacity, starts a growth and waits for it
    // (or grows now, Behind::GrowNow); and starts the next growth once `needed` passes ahead() of the capacity. `grownCapacity(needed)` is
    // the capacity a growth makes. Returns true when the buffers were replaced.
    auto update(RhiDevice* device, uint64_t needed, uint64_t capacity, const std::function<uint64_t(uint64_t)>& grownCapacity, const Describe& describe, const Install& install)
        -> bool;

    // A growable's first buffers, made on this thread (at init, or when it is first used): not a growth.
    static auto makeNow(RhiDevice* device, const std::vector<GrowthBufferDesc>& descs) -> std::vector<GrownBuffer>;

    // The share of its capacity a growable fills before its growth starts (render.growth.ahead, handed in by the main loop); 0 grows only
    // when needed, on the render thread. Any thread.
    static auto setAhead(double share) -> void;
    static auto ahead() -> double;

    // Destroys what a growth in flight makes, waiting for it: for the growable's destroy, never in a frame.
    auto discard(RhiDevice* device) -> void;

    auto inFlight() const -> bool { return state != nullptr; }
    auto isWaiting() const -> bool { return waiting; }
    auto statusOf(uint64_t capacity, uint64_t used) const -> GrowthStatus;

private:
    struct State {
        std::vector<GrowthBufferDesc> descs;
        std::vector<std::string> names;
        std::vector<GrownBuffer> made;
    };

    static auto make(RhiDevice* device, State& state) -> void;
    static auto step(uint64_t capacity) -> uint64_t;
    static inline std::atomic<double> aheadShare = 0.75;
    static auto destroyMade(RhiDevice* device, State& state) -> void;

    Behind policy;
    bool aheadOn;
    std::shared_ptr<State> state; // the growth in flight
    JobFence fence;
    uint64_t growingCapacity = 0;
    uint64_t startedNs = 0;
    uint64_t startedFor = 0; // what was needed when the growth in flight started
    bool waiting = false;
    uint64_t waitingSinceNs = 0;
    GrowthStatus status;
};
