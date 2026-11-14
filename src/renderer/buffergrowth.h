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
    bool waiting = false;    // it needs more than its capacity: what doesn't fit waits for the growth (Behind::OwnBuffer: its frames make their own)
    uint64_t grownAhead = 0; // growths taken from a background job
    uint64_t grownNow = 0;   // growths made on the render thread (Behind::GrowNow)
    uint64_t waits = 0;      // times it needed more than it had before a growth was ready: runs of frames, not frames
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
        Sized,     // a buffer at the size needed (a setting, one large upload): made larger or smaller when that changes, never ahead; what
                   // needs it waits, without a warning (GrowthOnNeed)
    };

    // The buffers for a capacity, in the order install receives them.
    using Describe = std::function<std::vector<GrowthBufferDesc>(uint64_t capacity)>;
    // Swaps in buffers made for `capacity`; the growable retires its old ones.
    using Install = std::function<void(uint64_t capacity, std::vector<GrownBuffer>& buffers)>;

    explicit BufferGrowth(const char* name, Behind behind = Behind::Wait) : policy(behind), aheadOn(behind != Behind::Sized), status{.name = name} {}
    BufferGrowth(const BufferGrowth&) = delete;
    auto operator=(const BufferGrowth&) -> BufferGrowth& = delete;
    ~BufferGrowth();

    // Each frame, before the growable is used: takes a finished growth; when `needed` is over the capacity, starts a growth and waits for it
    // (or grows now, Behind::GrowNow); and starts the next growth once `needed` passes ahead() of the capacity. `grownCapacity(needed)` is
    // the capacity a growth makes. Behind::Sized: starts buffers for `grownCapacity(needed)` whenever that differs from the capacity and
    // isn't 0. Returns true when the buffers were replaced.
    // The callables are wrapped (std::function, which may allocate) only in a frame that has something to do: most frames return at quiet().
    template <typename GrownCapacityFn, typename DescribeFn, typename InstallFn>
    auto update(RhiDevice* device, uint64_t needed, uint64_t capacity, const GrownCapacityFn& grownCapacity, const DescribeFn& describe, const InstallFn& install) -> bool {
        if (quiet(needed, capacity)) {
            return false;
        }
        return updateNow(device, needed, capacity, grownCapacity, describe, install);
    }

    // True when update() has nothing to do: no growth in flight, nothing waiting, and `needed` neither over the capacity nor past the share
    // that starts the next growth (Behind::Sized: nothing needed, or the capacity is what is needed).
    auto quiet(uint64_t needed, uint64_t capacity) const -> bool;

    // A growable's first buffers, made on this thread (at init, or when it is first used): not a growth.
    static auto makeNow(RhiDevice* device, const std::vector<GrowthBufferDesc>& descs) -> std::vector<GrownBuffer>;
    // Whether the device made every buffer asked for, and the way out when it didn't: what was made is destroyed, never having been used.
    static auto complete(const std::vector<GrownBuffer>& made) -> bool;
    static auto destroyBuffers(RhiDevice* device, std::vector<GrownBuffer>& made) -> void;

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
        // The debug names are copied: the job that makes the buffers may outlive the strings the descriptions point to.
        explicit State(std::vector<GrowthBufferDesc> buffers);

        std::vector<GrowthBufferDesc> descs;
        std::vector<std::string> names;
        std::vector<GrownBuffer> made;
    };

    auto updateNow(RhiDevice* device, uint64_t needed, uint64_t capacity, const std::function<uint64_t(uint64_t)>& grownCapacity, const Describe& describe, const Install& install)
        -> bool;
    // The device couldn't make a growth's buffers: reported, and no growth starts again until more is needed than now.
    auto failed(uint64_t needed, uint64_t capacity, uint64_t wanted) -> void;
    static auto make(RhiDevice* device, State& state) -> void;
    static auto step(uint64_t capacity) -> uint64_t;
    static inline std::atomic<double> aheadShare = 0.75;

    Behind policy;
    bool aheadOn;
    std::shared_ptr<State> state; // the growth in flight
    JobFence fence;
    uint64_t growingCapacity = 0;
    uint64_t startedNs = 0;
    uint64_t startedFor = 0; // what was needed when the growth in flight started
    uint64_t failedFor = 0;  // what was needed when a growth last failed: no new one starts until more is; 0 none failed
    bool waiting = false;
    uint64_t waitingSinceNs = 0;
    GrowthStatus status;
};
