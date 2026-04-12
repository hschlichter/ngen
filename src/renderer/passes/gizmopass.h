#pragma once

#include "framegraph.h"
#include "rhitypes.h"

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

class RhiDevice;

class GizmoPass {
public:
    auto init(RhiDevice* device, uint32_t imageCount, RhiExtent2D extent, RhiFormat colorFormat) -> bool;
    auto destroy(RhiDevice* device) -> void;

    auto addPass(FrameGraph& fg, FgTextureHandle color, RhiExtent2D fullExtent, const glm::mat4& viewMatrix, uint32_t imageIndex) -> void;
    // Returns axis index (0=X, 1=Y, 2=Z) or -1. Sets `negative` if the back side of the axis was clicked.
    auto hitTest(float mouseX, float mouseY, RhiExtent2D windowExtent, bool& negative) const -> int;

private:
    glm::mat4 lastViewProj = glm::mat4(1.0f);
    glm::mat4 lastViewMatrix = glm::mat4(1.0f);
    RhiExtent2D lastFullExtent = {};
    RhiPipeline* pipeline = nullptr;
    RhiDescriptorSetLayout* descriptorSetLayout = nullptr;
    RhiDescriptorPool* descriptorPool = nullptr;
    std::vector<RhiDescriptorSet*> descriptorSets;
    RhiShaderModule* vertShader = nullptr;
    RhiShaderModule* fragShader = nullptr;
    RhiBuffer* vertexBuffer = nullptr;
    std::vector<RhiBuffer*> uniformBuffers;
    std::vector<void*> uniformBuffersMapped;
};
