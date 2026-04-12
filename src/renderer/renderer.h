#pragma once

#include "debugrenderer.h"
#include "framegraph.h"
#include "geometrypass.h"
#include "lightingpass.h"
#include "renderertypes.h"
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
struct Camera;
struct DebugDrawData;
struct RenderWorld;
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
    auto render(const Camera& camera, SDL_Window* window, const DebugDrawData& debugData, const RenderWorld& world) -> void;
    auto destroy() -> void;

    auto editorui() -> RhiEditorUI* { return editorUI.get(); }
    inline auto gbufferViewMode() -> GBufferView& { return gbufferView; }
    inline auto bufferOverlayEnabled() -> bool& { return showBufferOverlay; }

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
    RhiDescriptorPool* geometryDescriptorPool = nullptr;
    std::vector<RhiDescriptorSet*> geometryDescriptorSets;

    // Passes
    GeometryPass geometryPass;
    LightingPass lightingPass;
    DebugRenderer debugRenderer;

    // Frame sync
    std::vector<RhiCommandBuffer*> cmdBuffers;
    std::vector<RhiSemaphore*> imageAvailableSemaphores;
    std::vector<RhiSemaphore*> renderFinishedSemaphores;
    std::vector<RhiFence*> inflightFences;
    uint32_t currentFrame = 0;

    GBufferView gbufferView = GBufferView::Lit;
    bool showBufferOverlay = false;

    FrameGraph frameGraph;
    ResourcePool resourcePool;

    std::unique_ptr<RhiEditorUI> editorUI;
};
