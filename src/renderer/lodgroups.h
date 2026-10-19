#pragma once

#include "framegraphresource.h"
#include "renderworld.h"

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

class DeletionQueue;
class FrameGraph;
class RhiBuffer;
class RhiDevice;

// One entry of the render-LOD group table (std430, stride 64), read by shaders/lodselect.comp. serial changes each time the slot takes a
// new group, so the level picked for the group before isn't taken as this one's.
struct GpuLodGroup {
    glm::vec3 center = glm::vec3(0.0f);
    float radius = 0.0f;
    std::array<float, RenderLodGroup::maxLevels - 1> thresholds = {};
    uint32_t levelCount = 0;
    uint32_t defaultLevel = 0;
    int32_t forcedLevel = -1;
    uint32_t serial = 0;
    uint32_t alive = 0;
};
static_assert(sizeof(GpuLodGroup) == 64);

// What shaders/lodselect.comp counts over the groups each frame (its Totals, std430).
struct LodSelectTotals {
    std::array<uint32_t, RenderLodGroup::maxLevels> picked = {}; // groups drawing each level
    uint32_t fallbacks = 0;                                      // groups drawing another level while the one wanted isn't resident
    uint32_t forced = 0;                                         // groups at a hand-set level or the default, the heuristic off
    uint32_t groups = 0;                                         // alive
    uint32_t pad = 0;
};
static_assert(sizeof(LodSelectTotals) == 48);

// The render-LOD groups on the GPU (src/renderer/README.md, "Render LOD"): the group table, a record per group slot uploaded as groups
// change; the state, per group the serial it was picked for, the level the heuristic wants and the level drawn, which lodselect.comp writes
// each frame and the cull pass reads; and the residency mask, per group a bit per level with a mesh not in the pool, which the cull pass
// sets and lodselect.comp reads and clears the next frame. The three persist across frames; a grown one is new, its contents undefined
// until written: the state is written before anything reads it, and stale residency bits only make a group fall back for a frame.
// Per frame slot: the per-workgroup counters, the totals and the readback.
class LodGroups {
public:
    static constexpr uint32_t groupSize = 256;
    static constexpr uint32_t counterCount = sizeof(LodSelectTotals) / sizeof(uint32_t);

    auto init(RhiDevice* device, uint32_t frameSlots, DeletionQueue* deletionQueue) -> void;
    auto destroy() -> void;

    // Applies a change list's groups: the table grows to `count` slots and the changed slots are uploaded at the next import. True when a
    // buffer was replaced, so the descriptor sets naming it are rewritten.
    auto apply(std::span<const std::pair<uint32_t, RenderLodGroup>> changes, uint32_t count, uint64_t frame) -> bool;
    auto groupCount() const -> uint32_t { return (uint32_t) groups.size(); }
    auto group(uint32_t slot) const -> const RenderLodGroup& { return groups[slot]; }
    static auto workgroupsFor(uint32_t groupCount) -> uint32_t;

    struct Handles {
        FgBufferHandle table;
        FgBufferHandle states;
        FgBufferHandle residency;
        FgBufferHandle groupCounters;
        FgBufferHandle totals;
        FgBufferHandle readback;
    };
    // Imports the persistent buffers with the access the last frame left them in and the slot's; adds LodGroupUpload when groups changed.
    auto import(FrameGraph& fg, uint32_t frameSlot) -> Handles;
    // Copies the totals, and with `states` every group's state, into the slot's readback.
    auto addReadbackPass(FrameGraph& fg, const Handles& handles, uint32_t frameSlot, bool states) -> void;
    // Once the slot's fence has passed: what that slot's frame read back.
    auto parseReadback(uint32_t frameSlot, uint64_t slotFrame) -> void;
    // Stores the access the persistent buffers are left in, for the next frame's import.
    auto afterExecute(const FrameGraph& fg) -> void;

    auto tableBuffer() const -> RhiBuffer* { return table; }
    auto stateBuffer() const -> RhiBuffer* { return states; }
    auto residencyBuffer() const -> RhiBuffer* { return residency; }
    auto groupCountersBuffer(uint32_t frameSlot) const -> RhiBuffer* { return slots[frameSlot].groupCounters; }
    auto totalsBuffer(uint32_t frameSlot) const -> RhiBuffer* { return slots[frameSlot].totals; }
    auto frameSlots() const -> uint32_t { return (uint32_t) slots.size(); }
    auto capacityBytes() const -> uint64_t { return (uint64_t) capacity * (sizeof(GpuLodGroup) + (3 * sizeof(uint32_t))); }

    // The latest readback (one frame-slot cycle old): the totals, and per group the level drawn and wanted (0xFF for none) when the states
    // were read.
    struct Readback {
        uint64_t frame = 0;
        uint32_t groupCount = 0;
        LodSelectTotals totals;
        bool hasStates = false;
        std::vector<uint8_t> drawn;
        std::vector<uint8_t> wanted;
    };
    auto latest() const -> const Readback& { return read; }

private:
    struct Slot {
        RhiBuffer* staging = nullptr; // CpuToGpu, mapped: the group records to upload
        void* stagingMapped = nullptr;
        RhiBuffer* groupCounters = nullptr;
        RhiBuffer* totals = nullptr;
        RhiBuffer* readback = nullptr; // GpuToCpu, mapped: totals, then states
        void* readbackMapped = nullptr;
        bool pending = false;
        bool readStates = false;
        uint32_t groupCount = 0;
    };
    auto ensureCapacity(uint32_t count, uint64_t frame) -> bool;
    auto record(uint32_t slot) const -> GpuLodGroup;

    RhiDevice* device = nullptr;
    DeletionQueue* deletionQueue = nullptr;
    uint32_t capacity = 0;
    std::vector<RenderLodGroup> groups; // by group slot, as the change lists left them
    std::vector<uint32_t> serials;      // by group slot
    RhiBuffer* table = nullptr;
    RhiBuffer* states = nullptr;
    RhiBuffer* residency = nullptr;
    std::array<FgAccessFlags, 3> access = {FgAccessFlags::None, FgAccessFlags::None, FgAccessFlags::None};
    std::array<FgBufferHandle, 3> imported;
    bool importedThisFrame = false;
    uint32_t dirtyFirst = 0;
    uint32_t dirtyEnd = 0;
    std::vector<Slot> slots;
    Readback read;
};
