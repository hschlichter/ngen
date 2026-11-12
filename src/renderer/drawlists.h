#pragma once

#include "framegraphresource.h"
#include "gpuscene.h"
#include "rhitypes.h"
#include "shadowcascades.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// A view's meshlets, from the task shaders of the pass that counts them (the camera's geometry pass, the shadow pass): those the drawn
// instances hold, those culled by the frustum, by the normal cone and by occlusion, those drawn and their triangles.
struct MeshletCounts {
    uint32_t tested = 0;
    uint32_t frustumCulled = 0;
    uint32_t coneCulled = 0;
    uint32_t occluded = 0;
    uint32_t drawn = 0;
    uint64_t triangles = 0;
};

// GPU culling result for the editor (Culling window, AABB overlay), from DrawLists' readback.
// Culling goes in steps: the scene, what instance culling keeps (frustum, occlusion, render LOD), and what the task shaders draw of it
// (meshlet culling). "Triangles" is what is drawn, after both; "instance triangles" what instance culling alone keeps.
struct CullResult {
    uint64_t frame = 0; // 0: nothing read back yet
    uint32_t instances = 0;
    uint32_t cameraCulled = 0;            // outside the camera's frustum
    uint32_t cameraDrawn = 0;             // instances whose meshlets the camera's task shaders took, both of occlusion's phases
    uint64_t cameraTriangles = 0;         // drawn by the camera: what its meshlet culling kept
    uint64_t cameraInstanceTriangles = 0; // the drawn instances' triangles, before meshlet culling
    uint64_t sceneTriangles = 0;          // every live instance's submesh, culled or not
    uint64_t sceneMeshlets = 0;           // every live instance's submesh's meshlets
    uint32_t cascadeCount = 0;
    std::array<uint32_t, maxShadowCascades> cascadeCulled = {};
    std::array<uint32_t, maxShadowCascades> cascadeDrawn = {};
    std::array<uint64_t, maxShadowCascades> cascadeTriangles = {};         // drawn per cascade: what its meshlet culling kept
    std::array<uint64_t, maxShadowCascades> cascadeInstanceTriangles = {}; // the cascade's drawn instances' triangles, every bucket
    MeshletCounts cameraMeshlets;                                          // both of occlusion's phases
    std::array<MeshletCounts, maxShadowCascades> cascadeMeshlets = {};
    // Occlusion culling, when it ran in the frame read back: the camera's instances drawn in the first phase (visible last frame), in the
    // second (shown by the depth pyramid, not drawn first), and the frustum-visible ones the pyramid hid. occlusionCullCamera: its depth was
    // drawn from the culling camera into a depth buffer of its own, as it wasn't the rendered one (a debug camera, a frozen frustum).
    bool occlusion = false;
    bool occlusionCullCamera = false;
    uint32_t earlyDrawn = 0;
    uint32_t lateDrawn = 0;
    uint32_t occluded = 0;
    uint32_t regionOverflow = 0; // draw records dropped past their region's end: the CPU's bucket counts disagree with the shader's (a bug)
    // Per region (view * 4 + bucket, DrawLists::regionName): the draws, their meshlets (the work items) and the instances' triangles.
    std::vector<uint32_t> regionDraws;
    std::vector<uint32_t> regionMeshlets;
    std::vector<uint64_t> regionTriangles;
    // Per instance, bit v = visible in view v (0 camera, 1.. cascades); read back only for a frame whose snapshot asked (readVisibility),
    // empty otherwise.
    std::vector<uint8_t> viewBits;
    // Render LOD: instances of a level their group doesn't draw (in no view), instances drawn in a fading group (both levels, in the cutout
    // buckets), and from the selection's readback the groups, those drawing each level, those falling back while the wanted level isn't
    // resident, those at a forced level, those fading, those whose drawn level changed this frame without a fade (a pop); per group the
    // level drawn and wanted (0xFF none), the level fading in (0xFF none) and its share, the measure, and its flags (LodStateFlags), when
    // the states were read.
    uint32_t lodDropped = 0;
    uint32_t lodFadingInstances = 0;
    uint64_t lodFrame = 0;
    uint32_t lodGroups = 0;
    std::array<uint32_t, 8> lodPicked = {};
    uint32_t lodFallbacks = 0;
    uint32_t lodForced = 0;
    uint32_t lodFading = 0;
    uint32_t lodPopped = 0;
    bool lodHasStates = false;
    glm::vec3 lodEye = glm::vec3(0.0f); // the camera the pick measured from: its position and the view window's area at depth 1
    float lodWindowArea = 1.0f;
    std::vector<uint8_t> lodDrawn;
    std::vector<uint8_t> lodWanted;
    std::vector<uint8_t> lodFadeLevel;
    std::vector<float> lodFade;
    std::vector<float> lodMeasure;
    std::vector<uint8_t> lodFlags;
};

// One instance drawn in a region (instancecull.comp's DrawRecord), in instance order: its meshlets there (its submesh's) and where they
// start in the work item buffer (an index into the whole buffer, not the region). What the draw log, the GPU Scene window and the frame debugger read; the draws
// themselves go through the work items.
struct DrawRecord {
    uint32_t instance = 0;
    uint32_t meshletCount = 0;
    uint32_t firstItem = 0; // in the work items
    uint32_t pad = 0;
};
static_assert(sizeof(DrawRecord) == 16);

// The box around a culling workgroup's slots (DrawLists::groupSize of them), which the cull pass tests before the slots: a group outside
// every view skips its instances. min.w: 0 a box to test, 1 a live slot without bounds (always tested slot by slot), 2 no live slot. max.w:
// how many of its slots are live, which a skipped group counts as culled (a dead slot counts in no view).
struct GroupBox {
    glm::vec4 min = glm::vec4(0.0f, 0.0f, 0.0f, 2.0f);
    glm::vec4 max = glm::vec4(0.0f);
};
static_assert(sizeof(GroupBox) == 32);
inline constexpr float groupBoxTested = 0.0f;
inline constexpr float groupBoxUnbounded = 1.0f;
inline constexpr float groupBoxEmpty = 2.0f;

// One meshlet to draw (meshlet.glsl's work items): the instance and the meshlet's place in the meshlet pool.
struct MeshletWorkItem {
    uint32_t instance = 0;
    uint32_t meshlet = 0;
};
static_assert(sizeof(MeshletWorkItem) == 8);

// A region's draw (instancecull.comp's scan writes it): the task workgroups for its work items, 32 meshlets each (x wraps into y past
// maxTaskGroupsX), and its work items' range. The draw reads the command; its task shaders read the range.
struct RegionWork {
    RhiDrawMeshTasksIndirectCommand tasks;
    uint32_t firstItem = 0;
    uint32_t itemCount = 0;
    uint32_t pad[3] = {};
};
static_assert(sizeof(RegionWork) == 32);
inline constexpr uint32_t meshletsPerTask = 32;
// The task workgroups a draw launches in x before it goes on in y: every device takes 65,535 a dimension.
inline constexpr uint32_t maxTaskGroupsX = 32768;

// The first words of every mesh pass's push constants (meshlet.glsl): the region whose commands draw, and meshletFlag*.
struct MeshletPush {
    uint32_t region = 0;
    uint32_t flags = 0;
};
inline constexpr uint32_t meshletFlagCount = 1u << 0;      // the pass adds to the meshlet counts (the camera's geometry pass, the shadow pass)
inline constexpr uint32_t meshletFlagSplit = 1u << 1;      // two mesh workgroups per meshlet (the debug views)
inline constexpr uint32_t meshletFlagOcclusion = 1u << 2;  // meshlets against the depth pyramid (the second phase; after a prepass, the geometry pass)
inline constexpr uint32_t meshletFlagClassify = 1u << 3;   // every meshlet kept, with the test that would cull it (the meshletcull debug view)
inline constexpr uint32_t meshletFlagHiddenOnly = 1u << 4; // with meshletFlagClassify: only the meshlets the depth pyramid hides
inline constexpr uint32_t meshletFlagCullCamera = 1u << 5; // the depth prepass draws from the culling camera, into its own depth

class DeletionQueue;
class FrameGraph;
class FrameGraphContext;
class RhiDevice;

// What each view draws, per pipeline bucket, written on the GPU by the culling passes (src/renderer/README.md, "Culling and draw lists").
// One region per (view, bucket): the camera, then each shadow cascade, then the camera's late phase (occlusion culling's second phase),
// each split into single-sided and double-sided, opaque and cutout. A cutout bucket's pipelines discard where the material's opacity is
// below its threshold. A region is drawn by one indirect mesh-task draw over its meshlet work items, every visible instance's meshlets in
// instance order, which share the frame slot's one work item buffer.
//
// A region holds as many draw records as instances can land in it: each live instance is in one bucket, the same in every view, known
// from its flags, so the regions are sized from counts kept as instances change (countInstance) and packed one after another (writeParams).
//
// Owns the frame slot's buffers the culling passes read and write, and the readback that brings counts and visibility back to the CPU
// once the slot's fence has passed, with the draw records too, into a buffer of their own, while the render debugger is open. Everything
// read back is one frame-slot cycle old. Also the occlusion history, kept across frames: per instance slot, whether the camera saw it
// last frame.
class DrawLists {
public:
    static constexpr uint32_t bucketCount = 4; // single-sided, double-sided, then the same two cutout
    static constexpr uint32_t viewCount = 2 + maxShadowCascades;
    static constexpr uint32_t lateView = viewCount - 1; // the camera's second phase
    static constexpr uint32_t regionCount = viewCount * bucketCount;
    // Visibility bits beside the views': drawn in the camera's first phase (visible last frame); bit lateView, drawn in its second.
    static constexpr uint32_t visibilityEarlyBit = 1u << 6;
    // Counters the culling shader produces per workgroup and in total (instancecull.comp).
    static constexpr uint32_t counterDraws = 0;
    static constexpr uint32_t counterPrimitives = regionCount;
    static constexpr uint32_t counterCameraCulled = regionCount * 2;
    static constexpr uint32_t counterCascadeCulled = counterCameraCulled + 1;
    static constexpr uint32_t counterCascadeDrawn = counterCascadeCulled + maxShadowCascades;
    static constexpr uint32_t counterLodDropped = counterCascadeDrawn + maxShadowCascades;
    static constexpr uint32_t counterCameraOccluded = counterLodDropped + 1;
    static constexpr uint32_t counterMeshlets = counterCameraOccluded + 1;         // regionCount: work items per region
    static constexpr uint32_t counterWorkOverflow = counterMeshlets + regionCount; // work items past the buffer's capacity, not drawn
    // Draw records the scatters dropped past their region's end: the CPU's bucket counts (countInstance) undercounted a region. 0 unless the
    // CPU's bucket rule and the shader's disagree.
    static constexpr uint32_t counterRegionOverflow = counterWorkOverflow + 1;
    static constexpr uint32_t counterFading = counterRegionOverflow + 1; // instances drawn in a fading render-LOD group
    static constexpr uint32_t counterCount = counterFading + 1;
    static constexpr uint32_t groupSize = 256;
    static constexpr uint32_t noBucket = UINT32_MAX;
    // Meshlet counts per view, summed by the task shaders of the passes that count (meshlet.glsl), zeroed by the culling scan.
    static constexpr uint32_t meshletCounterTested = 0;
    static constexpr uint32_t meshletCounterFrustum = 1;
    static constexpr uint32_t meshletCounterCone = 2;
    static constexpr uint32_t meshletCounterOccluded = 3;
    static constexpr uint32_t meshletCounterDrawn = 4;
    static constexpr uint32_t meshletCounterTriangles = 5;
    static constexpr uint32_t meshletCountersPerView = 6;
    static constexpr uint32_t meshletCounterCount = viewCount * meshletCountersPerView;

    static constexpr auto bucket(bool doubleSided, bool cutout) -> uint32_t { return (cutout ? 2 : 0) + (doubleSided ? 1 : 0); }
    static constexpr auto cameraRegion(bool doubleSided, bool cutout = false) -> uint32_t { return bucket(doubleSided, cutout); }
    static constexpr auto cameraLateRegion(bool doubleSided, bool cutout = false) -> uint32_t { return (lateView * bucketCount) + bucket(doubleSided, cutout); }
    static constexpr auto cascadeRegion(uint32_t cascade, bool doubleSided, bool cutout = false) -> uint32_t { return ((1 + cascade) * bucketCount) + bucket(doubleSided, cutout); }

    // Matches Params in instancecull.comp and meshlet.glsl (std430). cameraPosition is the culling camera's (the frozen one while frozen),
    // for the meshlets' normal-cone test.
    struct CullParams {
        uint32_t instanceCount = 0;
        uint32_t viewCount = 0; // the camera and the cascades: the views tested against their frustum
        uint32_t cullEnabled = 1;
        uint32_t commandCapacity = 0; // draw records the buffer holds, every region
        uint32_t groupCount = 0;
        uint32_t occlusion = 0; // two-phase occlusion on
        uint32_t hzbWidth = 1;  // the depth pyramid's mip 0
        uint32_t hzbHeight = 1;
        uint32_t workCapacity = 0; // meshlet work items the buffer holds, every region together
        uint32_t commandsUsed = 0; // the end of the last region
        uint32_t pad[2] = {};
        glm::vec4 cameraPosition = glm::vec4(0.0f);
        glm::mat4 cameraViewProj = glm::mat4(1.0f);                  // the culling camera's, for the occlusion tests
        std::array<glm::vec4, DrawLists::viewCount * 6> planes = {}; // the late view repeats the camera's
        std::array<uint32_t, regionCount> regionOffsets = {};        // each region's first draw record; last, so meshlet.glsl reads a prefix
    };

    // The bucket an instance's draws land in, in every view (the shader's bucketOf): its double-sidedness and cutout; noBucket for a dead
    // slot.
    static auto bucketOf(const GpuInstance& instance) -> uint32_t;
    // Counts an instance in its bucket and its meshlets in the live meshlets (delta +1), or takes it out (-1); a dead one counts nowhere.
    auto countInstance(const GpuInstance& instance, int delta) -> void;
    // Recomputes the boxes of the culling workgroups holding the changed slots (and of every group past a change in the slot count); the
    // next writeParams copies them to the frame slot's buffer.
    auto updateGroupBounds(const ChunkedList<GpuInstance>& instances, std::span<const uint32_t> changedSlots) -> void;
    // The draw records the buffer must hold for the instances counted: their buckets' sizes, once per view.
    auto commandsNeeded() const -> uint64_t;
    // Every live instance's meshlets, as counted.
    auto liveMeshlets() const -> uint64_t { return liveMeshletCount; }

    auto init(RhiDevice* device, uint32_t frameSlots, DeletionQueue* deletionQueue) -> void;
    auto destroy() -> void;

    // Grows every slot's buffers to hold `instanceCount` instances and `commandCount` draw records. Returns true when buffers were
    // replaced, so descriptor sets naming them must be rewritten.
    auto ensureCapacity(uint32_t instanceCount, uint64_t commandCount, uint64_t frame) -> bool;
    auto instanceCapacity() const -> uint32_t { return capacity; }
    // Grows every slot's work item buffer to `items` meshlets, and past what the last readback overflowed by. True when it was replaced.
    auto ensureWorkCapacity(uint64_t items, uint64_t frame) -> bool;
    auto workCapacity() const -> uint32_t { return workItemCapacity; }

    // Fills the slot's parameter buffer: view 0 from cameraViewProj, views 1.. from the
    // cascades. Returns the workgroup count for the cull and scatter dispatches.
    // Occlusion, when on: the depth pyramid's mip-0 size for the tests.
    struct Occlusion {
        bool enabled = false;
        bool cullCamera = false; // its depth drawn from the culling camera, which isn't the rendered one
        uint32_t hzbWidth = 1;
        uint32_t hzbHeight = 1;
    };
    auto writeParams(uint32_t frameSlot, uint32_t instanceCount, const glm::mat4& cameraViewProj, std::span<const ShadowCascade> cascades, bool cullEnabled, Occlusion occlusion) -> uint32_t;
    static auto groupCountFor(uint32_t instanceCount) -> uint32_t;

    struct Handles {
        FgBufferHandle params;
        FgBufferHandle groupBounds;
        FgBufferHandle visibility;
        FgBufferHandle cullPlanes;
        FgBufferHandle groupCounters;
        FgBufferHandle groupOffsets;
        FgBufferHandle commands; // draw records
        FgBufferHandle counts;
        FgBufferHandle meshletStats;
        FgBufferHandle readback;
        FgBufferHandle history;
        FgBufferHandle regionWork;
        FgBufferHandle workItems;
        // Not the draw lists': the render-LOD state, which the cutout buckets' mesh shaders read for fades. Set by the renderer from
        // LodGroups::Handles, so the passes drawing these lists declare the read.
        FgBufferHandle lodStates;
    };
    // Imports the slot's buffers; the slot's fence guards them, so no earlier access carries over. The history is imported with the access
    // the last frame left it in.
    auto import(FrameGraph& fg, uint32_t frameSlot) -> Handles;
    // Stores the access the history is left in for the next frame's import.
    auto afterExecute(const FrameGraph& fg) -> void;
    // A history buffer made since it was last cleared: the culling clears it first (InstanceCullPass), then this is called.
    auto historyNeedsClear() const -> bool { return historyFresh; }
    auto historyCleared() -> void { historyFresh = false; }
    auto historyBuffer() const -> RhiBuffer* { return history; }
    // Copies the counts and the meshlet counts into the slot's readback buffer, with readVisibility every instance's visibility bits too,
    // and with captureCommands the draw records into the slot's commands readback, made when it is asked for and let go (at `frame`,
    // through the deletion queue) the first frame it isn't.
    auto addReadbackPass(FrameGraph& fg, const Handles& handles, uint32_t frameSlot, bool readVisibility, bool captureCommands, uint64_t frame) -> void;
    // Once the slot's fence has passed: parses what that slot's frame read back.
    auto parseReadback(uint32_t frameSlot, uint64_t slotFrame) -> void;

    // Buffers for the culling passes' descriptor sets.
    struct SlotBuffers {
        RhiBuffer* params = nullptr;
        RhiBuffer* groupBounds = nullptr;
        RhiBuffer* visibility = nullptr;
        RhiBuffer* cullPlanes = nullptr;
        RhiBuffer* groupCounters = nullptr;
        RhiBuffer* groupOffsets = nullptr;
        RhiBuffer* commands = nullptr;
        RhiBuffer* counts = nullptr;
        RhiBuffer* meshletStats = nullptr;
        RhiBuffer* regionWork = nullptr;
        RhiBuffer* workItems = nullptr;
    };
    auto slotBuffers(uint32_t frameSlot) const -> SlotBuffers;
    auto frameSlots() const -> uint32_t { return (uint32_t) slots.size(); }

    // A region's draw records, as the frame's writeParams laid them out: its first and how many fit; its draw count is at countOffset in
    // the counts.
    auto regionOffset(uint32_t region) const -> uint32_t { return frameOffsets[region]; }
    auto regionCapacity(uint32_t region) const -> uint32_t { return frameCapacities[region]; }
    auto commandCapacityTotal() const -> uint64_t { return commandCapacity; }
    static auto countOffset(uint32_t region) -> uint64_t { return (uint64_t) region * sizeof(uint32_t); }
    // Draws a region's meshlets with the bound mesh pipeline: one drawMeshTasksIndirect, its command the region's RegionWork.
    static auto drawRegion(RhiCommandBuffer* cmd, RhiBuffer* regionWork, uint32_t region) -> void;
    // A region as text ("camera.single", "cascade0.double", ...): the draw log, the records, the windows.
    static auto regionName(uint32_t region) -> const char*;
    // What a region draws ("Opaque", "Opaque two-sided", "Cutout", "Cutout two-sided"), whichever view: the name of its draw's GPU zone, under
    // the pass and cascade zones that say whose view. A literal.
    static auto drawName(uint32_t region) -> const char*;

    // Latest readback (one frame-slot cycle old).
    auto readbackFrame() const -> uint64_t { return latest.frame; }
    auto drawsIn(uint32_t region) const -> uint32_t { return latest.totals[counterDraws + region]; }
    auto primitivesIn(uint32_t region) const -> uint64_t { return latest.totals[counterPrimitives + region]; }
    auto cameraCulled() const -> uint32_t { return latest.totals[counterCameraCulled]; }
    auto cascadeCulled(uint32_t cascade) const -> uint32_t { return latest.totals[counterCascadeCulled + cascade]; }
    auto cascadeDrawn(uint32_t cascade) const -> uint32_t { return latest.totals[counterCascadeDrawn + cascade]; }
    auto lodDropped() const -> uint32_t { return latest.totals[counterLodDropped]; }
    auto cameraOccluded() const -> uint32_t { return latest.totals[counterCameraOccluded]; }
    auto occlusionEnabled() const -> bool { return latest.occlusion; }
    auto occlusionCullCamera() const -> bool { return latest.occlusionCullCamera; }
    auto meshletsIn(uint32_t region) const -> uint32_t { return latest.totals[counterMeshlets + region]; }
    auto workOverflow() const -> uint32_t { return latest.totals[counterWorkOverflow]; }
    auto regionOverflow() const -> uint32_t { return latest.totals[counterRegionOverflow]; }
    auto fadingInstances() const -> uint32_t { return latest.totals[counterFading]; }
    // A view's meshlet count (meshletCounter*), latest readback.
    auto meshletCount(uint32_t view, uint32_t counter) const -> uint32_t { return latest.meshletStats[(view * meshletCountersPerView) + counter]; }
    auto cascadeCount() const -> uint32_t { return latest.viewCount > 0 ? latest.viewCount - 1 : 0; }
    auto instanceCount() const -> uint32_t { return latest.instanceCount; }
    auto commandCount() const -> uint32_t;
    // Per instance, 1 if visible to the camera in the latest readback (for the editor overlay).
    // Per instance, visibility bit v for view v (camera, then cascades), latest readback; empty when it didn't read them.
    auto viewBits() const -> const std::vector<uint8_t>& { return latest.viewBits; }

    // For a pass that just issued `region` indirectly: folds the region's draws and primitives
    // from the latest readback into the pass stats and logs its read-back commands.
    auto report(FrameGraphContext& ctx, uint32_t region, const ChunkedList<GpuInstance>& instances) const -> void;

private:
    struct Slot {
        RhiBuffer* params = nullptr; // CpuToGpu, mapped
        void* paramsMapped = nullptr;
        RhiBuffer* groupBounds = nullptr; // CpuToGpu, mapped: GroupBox per culling workgroup
        void* groupBoundsMapped = nullptr;
        RhiBuffer* visibility = nullptr;
        RhiBuffer* cullPlanes = nullptr;
        RhiBuffer* groupCounters = nullptr;
        RhiBuffer* groupOffsets = nullptr;
        RhiBuffer* commands = nullptr;
        RhiBuffer* counts = nullptr;
        RhiBuffer* meshletStats = nullptr;
        RhiBuffer* regionWork = nullptr;
        RhiBuffer* workItems = nullptr;
        RhiBuffer* readback = nullptr; // GpuToCpu, mapped: totals, meshlet counts, visibility
        void* readbackMapped = nullptr;
        RhiBuffer* commandsReadback = nullptr; // GpuToCpu, mapped: the draw records; only while the render debugger asks
        void* commandsReadbackMapped = nullptr;
        // What the slot's last recorded frame copied into the readback, and how it laid out the regions.
        bool pending = false;
        bool readVisibility = false;
        bool capturedCommands = false;
        uint32_t instanceCount = 0;
        uint32_t viewCount = 0;
        bool occlusion = false;
        bool occlusionCullCamera = false;
        uint32_t workCapacity = 0; // the work items the frame was built with: an overflow from fewer than now was answered already
        std::array<uint32_t, regionCount> offsets = {};
        std::array<uint32_t, regionCount> capacities = {};
    };
    struct Readback {
        uint64_t frame = 0;
        uint32_t instanceCount = 0;
        uint32_t viewCount = 0;
        bool occlusion = false;
        bool occlusionCullCamera = false;
        std::array<uint32_t, counterCount> totals = {};
        std::array<uint32_t, meshletCounterCount> meshletStats = {};
        std::vector<uint8_t> viewBits;
        bool hasCommands = false;
        std::array<std::vector<DrawRecord>, regionCount> commands;
    };

    static auto readbackMeshletOffset() -> uint64_t { return counterCount * sizeof(uint32_t); }
    auto readbackVisibilityOffset() const -> uint64_t { return readbackMeshletOffset() + (meshletCounterCount * sizeof(uint32_t)); }
    auto importHistory(FrameGraph& fg) -> FgBufferHandle;
    auto readbackBytes() const -> uint64_t { return readbackVisibilityOffset() + ((uint64_t) capacity * sizeof(uint32_t)); }
    auto commandBytes() const -> uint64_t { return commandCapacity * sizeof(DrawRecord); }

    RhiDevice* device = nullptr;
    DeletionQueue* deletionQueue = nullptr;
    uint32_t capacity = 0;         // instances
    uint64_t commandCapacity = 0;  // draw records, every region
    uint32_t workItemCapacity = 0; // meshlet work items per slot, every region together
    // Live instances per bucket and their meshlets; and this frame's region layout (writeParams).
    std::array<uint64_t, bucketCount> buckets = {};
    uint64_t liveMeshletCount = 0;
    std::vector<GroupBox> groupBoxes; // per culling workgroup, as the slots are now
    size_t groupSlotCount = 0;        // the slot count groupBoxes were last computed for
    std::array<uint32_t, regionCount> frameOffsets = {};
    std::array<uint32_t, regionCount> frameCapacities = {};
    std::vector<Slot> slots;
    RhiBuffer* history = nullptr; // per instance slot, kept across frames
    FgAccessFlags historyAccess = FgAccessFlags::None;
    FgBufferHandle historyImport;
    bool historyImported = false;
    bool historyFresh = false;
    Readback latest;
};
