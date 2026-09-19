// RHI example: render to texture, sample it, blit it.
//
// Adds to texture: an offscreen colour texture used first as a render target
// (pass 1 draws the triangle into it), then as a sampled texture (pass 2 draws
// it on a quad), plus a blitTexture copy into a second, smaller texture drawn
// on a second quad. Every layout transition is explicit:
//
//   target:  Undefined -> ColorAttachment -> ShaderReadOnly -> TransferSrc -> ShaderReadOnly
//   blitDst: Undefined -> TransferDst -> ShaderReadOnly
//
// The offscreen target has its own size and clear colour, so --check can tell it
// apart from the swapchain.
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-rendertarget --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"
#include "common/upload.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <print>
#include <span>
#include <vector>

// Pass 1: the triangle from the first example, drawn into the offscreen target.
static constexpr const char* triangleVertexSource = R"glsl(
#version 450
layout(location = 0) out vec3 fragColor;
const vec2 positions[3] = vec2[](vec2(0.0, -0.6), vec2(0.6, 0.6), vec2(-0.6, 0.6));
const vec3 colors[3] = vec3[](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2), vec3(0.2, 0.2, 1.0));
void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragColor = colors[gl_VertexIndex];
}
)glsl";

static constexpr const char* flatFragmentSource = R"glsl(
#version 450
layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(fragColor, 1.0);
}
)glsl";

// Pass 2: textured quads onto the swapchain.
static constexpr const char* quadVertexSource = R"glsl(
#version 450
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec2 fragUv;
void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    fragUv = inUv;
}
)glsl";

static constexpr const char* texturedFragmentSource = R"glsl(
#version 450
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 fragUv;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = texture(tex, fragUv);
}
)glsl";

struct Vertex {
    float x;
    float y;
    float u;
    float v;
};

static constexpr RhiExtent2D targetExtent = {512, 512};
static constexpr RhiExtent2D blitExtent = {256, 256};
static constexpr RhiFormat targetFormat = RhiFormat::R8G8B8A8_UNORM;
static constexpr std::array<float, 4> targetClear = {0.1f, 0.3f, 0.1f, 1.0f};
// Triangle colour at the target centre, see triangle.cpp for the derivation.
static constexpr std::array<float, 3> triangleCenter = {0.6f, 0.4f, 0.4f};
static constexpr float quadHalf = 0.4f;
static constexpr std::array<float, 2> quadCenterX = {-0.5f, 0.5f};

class RenderTargetExample : public RhiExample {
protected:
    auto setup() -> bool override {
        auto triVs = compileGlsl(RhiShaderStage::Vertex, triangleVertexSource, "rt-triangle.vert");
        auto flatFs = compileGlsl(RhiShaderStage::Fragment, flatFragmentSource, "rt-flat.frag");
        auto quadVs = compileGlsl(RhiShaderStage::Vertex, quadVertexSource, "rt-quad.vert");
        auto texFs = compileGlsl(RhiShaderStage::Fragment, texturedFragmentSource, "rt-textured.frag");
        if (triVs.empty() || flatFs.empty() || quadVs.empty() || texFs.empty()) {
            return false;
        }
        shaders[0] = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = triVs});
        shaders[1] = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = flatFs});
        shaders[2] = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = quadVs});
        shaders[3] = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = texFs});

        // Offscreen textures. Usage lists every role each one plays.
        RhiTextureDesc targetDesc = {
            .width = targetExtent.width,
            .height = targetExtent.height,
            .format = targetFormat,
            .usage = RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled | RhiTextureUsage::TransferSrc,
        };
        target = device().createTexture(targetDesc);
        RhiTextureDesc blitDesc = {
            .width = blitExtent.width,
            .height = blitExtent.height,
            .format = targetFormat,
            .usage = RhiTextureUsage::Sampled | RhiTextureUsage::TransferDst,
        };
        blitTarget = device().createTexture(blitDesc);

        // Pass 1 pipeline renders into the target's format, not the swapchain's.
        RhiGraphicsPipelineDesc trianglePipelineDesc = {
            .vertexShader = shaders[0],
            .fragmentShader = shaders[1],
            .colorFormats = {&targetFormat, 1},
            .raster = {.cullMode = RhiCullMode::None},
            .depth = {.testEnable = false, .writeEnable = false},
        };
        trianglePipeline = device().createGraphicsPipeline(trianglePipelineDesc);

        std::array<RhiDescriptorBinding, 1> bindings = {{
            {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Fragment},
        }};
        setLayout = device().createDescriptorSetLayout(bindings);
        std::array<RhiVertexAttribute, 2> attributes = {{
            {.location = 0, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = offsetof(Vertex, x)},
            {.location = 1, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = offsetof(Vertex, u)},
        }};
        auto swapchainFormat = colorFormat();
        RhiGraphicsPipelineDesc quadPipelineDesc = {
            .vertexShader = shaders[2],
            .fragmentShader = shaders[3],
            .descriptorSetLayouts = {&setLayout, 1},
            .colorFormats = {&swapchainFormat, 1},
            .vertexStride = sizeof(Vertex),
            .vertexAttributes = attributes,
            .raster = {.cullMode = RhiCullMode::None},
            .depth = {.testEnable = false, .writeEnable = false},
        };
        quadPipeline = device().createGraphicsPipeline(quadPipelineDesc);
        if (trianglePipeline == nullptr || quadPipeline == nullptr) {
            return false;
        }

        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        for (auto cx : quadCenterX) {
            auto base = (uint16_t) vertices.size();
            vertices.push_back({cx - quadHalf, -quadHalf, 0.0f, 0.0f});
            vertices.push_back({cx + quadHalf, -quadHalf, 1.0f, 0.0f});
            vertices.push_back({cx + quadHalf, quadHalf, 1.0f, 1.0f});
            vertices.push_back({cx - quadHalf, quadHalf, 0.0f, 1.0f});
            for (auto i : {0, 1, 2, 2, 3, 0}) {
                indices.push_back((uint16_t) (base + i));
            }
        }
        {
            UploadBatch upload(device());
            vertexBuffer = upload.buffer(std::as_bytes(std::span(vertices)), RhiBufferUsage::Vertex);
            indexBuffer = upload.buffer(std::as_bytes(std::span(indices)), RhiBufferUsage::Index);
            upload.finish();
        }

        sampler = device().createSampler({.magFilter = RhiFilter::Nearest, .minFilter = RhiFilter::Nearest});
        pool = device().createDescriptorPool(2, bindings);
        descriptorSets = device().allocateDescriptorSets(pool, setLayout, 2);
        std::array<RhiTexture*, 2> sampled = {target, blitTarget};
        for (uint32_t i = 0; i < 2; i++) {
            std::array<RhiDescriptorWrite, 1> writes = {{
                {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .texture = sampled[i], .sampler = sampler},
            }};
            device().updateDescriptorSet(descriptorSets[i], writes);
        }
        return true;
    }

    auto record(RhiCommandBuffer* cmd, RhiTexture* backbuffer, RhiExtent2D extent) -> void override {
        // Pass 1: triangle into the offscreen target.
        std::array<RhiBarrierDesc, 1> targetToColor = {{
            {.texture = target, .oldLayout = RhiImageLayout::Undefined, .newLayout = RhiImageLayout::ColorAttachment},
        }};
        cmd->pipelineBarrier(targetToColor);
        std::array<RhiRenderingAttachmentInfo, 1> targetAttachment = {{
            {.texture = target, .layout = RhiImageLayout::ColorAttachment, .clear = true, .clearColor = targetClear},
        }};
        cmd->beginRendering({.extent = targetExtent, .colorAttachments = targetAttachment});
        cmd->setViewport(targetExtent);
        cmd->setScissor(targetExtent);
        cmd->bindPipeline(trianglePipeline);
        cmd->draw(3, 1, 0, 0);
        cmd->endRendering();

        // Blit: target becomes a transfer source, blitDst a transfer destination,
        // then both become sampled textures. The blit also downsamples 512 -> 256.
        std::array<RhiBarrierDesc, 2> toTransfer = {{
            {.texture = target, .oldLayout = RhiImageLayout::ColorAttachment, .newLayout = RhiImageLayout::TransferSrc},
            {.texture = blitTarget, .oldLayout = RhiImageLayout::Undefined, .newLayout = RhiImageLayout::TransferDst},
        }};
        cmd->pipelineBarrier(toTransfer);
        cmd->blitTexture(target, blitTarget, targetExtent, blitExtent);
        std::array<RhiBarrierDesc, 2> toSampled = {{
            {.texture = target, .oldLayout = RhiImageLayout::TransferSrc, .newLayout = RhiImageLayout::ShaderReadOnly},
            {.texture = blitTarget, .oldLayout = RhiImageLayout::TransferDst, .newLayout = RhiImageLayout::ShaderReadOnly},
        }};
        cmd->pipelineBarrier(toSampled);

        // Pass 2: both textures onto the swapchain.
        std::array<RhiRenderingAttachmentInfo, 1> colorAttachments = {{
            {.texture = backbuffer, .layout = RhiImageLayout::ColorAttachment, .clear = true, .clearColor = clearColor},
        }};
        cmd->beginRendering({.extent = extent, .colorAttachments = colorAttachments});
        cmd->setViewport(extent);
        cmd->setScissor(extent);
        cmd->bindPipeline(quadPipeline);
        cmd->bindVertexBuffer(vertexBuffer);
        cmd->bindIndexBuffer(indexBuffer, RhiIndexType::Uint16);
        for (uint32_t q = 0; q < 2; q++) {
            cmd->bindDescriptorSet(quadPipeline, 0, descriptorSets[q]);
            cmd->drawIndexed(6, 1, q * 6, 0, 0);
        }
        cmd->endRendering();
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        std::array<float, 3> targetClearRgb = {targetClear[0], targetClear[1], targetClear[2]};
        bool ok = true;
        // Quad centre samples the target centre: the triangle's interpolated colour.
        ok = expectPixel(frame, frame.px(quadCenterX[0]), frame.py(0.0f), triangleCenter, "rendered-target-centre") && ok;
        ok = expectPixel(frame, frame.px(quadCenterX[1]), frame.py(0.0f), triangleCenter, "blitted-copy-centre") && ok;
        // Quad corner samples the target corner: the offscreen clear, not the swapchain clear.
        ok = expectPixel(frame, frame.px(quadCenterX[0] - quadHalf * 0.9f), frame.py(-quadHalf * 0.9f), targetClearRgb, "rendered-target-clear") && ok;
        ok = expectPixel(frame, frame.px(quadCenterX[1] - quadHalf * 0.9f), frame.py(-quadHalf * 0.9f), targetClearRgb, "blitted-copy-clear") && ok;
        return ok;
    }

    auto teardown() -> void override {
        device().destroyDescriptorPool(pool);
        for (auto* set : descriptorSets) {
            delete set;
        }
        device().destroySampler(sampler);
        device().destroyBuffer(indexBuffer);
        device().destroyBuffer(vertexBuffer);
        device().destroyPipeline(quadPipeline);
        device().destroyPipeline(trianglePipeline);
        device().destroyDescriptorSetLayout(setLayout);
        device().destroyTexture(blitTarget);
        device().destroyTexture(target);
        for (auto* shader : shaders) {
            device().destroyShaderModule(shader);
        }
    }

private:
    std::array<RhiShaderModule*, 4> shaders = {};
    RhiTexture* target = nullptr;
    RhiTexture* blitTarget = nullptr;
    RhiPipeline* trianglePipeline = nullptr;
    RhiPipeline* quadPipeline = nullptr;
    RhiDescriptorSetLayout* setLayout = nullptr;
    RhiBuffer* vertexBuffer = nullptr;
    RhiBuffer* indexBuffer = nullptr;
    RhiSampler* sampler = nullptr;
    RhiDescriptorPool* pool = nullptr;
    std::vector<RhiDescriptorSet*> descriptorSets;
};

auto main(int argc, char** argv) -> int {
    RenderTargetExample example;
    return example.run(argc, argv, "rendertarget");
}
