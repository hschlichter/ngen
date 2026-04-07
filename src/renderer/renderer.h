#pragma once

#include "debugrenderer.h"
#include "framegraph.h"
#include "renderworld.h"
#include "resourcepool.h"
#include "rhitypes.h"

#include <expected>
#include <glm/glm.hpp>
#include <memory>
#include <vector>

class RhiDevice;
class RhiSwapchain;
class RhiCommandBuffer;
class RhiDebugUI;
class MeshLibrary;
class MaterialLibrary;
struct Camera;
struct DebugDrawData;
struct SDL_Window;

struct UniformBufferObject {
    glm::mat4 view;
    glm::mat4 proj;
};

struct GpuMesh {
    RhiBuffer* vertexBuffer;
    RhiBuffer* indexBuffer;
    uint32_t indexCount;
    RhiTexture* texture;
    glm::mat4 transform;
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
    auto render(const Camera& camera, SDL_Window* window, const DebugDrawData& debugData) -> void;
    auto destroy() -> void;

    auto debugui() -> RhiDebugUI* { return debugUI.get(); }

private:
    RhiDevice* device = nullptr;
    RhiSwapchain* swapchain = nullptr;

    // Forward pass
    RhiPipeline* pipeline = nullptr;
    RhiDescriptorSetLayout* descriptorSetLayout = nullptr;
    RhiDescriptorPool* descriptorPool = nullptr;
    std::vector<RhiDescriptorSet*> descriptorSets;
    RhiSampler* textureSampler = nullptr;
    RhiShaderModule* vertShader = nullptr;
    RhiShaderModule* fragShader = nullptr;

    std::vector<GpuMesh> gpuMeshes;
    std::vector<RhiBuffer*> uniformBuffers;
    std::vector<void*> uniformBuffersMapped;

    // Debug line pass
    RhiPipeline* debugLinePipeline = nullptr;
    RhiDescriptorSetLayout* debugDescriptorSetLayout = nullptr;
    RhiDescriptorPool* debugDescriptorPool = nullptr;
    std::vector<RhiDescriptorSet*> debugDescriptorSets;
    RhiShaderModule* debugVertShader = nullptr;
    RhiShaderModule* debugFragShader = nullptr;
    std::vector<RhiBuffer*> debugVertexBuffers;
    std::vector<void*> debugVertexBuffersMapped;
    static constexpr uint32_t debugMaxVertices = 65536;

    // Frame sync
    std::vector<RhiCommandBuffer*> cmdBuffers;
    std::vector<RhiSemaphore*> imageAvailableSemaphores;
    std::vector<RhiSemaphore*> renderFinishedSemaphores;
    std::vector<RhiFence*> inflightFences;
    uint32_t currentFrame = 0;

    FrameGraph frameGraph;
    ResourcePool resourcePool;

    std::unique_ptr<RhiDebugUI> debugUI;
    DebugRenderer debugRenderer;
};
