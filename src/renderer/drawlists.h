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
    uint64_t cameraTriangles = 0; // drawn by the camera after culling, every bucket
    uint64_t sceneTriangles = 0;  // every instance's submesh, culled or not
    uint32_t cascadeCount = 0;
    std::array<uint32_t, maxShadowCascades> cascadeCulled = {};
    std::array<uint32_t, maxShadowCascades> cascadeDrawn = {};
    std::array<uint64_t, maxShadowCascades> cascadeTriangles = {}; // drawn per cascade, every bucket
    std::vector<uint8_t> cameraVisible;                            // per instance, 1 = visible
    std::vector<uint8_t> viewBits;                                 // per instance, bit v = visible in view v (0 camera, 1.. cascades)
    // Render LOD: instances of a level their group doesn't draw (in no view), and from the selection's readback the groups, those drawing
    // each level, those falling back while the wanted level isn't resident, those at a forced level; per group the level drawn and
    // wanted (0xFF none) when the states were read.
    uint32_t lodDropped = 0;
    uint64_t lodFrame = 0;
    uint32_t lodGroups = 0;
    std::array<uint32_t, 8> lodPicked = {};
    uint32_t lodFallbacks = 0;
    uint32_t lodForced = 0;
    bool lodHasStates = false;
    glm::vec3 lodEye = glm::vec3(0.0f); // the camera the pick measured from: its position and the view window's area at depth 1
    float lodWindowArea = 1.0f;
    std::vector<uint8_t> lodDrawn;
    std::vector<uint8_t> lodWanted;
};

class DeletionQueue;
class FrameGraph;
class FrameGraphContext;
class RhiDevice;

// Indirect draw commands per view and pipeline bucket, written on the GPU by the culling
// passes (src/renderer/README.md, "GPU-driven frame"). One region per (view, bucket):
// the camera, then each shadow cascade, each split into single-sided and double-sided, opaque
// and cutout. A cutout bucket's pipelines discard where the material's opacity is below its threshold.
// A region holds as many commands as instances can land in it: each live instance is in one camera
// bucket and at most one cascade bucket, known from its flags, so the regions are sized from counts
// kept as instances change (countInstance) and packed one after another (writeParams).
//
// Owns the frame slot's buffers the culling passes read and write, and the readback that
// brings counts and visibility back to the CPU once the slot's fence has passed, with the
// commands too, into a buffer of their own, while the render debugger is open. Everything
// read back is one frame-slot cycle old.
class DrawLists {
public:
    static constexpr uint32_t bucketCount = 4; // single-sided, double-sided, then the same two cutout
    static constexpr uint32_t viewCount = 1 + maxShadowCascades;
    static constexpr uint32_t regionCount = viewCount * bucketCount;
    // Counters the culling shader produces per workgroup and in total (instancecull.comp).
    static constexpr uint32_t counterDraws = 0;
    static constexpr uint32_t counterPrimitives = regionCount;
    static constexpr uint32_t counterCameraCulled = regionCount * 2;
    static constexpr uint32_t counterCascadeCulled = counterCameraCulled + 1;
    static constexpr uint32_t counterCascadeDrawn = counterCascadeCulled + maxShadowCascades;
    static constexpr uint32_t counterLodDropped = counterCascadeDrawn + maxShadowCascades;
    static constexpr uint32_t counterCount = counterLodDropped + 1;
    static constexpr uint32_t groupSize = 256;
    static constexpr uint32_t noBucket = UINT32_MAX;

    static constexpr auto bucket(bool doubleSided, bool cutout) -> uint32_t { return (cutout ? 2 : 0) + (doubleSided ? 1 : 0); }
    static constexpr auto cameraRegion(bool doubleSided, bool cutout = false) -> uint32_t { return bucket(doubleSided, cutout); }
    static constexpr auto cascadeRegion(uint32_t cascade, bool doubleSided, bool cutout = false) -> uint32_t { return ((1 + cascade) * bucketCount) + bucket(doubleSided, cutout); }

    // Matches Params in instancecull.comp (std430).
    struct CullParams {
        uint32_t instanceCount = 0;
        uint32_t viewCount = 0;
        uint32_t cullEnabled = 1;
        uint32_t commandCapacity = 0; // commands the buffer holds, every region
        uint32_t groupCount = 0;
        uint32_t commandsUsed = 0; // the end of the last region
        uint32_t pad[2] = {};
        std::array<uint32_t, regionCount> regionOffsets = {}; // each region's first command
        std::array<glm::vec4, (1 + maxShadowCascades) * 6> planes = {};
    };

    // The bucket an instance's commands land in for the camera and for the cascades (the shader's bucketOf and drawnIn): every live
    // instance draws its submesh for the camera; a cascade draws a prim's whole mesh once, from its primFirst instance, unless a submesh
    // of the prim is cutout, when it draws each submesh. noBucket where it draws nothing.
    static auto cameraBucketOf(const GpuInstance& instance) -> uint32_t;
    static auto cascadeBucketOf(const GpuInstance& instance) -> uint32_t;
    // Counts an instance in its buckets (delta +1) or takes it out (-1); a dead instance counts nowhere.
    auto countInstance(const GpuInstance& instance, int delta) -> void;
    // The commands the buffer must hold for the instances counted: the camera's buckets and every cascade's.
    auto commandsNeeded() const -> uint64_t;

    auto init(RhiDevice* device, uint32_t frameSlots, DeletionQueue* deletionQueue) -> void;
    auto destroy() -> void;

    // Grows every slot's buffers to hold `instanceCount` instances and `commandCount` commands. Returns
    // true when buffers were replaced, so descriptor sets naming them must be rewritten.
    auto ensureCapacity(uint32_t instanceCount, uint64_t commandCount, uint64_t frame) -> bool;

    // Fills the slot's parameter buffer: view 0 from cameraViewProj, views 1.. from the
    // cascades. Returns the workgroup count for the cull and scatter dispatches.
    auto writeParams(uint32_t frameSlot, uint32_t instanceCount, const glm::mat4& cameraViewProj, std::span<const ShadowCascade> cascades, bool cullEnabled) -> uint32_t;
    static auto groupCountFor(uint32_t instanceCount) -> uint32_t;

    struct Handles {
        FgBufferHandle params;
        FgBufferHandle visibility;
        FgBufferHandle cullPlanes;
        FgBufferHandle groupCounters;
        FgBufferHandle groupOffsets;
        FgBufferHandle commands;
        FgBufferHandle counts;
        FgBufferHandle readback;
    };
    // Imports the slot's buffers; the slot's fence guards them, so no earlier access carries over.
    auto import(FrameGraph& fg, uint32_t frameSlot) -> Handles;
    // Copies counts and visibility into the slot's readback buffer, and with captureCommands the commands into the slot's commands
    // readback, made the first time it is asked for.
    auto addReadbackPass(FrameGraph& fg, const Handles& handles, uint32_t frameSlot, bool captureCommands) -> void;
    // Once the slot's fence has passed: parses what that slot's frame read back.
    auto parseReadback(uint32_t frameSlot, uint64_t slotFrame) -> void;

    // Buffers for the culling passes' descriptor sets.
    struct SlotBuffers {
        RhiBuffer* params = nullptr;
        RhiBuffer* visibility = nullptr;
        RhiBuffer* cullPlanes = nullptr;
        RhiBuffer* groupCounters = nullptr;
        RhiBuffer* groupOffsets = nullptr;
        RhiBuffer* commands = nullptr;
        RhiBuffer* counts = nullptr;
    };
    auto slotBuffers(uint32_t frameSlot) const -> SlotBuffers;
    auto frameSlots() const -> uint32_t { return (uint32_t) slots.size(); }

    // Byte offsets for drawIndexedIndirectCount, and the region's capacity (maxDrawCount), as the frame's writeParams laid them out.
    auto commandOffset(uint32_t region) const -> uint64_t { return (uint64_t) frameOffsets[region] * sizeof(RhiDrawIndexedIndirectCommand); }
    static auto countOffset(uint32_t region) -> uint64_t { return (uint64_t) region * sizeof(uint32_t); }
    auto regionCapacity(uint32_t region) const -> uint32_t { return frameCapacities[region]; }
    auto commandCapacityTotal() const -> uint64_t { return commandCapacity; }
    // A region as text ("camera.single", "cascade0.double", ...); a literal, so it can name GPU zones.
    static auto regionName(uint32_t region) -> const char*;

    // Latest readback (one frame-slot cycle old).
    auto readbackFrame() const -> uint64_t { return latest.frame; }
    auto drawsIn(uint32_t region) const -> uint32_t { return latest.totals[counterDraws + region]; }
    auto primitivesIn(uint32_t region) const -> uint64_t { return latest.totals[counterPrimitives + region]; }
    auto cameraCulled() const -> uint32_t { return latest.totals[counterCameraCulled]; }
    auto cascadeCulled(uint32_t cascade) const -> uint32_t { return latest.totals[counterCascadeCulled + cascade]; }
    auto cascadeDrawn(uint32_t cascade) const -> uint32_t { return latest.totals[counterCascadeDrawn + cascade]; }
    auto lodDropped() const -> uint32_t { return latest.totals[counterLodDropped]; }
    auto cascadeCount() const -> uint32_t { return latest.viewCount > 0 ? latest.viewCount - 1 : 0; }
    auto instanceCount() const -> uint32_t { return latest.instanceCount; }
    auto commandCount() const -> uint32_t;
    // Per instance, 1 if visible to the camera in the latest readback (for the editor overlay).
    auto cameraVisible() const -> const std::vector<uint8_t>& { return latest.cameraVisible; }
    // Per instance, visibility bit v for view v (camera, then cascades), latest readback.
    auto viewBits() const -> const std::vector<uint8_t>& { return latest.viewBits; }

    // For a pass that just issued `region` indirectly: folds the region's draws and primitives
    // from the latest readback into the pass stats and logs its read-back commands.
    auto report(FrameGraphContext& ctx, uint32_t region, std::span<const GpuInstance> instances) const -> void;

private:
    struct Slot {
        RhiBuffer* params = nullptr; // CpuToGpu, mapped
        void* paramsMapped = nullptr;
        RhiBuffer* visibility = nullptr;
        RhiBuffer* cullPlanes = nullptr;
        RhiBuffer* groupCounters = nullptr;
        RhiBuffer* groupOffsets = nullptr;
        RhiBuffer* commands = nullptr;
        RhiBuffer* counts = nullptr;
        RhiBuffer* readback = nullptr; // GpuToCpu, mapped: totals, visibility
        void* readbackMapped = nullptr;
        RhiBuffer* commandsReadback = nullptr; // GpuToCpu, mapped: the commands; made when the render debugger first asks
        void* commandsReadbackMapped = nullptr;
        // What the slot's last recorded frame copied into the readback, and how it laid out the regions.
        bool pending = false;
        bool capturedCommands = false;
        uint32_t instanceCount = 0;
        uint32_t viewCount = 0;
        std::array<uint32_t, regionCount> offsets = {};
    };
    struct Readback {
        uint64_t frame = 0;
        uint32_t instanceCount = 0;
        uint32_t viewCount = 0;
        std::array<uint32_t, counterCount> totals = {};
        std::vector<uint8_t> cameraVisible;
        std::vector<uint8_t> viewBits;
        bool hasCommands = false;
        std::array<std::vector<RhiDrawIndexedIndirectCommand>, regionCount> commands;
    };

    auto readbackVisibilityOffset() const -> uint64_t { return counterCount * sizeof(uint32_t); }
    auto readbackBytes() const -> uint64_t { return readbackVisibilityOffset() + ((uint64_t) capacity * sizeof(uint32_t)); }
    auto commandBytes() const -> uint64_t { return commandCapacity * sizeof(RhiDrawIndexedIndirectCommand); }

    RhiDevice* device = nullptr;
    DeletionQueue* deletionQueue = nullptr;
    uint32_t capacity = 0;        // instances
    uint64_t commandCapacity = 0; // commands, every region
    // Live instances per bucket, for the camera and for each cascade; and this frame's region layout (writeParams).
    std::array<uint64_t, bucketCount> cameraBuckets = {};
    std::array<uint64_t, bucketCount> cascadeBuckets = {};
    std::array<uint32_t, regionCount> frameOffsets = {};
    std::array<uint32_t, regionCount> frameCapacities = {};
    std::vector<Slot> slots;
    Readback latest;
};
