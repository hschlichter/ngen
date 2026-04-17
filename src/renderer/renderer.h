#pragma once

#include "axis3dgizmo.h"
#include "debugrenderer.h"
#include "editoruipass.h"
#include "framegraph.h"
#include "framegraphdebug.h"
#include "framegraphpreviews.h"
#include "geometrypass.h"
#include "gizmopass.h"
#include "lightingpass.h"
#include "renderertypes.h"
#include "renderworld.h"
#include "resourcepool.h"
#include "rhitypes.h"

#include <expected>
#include <memory>
#include <unordered_map>
#include <vector>

class RhiDevice;
class RhiSwapchain;
class RhiCommandBuffer;
class RhiEditorUI;
class MeshLibrary;
class MaterialLibrary;
struct RenderSnapshot;
struct SDL_Window;

struct CachedTexture {
    RhiTexture* texture = nullptr;
};

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = default;
    Renderer& operator=(Renderer&&) = default;
    ~Renderer() = default;

    auto init(RhiDevice* rhiDevice, SDL_Window* window) -> std::expected<void, int>;
    auto uploadRenderWorld(const RenderWorld& world, const MeshLibrary& meshLib, const MaterialLibrary& matLib) -> void;
    auto render(RenderSnapshot& snapshot) -> void;
    auto destroy() -> void;

    auto editorui() -> RhiEditorUI* { return editorUI.get(); }
    auto frameGraphRef() const -> const FrameGraph& { return frameGraph; }
    auto setFrameGraphDebugEnabled(bool enabled) -> void;
    auto buildFrameGraphDebugSnapshot() const -> FrameGraphDebugSnapshot;
    auto initGizmos(Camera* camera) -> void;
    auto gizmoUpdate(const RenderSnapshot& snapshot, RhiExtent2D extent) -> std::vector<GizmoDrawRequest>;
    auto gizmoHitTest(float mouseX, float mouseY, RhiExtent2D windowExtent) -> bool;

private:
    RhiDevice* device = nullptr;
    RhiSwapchain* swapchain = nullptr;

    // Shared resources
    RhiSampler* textureSampler = nullptr;
    RhiTexture* fallbackTexture = nullptr;
    std::vector<RhiBuffer*> uniformBuffers;
    std::vector<void*> uniformBuffersMapped;

    // Scene GPU resources
    std::unordered_map<uint32_t, CachedMesh> meshCache;
    std::unordered_map<uint32_t, CachedTexture> textureCache;
    std::vector<GpuInstance> gpuInstances;
    std::vector<RenderLight> lights;
    RhiDescriptorPool* geometryDescriptorPool = nullptr;
    std::vector<RhiDescriptorSet*> geometryDescriptorSets;

    // Passes
    GeometryPass geometryPass;
    LightingPass lightingPass;
    DebugRenderer debugRenderer;
    GizmoPass gizmoPass;
    Axis3DGizmo axisGizmo;
    EditorUIPass editorUIPass;

    // Frame sync
    std::vector<RhiCommandBuffer*> cmdBuffers;
    std::vector<RhiSemaphore*> imageAvailableSemaphores;
    std::vector<RhiSemaphore*> renderFinishedSemaphores;
    std::vector<RhiFence*> inflightFences;
    uint32_t currentFrame = 0;

    FrameGraph frameGraph;
    ResourcePool resourcePool;
    FrameGraphPreviews fgPreviews;
    bool fgDebugEnabled = false;

    std::unique_ptr<RhiEditorUI> editorUI;
};
