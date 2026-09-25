#pragma once

#include "framegraphresource.h"
#include "gpuscene.h"
#include "rhitypes.h"
#include "shadowcascades.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

class DeletionQueue;
class FrameGraph;
class FrameGraphContext;
class RhiDevice;

// Indirect draw commands per view and pipeline bucket (docs/plan_indirect_draws.md). One
// region per (view, bucket): the camera, then each shadow cascade, each split into
// single-sided and double-sided. Built on the CPU from the visibility masks every frame
// into the frame slot's host-visible buffers; stage 5 moves the producer to the GPU.
class DrawLists {
public:
    static constexpr uint32_t bucketCount = 2; // single-sided, double-sided
    static constexpr uint32_t viewCount = 1 + maxShadowCascades;
    static constexpr uint32_t regionCount = viewCount * bucketCount;

    static constexpr auto cameraRegion(bool doubleSided) -> uint32_t { return doubleSided ? 1 : 0; }
    static constexpr auto cascadeRegion(uint32_t cascade, bool doubleSided) -> uint32_t { return ((1 + cascade) * bucketCount) + (doubleSided ? 1 : 0); }

    auto init(RhiDevice* device, uint32_t frameSlots, DeletionQueue* deletionQueue) -> void;
    auto destroy() -> void;

    // Writes every region's commands and counts into the slot's buffers. Camera regions hold
    // one command per visible instance (submesh range); cascade regions one per visible
    // primFirst instance (whole mesh). An empty or mismatched mask draws everything.
    auto build(std::span<const GpuInstance> instances,
               std::span<const uint8_t> visible,
               const std::array<std::vector<uint8_t>, maxShadowCascades>& shadowVisible,
               uint32_t cascadeCount,
               const GpuScene& scene,
               uint32_t frameSlot,
               uint64_t frame) -> void;

    // Imports the slot's command and count buffers; passes read both with IndirectRead.
    struct Handles {
        FgBufferHandle commands;
        FgBufferHandle counts;
    };
    auto import(FrameGraph& fg, uint32_t frameSlot) -> Handles;

    // Byte offsets for drawIndexedIndirectCount, and the region's capacity (maxDrawCount).
    auto commandOffset(uint32_t region) const -> uint64_t { return (uint64_t) region * capacity * sizeof(RhiDrawIndexedIndirectCommand); }
    static auto countOffset(uint32_t region) -> uint64_t { return (uint64_t) region * sizeof(uint32_t); }
    auto regionCapacity() const -> uint32_t { return capacity; }

    // CPU mirror of this frame's commands, for the draw log and the pass stats.
    auto instancesIn(uint32_t region) const -> std::span<const uint32_t> { return mirrorInstances[region]; }
    auto commandsIn(uint32_t region) const -> std::span<const RhiDrawIndexedIndirectCommand> { return mirrorCommands[region]; }
    auto primitivesIn(uint32_t region) const -> uint64_t;
    auto commandCount() const -> uint32_t;

    // For a pass that just issued `region` indirectly: logs each command for the render
    // debugger and folds the region's draws and primitives into the pass stats.
    auto report(FrameGraphContext& ctx, uint32_t region, std::span<const GpuInstance> instances) const -> void;

private:
    auto ensureCapacity(uint32_t count, uint64_t frame) -> void;

    RhiDevice* device = nullptr;
    DeletionQueue* deletionQueue = nullptr;
    uint32_t capacity = 0;                  // commands per region
    std::vector<RhiBuffer*> commandBuffers; // per frame slot, CpuToGpu, mapped
    std::vector<void*> commandMapped;
    std::vector<RhiBuffer*> countBuffers;
    std::vector<void*> countMapped;
    std::array<std::vector<RhiDrawIndexedIndirectCommand>, regionCount> mirrorCommands;
    std::array<std::vector<uint32_t>, regionCount> mirrorInstances;
};
