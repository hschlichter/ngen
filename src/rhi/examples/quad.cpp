// RHI example: vertex and index buffers.
//
// Adds to triangle: GPU-only buffers filled through a staging upload
// (common/upload.h), vertex attributes with a stride, and drawIndexed with both
// index types. Left quad is indexed with uint16, right quad with uint32.
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-quad --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"
#include "common/upload.h"

#include <array>
#include <cstddef>
#include <print>
#include <span>
#include <vector>

static constexpr const char* vertexShaderSource = R"glsl(
#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
}
)glsl";

static constexpr const char* fragmentShaderSource = R"glsl(
#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fragColor, 1.0);
}
)glsl";

struct Vertex {
    float x;
    float y;
    float r;
    float g;
    float b;
};

// Two flat-coloured quads, so the expected pixel value is the vertex colour itself.
static constexpr std::array<float, 3> leftColor = {0.2f, 0.8f, 0.2f};
static constexpr std::array<float, 3> rightColor = {0.2f, 0.4f, 0.9f};
static constexpr float quadHalfSize = 0.3f;
static constexpr float leftCenterX = -0.5f;
static constexpr float rightCenterX = 0.5f;

static auto makeQuad(float cx, float cy, std::array<float, 3> color) -> std::array<Vertex, 4> {
    return {{
        {cx - quadHalfSize, cy - quadHalfSize, color[0], color[1], color[2]},
        {cx + quadHalfSize, cy - quadHalfSize, color[0], color[1], color[2]},
        {cx + quadHalfSize, cy + quadHalfSize, color[0], color[1], color[2]},
        {cx - quadHalfSize, cy + quadHalfSize, color[0], color[1], color[2]},
    }};
}

class QuadExample : public RhiExample {
protected:
    auto setup() -> bool override {
        auto vertexSpirv = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "quad.vert");
        auto fragmentSpirv = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "quad.frag");
        if (vertexSpirv.empty() || fragmentSpirv.empty()) {
            return false;
        }
        vertexShader = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vertexSpirv});
        fragmentShader = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fragmentSpirv});

        // Vertex layout: the pipeline needs stride and per-attribute format and offset.
        std::array<RhiVertexAttribute, 2> attributes = {{
            {.location = 0, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = offsetof(Vertex, x)},
            {.location = 1, .binding = 0, .format = RhiFormat::R32G32B32_SFLOAT, .offset = offsetof(Vertex, r)},
        }};
        auto format = colorFormat();
        RhiGraphicsPipelineDesc pipelineDesc = {
            .vertexShader = vertexShader,
            .fragmentShader = fragmentShader,
            .colorFormats = {&format, 1},
            .vertexStride = sizeof(Vertex),
            .vertexAttributes = attributes,
            .raster = {.cullMode = RhiCullMode::None},
            .depth = {.testEnable = false, .writeEnable = false},
        };
        pipeline = device().createGraphicsPipeline(pipelineDesc);
        if (pipeline == nullptr) {
            return false;
        }

        // Geometry: 8 vertices in one buffer; two index buffers of different type.
        auto left = makeQuad(leftCenterX, 0.0f, leftColor);
        auto right = makeQuad(rightCenterX, 0.0f, rightColor);
        std::vector<Vertex> vertices;
        vertices.insert(vertices.end(), left.begin(), left.end());
        vertices.insert(vertices.end(), right.begin(), right.end());
        std::array<uint16_t, 6> indices16 = {0, 1, 2, 2, 3, 0};
        std::array<uint32_t, 6> indices32 = {4, 5, 6, 6, 7, 4};

        // Upload through a staging batch: one command buffer, one submit, one wait.
        UploadBatch upload(device());
        vertexBuffer = upload.buffer(std::as_bytes(std::span(vertices)), RhiBufferUsage::Vertex);
        indexBuffer16 = upload.buffer(std::as_bytes(std::span(indices16)), RhiBufferUsage::Index);
        indexBuffer32 = upload.buffer(std::as_bytes(std::span(indices32)), RhiBufferUsage::Index);
        upload.finish();
        return true;
    }

    auto record(RhiCommandBuffer* cmd, RhiTexture* backbuffer, RhiExtent2D extent) -> void override {
        std::array<RhiRenderingAttachmentInfo, 1> colorAttachments = {{
            {.texture = backbuffer, .state = RhiTextureState::ColorAttachment, .clear = true, .clearColor = clearColor},
        }};
        cmd->beginRendering({.extent = extent, .colorAttachments = colorAttachments});
        cmd->setViewport(extent);
        cmd->setScissor(extent);
        cmd->bindPipeline(pipeline);
        cmd->bindVertexBuffer(vertexBuffer);

        cmd->bindIndexBuffer(indexBuffer16, RhiIndexType::Uint16);
        cmd->drawIndexed(6, 1, 0, 0, 0);

        cmd->bindIndexBuffer(indexBuffer32, RhiIndexType::Uint32);
        cmd->drawIndexed(6, 1, 0, 0, 0);

        cmd->endRendering();
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        bool ok = true;
        ok = expectPixel(frame, frame.px(leftCenterX), frame.py(0.0f), leftColor, "left-quad-uint16") && ok;
        ok = expectPixel(frame, frame.px(rightCenterX), frame.py(0.0f), rightColor, "right-quad-uint32") && ok;
        ok = expectPixel(frame, frame.px(0.0f), frame.py(0.0f), {clearColor[0], clearColor[1], clearColor[2]}, "gap-between") && ok;
        return ok;
    }

    auto teardown() -> void override {
        device().destroyBuffer(indexBuffer32);
        device().destroyBuffer(indexBuffer16);
        device().destroyBuffer(vertexBuffer);
        device().destroyPipeline(pipeline);
        device().destroyShaderModule(fragmentShader);
        device().destroyShaderModule(vertexShader);
    }

private:
    RhiShaderModule* vertexShader = nullptr;
    RhiShaderModule* fragmentShader = nullptr;
    RhiPipeline* pipeline = nullptr;
    RhiBuffer* vertexBuffer = nullptr;
    RhiBuffer* indexBuffer16 = nullptr;
    RhiBuffer* indexBuffer32 = nullptr;
};

auto main(int argc, char** argv) -> int {
    QuadExample example;
    return example.run(argc, argv, "quad");
}
