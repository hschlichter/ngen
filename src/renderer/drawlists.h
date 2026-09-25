#pragma once

#include "framegraphresource.h"
#include "gpuscene.h"
#include "rhitypes.h"
#include "shadowcascades.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// GPU culling result for the editor (Culling window, AABB overlay), from DrawLists' readback.
struct CullResult {
    uint64_t frame = 0; // 0: nothing read back yet
    uint32_t instances = 0;
    uint32_t cameraCulled = 0;
    uint32_t cascadeCount = 0;
    std::array<uint32_t, maxShadowCascades> cascadeCulled = {};
    std::array<uint32_t, maxShadowCascades> cascadeDrawn = {};
    std::vector<uint8_t> cameraVisible; // per instance, 1 = visible
};

class DeletionQueue;
class FrameGraph;
class FrameGraphContext;
class RhiDevice;

// Indirect draw commands per view and pipeline bucket (docs/plan_indirect_draws.md), written
// on the GPU by the culling passes (docs/plan_gpu_culling.md). One region per (view, bucket):
// the camera, then each shadow cascade, each split into single-sided and double-sided.
//
// Owns the frame slot's buffers the culling passes read and write, and the readback that
// brings counts, visibility and (while the render debugger is open) the commands back to
// the CPU once the slot's fence has passed. Everything read back is one frame-slot cycle old.
class DrawLists {
public:
    static constexpr uint32_t bucketCount = 2; // single-sided, double-sided
    static constexpr uint32_t viewCount = 1 + maxShadowCascades;
    static constexpr uint32_t regionCount = viewCount * bucketCount;
    // Counters the culling shader produces per workgroup and in total (instancecull.comp).
    static constexpr uint32_t counterDraws = 0;
    static constexpr uint32_t counterPrimitives = regionCount;
    static constexpr uint32_t counterCameraCulled = regionCount * 2;
    static constexpr uint32_t counterCascadeCulled = counterCameraCulled + 1;
    static constexpr uint32_t counterCascadeDrawn = counterCascadeCulled + maxShadowCascades;
    static constexpr uint32_t counterCount = counterCascadeDrawn + maxShadowCascades;
    static constexpr uint32_t groupSize = 256;

    static constexpr auto cameraRegion(bool doubleSided) -> uint32_t { return doubleSided ? 1 : 0; }
    static constexpr auto cascadeRegion(uint32_t cascade, bool doubleSided) -> uint32_t { return ((1 + cascade) * bucketCount) + (doubleSided ? 1 : 0); }

    // Matches Params in instancecull.comp (std430).
    struct CullParams {
        uint32_t instanceCount = 0;
        uint32_t viewCount = 0;
        uint32_t cullEnabled = 1;
        uint32_t regionCapacity = 0;
        uint32_t groupCount = 0;
        uint32_t pad[3] = {};
        std::array<glm::vec4, (1 + maxShadowCascades) * 6> planes = {};
    };

    auto init(RhiDevice* device, uint32_t frameSlots, DeletionQueue* deletionQueue) -> void;
    auto destroy() -> void;

    // Grows every slot's buffers to hold `instanceCount` instances. Returns true when the
    // buffers were replaced, so descriptor sets naming them must be rewritten.
    auto ensureCapacity(uint32_t instanceCount, uint64_t frame) -> bool;

    // Fills the slot's parameter buffer: view 0 from cameraViewProj, views 1.. from the
    // cascades. Returns the workgroup count for the cull and scatter dispatches.
    auto writeParams(uint32_t frameSlot, uint32_t instanceCount, const glm::mat4& cameraViewProj, std::span<const ShadowCascade> cascades, bool cullEnabled) -> uint32_t;
    static auto groupCountFor(uint32_t instanceCount) -> uint32_t;

    struct Handles {
        FgBufferHandle params;
        FgBufferHandle visibility;
        FgBufferHandle groupCounters;
        FgBufferHandle groupOffsets;
        FgBufferHandle commands;
        FgBufferHandle counts;
        FgBufferHandle readback;
    };
    // Imports the slot's buffers; the slot's fence guards them, so no earlier access carries over.
    auto import(FrameGraph& fg, uint32_t frameSlot) -> Handles;
    // Copies counts, visibility and optionally the commands into the slot's readback buffer.
    auto addReadbackPass(FrameGraph& fg, const Handles& handles, uint32_t frameSlot, bool captureCommands) -> void;
    // Once the slot's fence has passed: parses what that slot's frame read back.
    auto parseReadback(uint32_t frameSlot, uint64_t slotFrame) -> void;

    // Buffers for the culling passes' descriptor sets.
    struct SlotBuffers {
        RhiBuffer* params = nullptr;
        RhiBuffer* visibility = nullptr;
        RhiBuffer* groupCounters = nullptr;
        RhiBuffer* groupOffsets = nullptr;
        RhiBuffer* commands = nullptr;
        RhiBuffer* counts = nullptr;
    };
    auto slotBuffers(uint32_t frameSlot) const -> SlotBuffers;
    auto frameSlots() const -> uint32_t { return (uint32_t) slots.size(); }

    // Byte offsets for drawIndexedIndirectCount, and the region's capacity (maxDrawCount).
    auto commandOffset(uint32_t region) const -> uint64_t { return (uint64_t) region * capacity * sizeof(RhiDrawIndexedIndirectCommand); }
    static auto countOffset(uint32_t region) -> uint64_t { return (uint64_t) region * sizeof(uint32_t); }
    auto regionCapacity() const -> uint32_t { return capacity; }

    // Latest readback (one frame-slot cycle old).
    auto readbackFrame() const -> uint64_t { return latest.frame; }
    auto drawsIn(uint32_t region) const -> uint32_t { return latest.totals[counterDraws + region]; }
    auto primitivesIn(uint32_t region) const -> uint64_t { return latest.totals[counterPrimitives + region]; }
    auto cameraCulled() const -> uint32_t { return latest.totals[counterCameraCulled]; }
    auto cascadeCulled(uint32_t cascade) const -> uint32_t { return latest.totals[counterCascadeCulled + cascade]; }
    auto cascadeDrawn(uint32_t cascade) const -> uint32_t { return latest.totals[counterCascadeDrawn + cascade]; }
    auto cascadeCount() const -> uint32_t { return latest.viewCount > 0 ? latest.viewCount - 1 : 0; }
    auto instanceCount() const -> uint32_t { return latest.instanceCount; }
    auto commandCount() const -> uint32_t;
    // Per instance, 1 if visible to the camera in the latest readback (for the editor overlay).
    auto cameraVisible() const -> const std::vector<uint8_t>& { return latest.cameraVisible; }

    // For a pass that just issued `region` indirectly: folds the region's draws and primitives
    // from the latest readback into the pass stats and logs its read-back commands.
    auto report(FrameGraphContext& ctx, uint32_t region, std::span<const GpuInstance> instances) const -> void;

private:
    struct Slot {
        RhiBuffer* params = nullptr; // CpuToGpu, mapped
        void* paramsMapped = nullptr;
        RhiBuffer* visibility = nullptr;
        RhiBuffer* groupCounters = nullptr;
        RhiBuffer* groupOffsets = nullptr;
        RhiBuffer* commands = nullptr;
        RhiBuffer* counts = nullptr;
        RhiBuffer* readback = nullptr; // CpuToGpu, mapped: totals, visibility, commands
        void* readbackMapped = nullptr;
        // What the slot's last recorded frame copied into the readback.
        bool pending = false;
        bool capturedCommands = false;
        uint32_t instanceCount = 0;
        uint32_t viewCount = 0;
    };
    struct Readback {
        uint64_t frame = 0;
        uint32_t instanceCount = 0;
        uint32_t viewCount = 0;
        std::array<uint32_t, counterCount> totals = {};
        std::vector<uint8_t> cameraVisible;
        bool hasCommands = false;
        std::array<std::vector<RhiDrawIndexedIndirectCommand>, regionCount> commands;
    };

    auto readbackVisibilityOffset() const -> uint64_t { return counterCount * sizeof(uint32_t); }
    auto readbackCommandsOffset() const -> uint64_t { return readbackVisibilityOffset() + ((uint64_t) capacity * sizeof(uint32_t)); }

    RhiDevice* device = nullptr;
    DeletionQueue* deletionQueue = nullptr;
    uint32_t capacity = 0; // instances, and commands per region
    std::vector<Slot> slots;
    Readback latest;
};
