#pragma once

#include "aapass.h"
#include "axis3dgizmo.h"
#include "debugrenderer.h"
#include "deletionqueue.h"
#include "depthprepass.h"
#include "editoruipass.h"
#include "framegraph.h"
#include "framegraphdebug.h"
#include "framegraphpreviews.h"
#include "geometrypass.h"
#include "gizmopass.h"
#include "gpuuploader.h"
#include "lightingpass.h"
#include "renderdebug.h"
#include "renderertypes.h"
#include "rendersnapshot.h"
#include "renderworld.h"
#include "resourcepool.h"
#include "rhitypes.h"
#include "shadowpass.h"

#include <expected>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class RhiDevice;
class RhiSwapchain;
class RhiCommandBuffer;
class ImGuiBackend;
class MeshLibrary;
class MaterialLibrary;
struct RenderSnapshot;

struct CachedTexture {
    RhiTexture* texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    uint64_t bytes = 0; // all levels
    RhiFormat format = RhiFormat::Undefined;
};

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = default;
    Renderer& operator=(Renderer&&) = default;
    ~Renderer() = default;

    auto init(RhiDevice* rhiDevice, ImGuiBackend* imguiBackend, RhiExtent2D windowExtent) -> std::expected<void, int>;
    auto uploadRenderWorld(const RenderWorld& world, const MeshLibrary& meshLib, const MaterialLibrary& matLib) -> void;
    auto render(RenderSnapshot& snapshot) -> void;
    auto destroy() -> void;

    auto frameGraphRef() const -> const FrameGraph& { return frameGraph; }
    auto setFrameGraphDebugEnabled(bool enabled) -> void;
    auto buildFrameGraphDebugSnapshot() const -> FrameGraphDebugSnapshot;
    auto buildRenderDebugSnapshot() const -> RenderDebugSnapshot;
    auto setValidationEnabled(bool enabled) -> void { validationEnabled = enabled; }
    // Next presented frame is read back and written as PNG. Waits on that frame's fence once.
    auto requestScreenshot(std::string path) -> void { screenshotPath = std::move(path); }
    // Render debugger inputs, set by the render thread before render().
    auto setRenderDebugEnabled(bool enabled) -> void { renderDebugEnabled = enabled; }
    auto setDrawTiming(const FgDrawTimingRequest& request) -> void { drawTimingRequest = request; }
    auto initGizmos(Camera* camera) -> void;
    auto gizmoUpdate(const RenderSnapshot& snapshot, RhiExtent2D extent) -> std::vector<GizmoDrawRequest>;
    auto gizmoHitTest(float mouseX, float mouseY, RhiExtent2D windowExtent) -> bool;

private:
    RhiDevice* device = nullptr;
    RhiSwapchain* swapchain = nullptr;

    // Shared resources
    RhiSampler* textureSampler = nullptr;
    RhiTexture* fallbackTexture = nullptr;

    // Main depth buffer. Renderer-owned (the swapchain only provides color images);
    // recreated with the swapchain. Immediate destroy is safe there because recreate waits idle.
    static constexpr RhiFormat depthFormat = RhiFormat::D32_SFLOAT;
    RhiTexture* depthTexture = nullptr;
    auto recreateDepthTexture(RhiExtent2D extent) -> void;
    std::vector<RhiBuffer*> uniformBuffers;
    std::vector<void*> uniformBuffersMapped;

    // Scene GPU resources
    std::unordered_map<uint32_t, CachedMesh> meshCache;
    std::unordered_map<uint32_t, CachedTexture> textureCache;
    std::vector<GpuInstance> gpuInstances;
    AABB sceneBounds; // union of instance world bounds, refreshed with gpuInstances
    uint32_t debugCulledInstances = 0; // from the last snapshot, for RenderStats and the debug window
    RhiSampler* materialSampler = nullptr;
    SamplerSettings materialSamplerSettings;

    static auto toSamplerDesc(const SamplerSettings& settings) -> RhiSamplerDesc;
    auto applySamplerSettings(const SamplerSettings& settings) -> void;
    auto rebuildGeometryDescriptorSets() -> void;
    std::vector<RenderLight> lights;
    RhiDescriptorPool* geometryDescriptorPool = nullptr;
    std::vector<RhiDescriptorSet*> geometryDescriptorSets;

    // Passes
    ShadowPass shadowPass;
    DepthPrepass depthPrepass;
    GeometryPass geometryPass;
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
    bool lastAntiAliasing = true;
    bool validationEnabled = false;
    bool renderDebugEnabled = false;
    std::string screenshotPath;
    FgDrawTimingRequest drawTimingRequest;
    std::vector<std::vector<FgDrawRecord>> slotDrawLogs; // per frame slot, joined with GPU zones after the fence
    std::vector<FgDrawRecord> lastDrawLog;
    // Last frame's lighting pick, kept for the debug snapshot.
    bool debugHasSun = false;
    LightingInputs debugSun;
    RhiExtent2D debugShadowExtent = {};

    ImGuiBackend* editorUI = nullptr; // owned by the application
};
