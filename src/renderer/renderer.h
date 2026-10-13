#pragma once

#include "aapass.h"
#include "axis3dgizmo.h"
#include "capture.h"
#include "debugrenderer.h"
#include "debugviewpass.h"
#include "deletionqueue.h"
#include "depthprepass.h"
#include "editoruipass.h"
#include "framedebug.h"
#include "framegraph.h"
#include "framegraphdebug.h"
#include "framegraphpreviews.h"
#include "geometrypass.h"
#include "gizmopass.h"
#include "gpucounters.h"
#include "gpuscene.h"
#include "gpuuploader.h"
#include "instancecullpass.h"
#include "lightingpass.h"
#include "pipelineregistry.h"
#include "renderdebug.h"
#include "renderertypes.h"
#include "rendersnapshot.h"
#include "renderworld.h"
#include "resourcepool.h"
#include "rhitypes.h"
#include "screenshot.h"
#include "shadowpass.h"

#include <expected>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class AssetClient;
class RhiDevice;
class RhiSwapchain;
class RhiCommandBuffer;
class ImGuiBackend;
struct RenderSnapshot;

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = default;
    Renderer& operator=(Renderer&&) = default;
    ~Renderer() = default;

    // Creates the passes, which register their pipelines; no pipeline exists yet. `shaders` is where they come from.
    auto init(RhiDevice* rhiDevice, ImGuiBackend* imguiBackend, AssetClient* shaders, RhiExtent2D windowExtent) -> std::expected<void, int>;
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
    auto setFrameGraphDebugEnabled(bool enabled) -> void;
    auto buildFrameGraphDebugSnapshot() const -> FrameGraphDebugSnapshot;
    auto buildRenderDebugSnapshot() const -> RenderDebugSnapshot;
    auto setValidationEnabled(bool enabled) -> void { validationEnabled = enabled; }
    // Next presented frame is read back and written as PNG. Waits on that frame's fence once. The UI is left out of
    // that frame (it skips the editor UI pass) unless setScreenshotsShowUi(true).
    auto requestScreenshot(std::string path) -> void { screenshotPath = std::move(path); }
    auto setScreenshotsShowUi(bool show) -> void { screenshotsShowUi = show; }
    // Screenshots written since the last call, in order (after their fence).
    auto takeScreenshotResults() -> std::vector<ScreenshotResult> { return std::exchange(screenshotResults, {}); }
    // Render debugger inputs, set by the render thread before render().
    auto setRenderDebugEnabled(bool enabled) -> void { renderDebugEnabled = enabled; }
    auto setTextureInspect(const TextureInspectRequest& request) -> void { textureInspectRequest = request; }
    // Asset ids whose sources changed: the ones that are shaders are requested again and their pipelines recreated when they arrive.
    auto reloadShaders(std::span<const std::string> ids) -> void { pipelineRegistry.reload(ids); }
    // Writes one mip level of a texture (TextureHandle::index) as PNG after this frame's fence.
    auto requestTextureDump(uint32_t texture, uint32_t level, std::string path) -> void {
        textureDump = {.texture = texture, .level = level, .path = std::move(path)};
    }
    auto initGizmos(Camera* camera) -> void;
    auto gizmoUpdate(const RenderSnapshot& snapshot, RhiExtent2D extent) -> std::vector<GizmoDrawRequest>;
    auto gizmoHitTest(float mouseX, float mouseY, RhiExtent2D windowExtent) -> bool;
    // Latest GPU culling readback (one frame-slot cycle old), for the editor.
    auto cullResult() const -> CullResult;
    // Capture watches from the main thread, and the results that came back.
    auto setCaptureWatches(std::vector<CaptureWatch> watches) -> void { captureService.setWatches(std::move(watches)); }
    auto takeCaptureResults() -> std::vector<CaptureResult> { return captureService.takeResults(); }
    // Records the next frame with the command log on and keeps it as a FrameDebugCapture.
    auto requestFrameDebugCapture() -> void { frameDebugRequested = true; }
    auto takeFrameDebugCapture() -> std::optional<FrameDebugCapture> { return std::exchange(frameDebugResult, std::nullopt); }
    // GPU counters: one GpuCounters per completed frame while enabled; enabling also turns on
    // per-pass pipeline statistics.
    auto setCountersEnabled(bool enabled) -> void { countersEnabled = enabled; }
    auto takeCounters() -> std::vector<GpuCounters> { return std::exchange(countersResults, {}); }

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
    DrawLists drawLists;               // indirect commands per view and bucket, written by instanceCullPass
    InstanceCullPass instanceCullPass; // GPU culling
    CaptureService captureService;     // resource captures at pass boundaries
    bool frameDebugRequested = false;
    std::optional<FrameDebugCapture> frameDebugResult;
    bool countersEnabled = false;
    std::vector<GpuCounters> countersResults;
    std::vector<RhiPipelineStatsZone> pipelineStatsScratch;
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
    GizmoPass gizmoPass;
    Axis3DGizmo axisGizmo;
    EditorUIPass editorUIPass;

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
    struct PassGpuTime {
        const char* name;
        double ms;
    };
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
    FrameGraphPreviews fgPreviews;
    bool fgDebugEnabled = false;
    bool validationEnabled = false;
    bool firstFramePresented = false;
    bool renderDebugEnabled = false;
    std::string screenshotPath;
    bool screenshotsShowUi = false;
    std::vector<ScreenshotResult> screenshotResults;

    // Texture inspector: one level of one material
    // texture blitted into a preview the editor draws; replaced when the request changes.
    TextureInspectRequest textureInspectRequest;
    struct TexturePreview {
        RhiTexture* texture = nullptr;
        uint64_t imguiId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        bool everCaptured = false;
    };
    TexturePreview texturePreview;
    RenderDebugTextureInspect textureInspectResult;
    auto recordTextureInspect(RhiCommandBuffer* cmd) -> void;
    auto releaseTexturePreview() -> void;

    struct TextureDumpRequest {
        uint32_t texture = 0;
        uint32_t level = 0;
        std::string path;
    };
    std::optional<TextureDumpRequest> textureDump;
    std::vector<std::vector<FgDrawRecord>> slotDrawLogs; // per frame slot, joined with GPU zones after the fence
    std::vector<FgDrawRecord> lastDrawLog;
    // Last frame's lighting pick, kept for the debug snapshot.
    bool debugHasSun = false;
    LightingInputs debugSun;
    RhiExtent2D debugShadowExtent = {};

    ImGuiBackend* editorUI = nullptr; // owned by the application
};
