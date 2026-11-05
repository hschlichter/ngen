#pragma once

#include "aapass.h"
#include "capture.h"
#include "debugrenderer.h"
#include "debugviewpass.h"
#include "deletionqueue.h"
#include "depthprepass.h"
#include "framegraph.h"
#include "framegraphdebug.h"
#include "geometrypass.h"
#include "gpuscene.h"
#include "gpuuploader.h"
#include "instancecullpass.h"
#include "lightingpass.h"
#include "lodgroups.h"
#include "lodselectpass.h"
#include "pipelineregistry.h"
#include "renderertypes.h"
#include "renderextension.h"
#include "rendersnapshot.h"
#include "renderworld.h"
#include "resourcepool.h"
#include "rhitypes.h"
#include "scenegpustats.h"
#include "shadowpass.h"

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

class AssetClient;
class PreviewTextures;
class RhiDevice;
class RhiSwapchain;
class RhiCommandBuffer;
struct RenderSnapshot;

// A pass's GPU time in the last frame read back.
struct PassGpuTime {
    const char* name;
    double ms;
};

// What the renderer holds and last did, for the instrumentation (src/rendertools): read on the render thread, between its hooks.
struct RendererState {
    uint64_t frame = 0; // the last frame recorded
    uint32_t slot = 0;  // the frame slot being recorded, or recorded last
    const RhiDevice* device = nullptr;
    RhiExtent2D swapchainExtent = {};
    RhiFormat swapchainFormat = RhiFormat::Undefined;
    uint32_t swapchainImages = 0;
    const GpuScene* gpuScene = nullptr;
    std::span<const GpuInstance> instances; // by draw slot
    uint32_t culledInstances = 0;           // by the camera, from the latest readback the last frame used
    uint32_t lightCount = 0;
    bool hasSun = false; // the lighting's pick in the last frame
    LightingInputs sun;
    RhiExtent2D shadowAtlasExtent = {};
    const ResourcePool* resourcePool = nullptr;
    std::span<const FgDrawRecord> drawLog; // the last frame read back's, while the draw log is on
    // The GPU's timings of the last frame read back: per pass, in all, and every zone.
    std::span<const PassGpuTime> passGpuTimes;
    double gpuFrameMs = -1.0;
    uint64_t gpuFrame = 0;
    std::span<const RhiGpuZone> gpuZones;
};

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = default;
    Renderer& operator=(Renderer&&) = default;
    ~Renderer() = default;

    // What joins the frame from outside the core (renderextension.h), called in the order added; and where previews are shown. Both
    // before init; the renderer holds references.
    auto addExtension(RenderExtension& extension) -> void;
    auto setPreviewTextures(PreviewTextures* previews) -> void { previewTextures = previews; }
    // Creates the passes, which register their pipelines, then inits the extensions; no pipeline exists yet. `shaders` is where they
    // come from.
    auto init(RhiDevice* rhiDevice, AssetClient* shaders, RhiExtent2D windowExtent) -> std::expected<void, int>;
    // Sends one request for the at-start pipelines' shaders; returns how many shaders it names. The pipelines are created as they arrive.
    auto requestShaders() -> size_t;
    // Whether a pipeline failed (its shader didn't pack, or the device couldn't create it).
    auto pipelinesFailed() const -> bool { return pipelineRegistry.anyFailed(); }
    // Thread-safe: every pipeline and its state, for the `pipelines` record.
    auto pipelineInfos() const -> std::vector<PipelineInfo> { return pipelineRegistry.infos(); }
    // Applies what changed in the scene to the GPU scene: meshes, textures and materials added and removed, draw slots changed, lights.
    // Uploads are queued and recorded a budget per frame; nothing waits.
    auto applySceneChanges(const SceneChanges& changes) -> void;
    // Records every queued upload and waits for it.
    auto flushSceneUploads() -> void;
    // Nothing of the scene left to upload, and no pipeline waiting for its shaders: the last frame drew everything applied.
    auto sceneGpuIdle() const -> bool { return gpuScene.idle() && !pipelineRegistry.anyPending(); }
    auto render(RenderSnapshot& snapshot) -> void;
    auto destroy() -> void;

    auto frameGraphRef() const -> const FrameGraph& { return frameGraph; }
    // For the instrumentation, on the render thread: what the renderer holds and last did.
    auto state() const -> RendererState;
    // The draw log (each indirect draw the frame recorded, with the commands the culling wrote) while on; off by default.
    auto setDrawLogEnabled(bool enabled) -> void { drawLogEnabled = enabled; }
    // Waits until the frame just submitted has finished on the GPU (an extension's afterSubmit).
    auto waitForFrame() -> void;
    // A frame slot's command buffer: what its frame recorded, read after its fence (pipeline statistics).
    auto commandBuffer(uint32_t slot) const -> RhiCommandBuffer* { return cmdBuffers[slot]; }
    // The most each geometry pool buffer may take, in bytes; 0 for the device's largest buffer (GpuScene::setGeometryLimit). Before the
    // render thread starts.
    auto setGeometryLimit(uint64_t bytes) -> void { gpuScene.setGeometryLimit(bytes); }
    auto setUploadBudget(uint64_t bytes) -> void { gpuScene.setUploadBudget(bytes); }
    // The GPU scene's draw slots, tables, upload queue and pool, after a frame (the change list counts are the render thread's).
    auto sceneGpuStats() const -> SceneGpuStats;
    // Asset ids whose sources changed: the ones that are shaders are requested again and their pipelines recreated when they arrive.
    auto reloadShaders(std::span<const std::string> ids) -> void { pipelineRegistry.reload(ids); }
    // Latest GPU culling readback (one frame-slot cycle old), for the editor.
    auto cullResult() const -> CullResult;
    // The capture results that came back (the watches arrive with the snapshot).
    auto takeCaptureResults() -> std::vector<CaptureResult> { return captureService.takeResults(); }

private:
    RhiDevice* device = nullptr;
    RhiSwapchain* swapchain = nullptr;

    // Shared resources
    RhiSampler* textureSampler = nullptr;
    RhiTexture* fallbackTexture = nullptr;
    RhiTexture* pendingTexture = nullptr; // white 1x1: a material's texture while it uploads

    // Main depth buffer. Renderer-owned (the swapchain only provides color images);
    // recreated with the swapchain. Immediate destroy is safe there because recreate waits idle.
    static constexpr RhiFormat depthFormat = RhiFormat::D32_SFLOAT;
    RhiTexture* depthTexture = nullptr;
    auto recreateDepthTexture(RhiExtent2D extent) -> void;
    std::vector<RhiBuffer*> uniformBuffers;
    std::vector<void*> uniformBuffersMapped;

    // Scene GPU resources
    std::vector<GpuInstance> gpuInstances; // by draw slot
    uint64_t sceneTriangles = 0;           // every draw slot's triangles, culled or not: kept as the slots change, not summed each frame
    bool descriptorSetsDirty = false;      // a buffer they name was replaced; rewritten at the next frame
    AABB sceneBounds;                      // union of instance world bounds, refreshed with gpuInstances
    uint32_t debugCulledInstances = 0;     // from the last snapshot, for the debug window
    RhiSampler* materialSampler = nullptr;
    RhiSampler* shadowSampler = nullptr; // compare sampler for the cascade atlas
    SamplerSettings materialSamplerSettings;

    static auto toSamplerDesc(const SamplerSettings& settings) -> RhiSamplerDesc;
    auto applySamplerSettings(const SamplerSettings& settings) -> void;
    // reason: geometry, sampler or instance_buffer (GeometryDescriptorsRebuilt observation).
    auto rebuildGeometryDescriptorSets() -> void;
    std::vector<RenderLight> lights;
    RhiDescriptorPool* geometryDescriptorPool = nullptr;
    std::vector<RhiDescriptorSet*> geometryDescriptorSets;

    // Scene GPU tables and the GPU-driven draw path (src/renderer/README.md).
    GpuScene gpuScene;
    DrawLists drawLists;                 // indirect commands per view and bucket, written by instanceCullPass
    InstanceCullPass instanceCullPass;   // GPU culling
    LodGroups lodGroups;                 // render-LOD groups: their table, the levels picked, the residency of their levels
    LodSelectPass lodSelectPass;         // the level each group draws, before culling
    uint32_t reportedRegionOverflow = 0; // the region overflow last traced (CullRegionOverflow), so a steady one is said once
    CaptureService captureService;       // resource captures at pass boundaries
    std::vector<RenderExtension*> extensions;
    PreviewTextures* previewTextures = nullptr; // owned by the UI
    auto nameSwapchainImages() -> void;
    uint32_t boundInstanceGeneration = 0; // instance buffer generation the descriptor sets point at

    // Passes
    ShadowPass shadowPass;
    DepthPrepass depthPrepass;
    GeometryPass geometryPass;
    DebugViewPass debugViewPass;
    LightingPass lightingPass;
    AAPass aaPass;
    DebugRenderer debugRenderer;

    // Frame sync
    std::vector<RhiCommandBuffer*> cmdBuffers;
    std::vector<RhiSemaphore*> imageAvailableSemaphores;
    std::vector<RhiSemaphore*> renderFinishedSemaphores;
    std::vector<RhiFence*> inflightFences;
    // Monotonic frame number last submitted through each slot; 0 = never.
    std::vector<uint64_t> slotFrame;
    std::vector<uint64_t> slotSubmitNs; // profile::now() at submit, matches GPU zones to CPU frames
    uint32_t currentFrame = 0;

    GpuUploader uploader;
    DeletionQueue deletionQueue;
    PipelineRegistry pipelineRegistry; // every pipeline the passes draw with

    // GPU timings come from the zones recorded on each slot's command buffer, read back
    // when the slot's fence has signalled, so results lag by the number of frames in flight.
    std::vector<PassGpuTime> lastGpuTimes;
    std::vector<RhiGpuZone> gpuZoneScratch;
    // GPU clock -> CPU clock: cpuNs = gpuNs - gpuClockAtCalibration + cpuClockAtCalibration.
    // Refreshed periodically; when the device cannot calibrate, GPU work is anchored at submit time.
    bool gpuClockCalibrated = false;
    uint64_t gpuClockAtCalibration = 0;
    uint64_t cpuClockAtCalibration = 0;
    uint64_t framesSinceCalibration = 0;
    double lastGpuFrameMs = -1.0;
    uint64_t lastGpuFrame = 0;
    auto readGpuTimings(uint32_t slot) -> void;

    // Monotonic frame counter — pre-incremented at the top of render(), so frame 0
    // never appears in observation streams (readers don't have to distinguish
    // "first frame" from "uninitialized"). Render-thread-only; don't read from
    // main.
    uint64_t m_frameIndex = 0;

    FrameGraph frameGraph;
    ResourcePool resourcePool;
    bool firstFramePresented = false;
    bool drawLogEnabled = false;
    // The frame recorded by render(), its parts after the fence wait and the acquire.
    auto renderFrame(RenderSnapshot& snapshot) -> void;
    std::vector<std::vector<FgDrawRecord>> slotDrawLogs; // per frame slot, joined with GPU zones after the fence
    std::vector<FgDrawRecord> lastDrawLog;
    // Last frame's lighting pick, kept for the debug snapshot.
    bool debugHasSun = false;
    LightingInputs debugSun;
    RhiExtent2D debugShadowExtent = {};
};
