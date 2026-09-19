// RHI example: blend state, cull mode, front face.
//
// Adds to quad: pipelines that differ only in RhiBlendState and RhiRasterState.
// An opaque background quad is drawn first; five half-transparent quads on top:
//
//   x=-0.8  alpha blend        result = 0.5*src + 0.5*dst
//   x=-0.4  additive           result = src + dst, clamped
//   x= 0.0  blend off          result = src
//   x= 0.4  cull back, CCW front   quad winds clockwise in framebuffer space -> culled, background shows
//   x= 0.8  cull back, CW front    same winding is now front-facing -> visible, result = src
//
// Blending happens in linear space; --check computes the formula in linear and
// lets expectPixel encode for the swapchain format.
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-blend --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"
#include "common/upload.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <print>
#include <span>
#include <vector>

static constexpr const char* vertexShaderSource = R"glsl(
#version 450
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 fragColor;
void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
}
)glsl";

static constexpr const char* fragmentShaderSource = R"glsl(
#version 450
layout(location = 0) in vec4 fragColor;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = fragColor;
}
)glsl";

struct Vertex {
    float x;
    float y;
    float r;
    float g;
    float b;
    float a;
};

static constexpr std::array<float, 4> background = {0.2f, 0.2f, 0.6f, 1.0f};
static constexpr std::array<float, 4> overlay = {0.9f, 0.6f, 0.1f, 0.5f};
static constexpr std::array<float, 5> columnX = {-0.8f, -0.4f, 0.0f, 0.4f, 0.8f};
static constexpr float quadHalf = 0.15f;

static auto pushQuad(std::vector<Vertex>& out, float cx, float cy, float hx, float hy, std::array<float, 4> c) -> void {
    // Top-left, top-right, bottom-right, bottom-left in framebuffer space (y down):
    // this order is clockwise on screen.
    out.push_back({cx - hx, cy - hy, c[0], c[1], c[2], c[3]});
    out.push_back({cx + hx, cy - hy, c[0], c[1], c[2], c[3]});
    out.push_back({cx + hx, cy + hy, c[0], c[1], c[2], c[3]});
    out.push_back({cx - hx, cy + hy, c[0], c[1], c[2], c[3]});
}

class BlendExample : public RhiExample {
protected:
    auto setup() -> bool override {
        auto vertexSpirv = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "blend.vert");
        auto fragmentSpirv = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "blend.frag");
        if (vertexSpirv.empty() || fragmentSpirv.empty()) {
            return false;
        }
        vertexShader = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vertexSpirv});
        fragmentShader = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fragmentSpirv});

        std::array<RhiVertexAttribute, 2> attributes = {{
            {.location = 0, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = offsetof(Vertex, x)},
            {.location = 1, .binding = 0, .format = RhiFormat::R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, r)},
        }};
        auto format = colorFormat();

        struct Variant {
            RhiBlendState blend;
            RhiRasterState raster;
        };
        std::array<Variant, 5> variants = {{
            {.blend = {.enable = true}, .raster = {.cullMode = RhiCullMode::None}},
            {.blend = {.enable = true, .srcColor = RhiBlendFactor::One, .dstColor = RhiBlendFactor::One}, .raster = {.cullMode = RhiCullMode::None}},
            {.blend = {.enable = false}, .raster = {.cullMode = RhiCullMode::None}},
            {.blend = {.enable = false}, .raster = {.cullMode = RhiCullMode::Back, .frontFace = RhiFrontFace::CounterClockwise}},
            {.blend = {.enable = false}, .raster = {.cullMode = RhiCullMode::Back, .frontFace = RhiFrontFace::Clockwise}},
        }};
        for (size_t i = 0; i < variants.size(); i++) {
            RhiGraphicsPipelineDesc pipelineDesc = {
                .vertexShader = vertexShader,
                .fragmentShader = fragmentShader,
                .colorFormats = {&format, 1},
                .vertexStride = sizeof(Vertex),
                .vertexAttributes = attributes,
                .raster = variants[i].raster,
                .depth = {.testEnable = false, .writeEnable = false},
                .blend = variants[i].blend,
            };
            pipelines[i] = device().createGraphicsPipeline(pipelineDesc);
            if (pipelines[i] == nullptr) {
                return false;
            }
        }

        // Vertex 0-3 background, then one quad per column.
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        pushQuad(vertices, 0.0f, 0.0f, 0.95f, 0.3f, background);
        for (auto cx : columnX) {
            pushQuad(vertices, cx, 0.0f, quadHalf, quadHalf, overlay);
        }
        for (uint16_t q = 0; q < 6; q++) {
            for (auto i : {0, 1, 2, 2, 3, 0}) {
                indices.push_back((uint16_t) (q * 4 + i));
            }
        }
        UploadBatch upload(device());
        vertexBuffer = upload.buffer(std::as_bytes(std::span(vertices)), RhiBufferUsage::Vertex);
        indexBuffer = upload.buffer(std::as_bytes(std::span(indices)), RhiBufferUsage::Index);
        upload.finish();
        return true;
    }

    auto record(RhiCommandBuffer* cmd, RhiTexture* backbuffer, RhiExtent2D extent) -> void override {
        std::array<RhiRenderingAttachmentInfo, 1> colorAttachments = {{
            {.texture = backbuffer, .layout = RhiImageLayout::ColorAttachment, .clear = true, .clearColor = clearColor},
        }};
        cmd->beginRendering({.extent = extent, .colorAttachments = colorAttachments});
        cmd->setViewport(extent);
        cmd->setScissor(extent);
        cmd->bindVertexBuffer(vertexBuffer);
        cmd->bindIndexBuffer(indexBuffer, RhiIndexType::Uint16);

        // Background with the plain pipeline (blend off, no culling).
        cmd->bindPipeline(pipelines[2]);
        cmd->drawIndexed(6, 1, 0, 0, 0);

        for (uint32_t column = 0; column < 5; column++) {
            cmd->bindPipeline(pipelines[column]);
            cmd->drawIndexed(6, 1, (column + 1) * 6, 0, 0);
        }
        cmd->endRendering();
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        auto alphaBlend = std::array<float, 3>{};
        auto additive = std::array<float, 3>{};
        for (size_t c = 0; c < 3; c++) {
            alphaBlend[c] = overlay[c] * overlay[3] + background[c] * (1.0f - overlay[3]);
            additive[c] = std::min(overlay[c] + background[c], 1.0f);
        }
        std::array<float, 3> src = {overlay[0], overlay[1], overlay[2]};
        std::array<float, 3> dst = {background[0], background[1], background[2]};
        auto y = frame.py(0.0f);
        bool ok = true;
        ok = expectPixel(frame, frame.px(columnX[0]), y, alphaBlend, "alpha-blend") && ok;
        ok = expectPixel(frame, frame.px(columnX[1]), y, additive, "additive") && ok;
        ok = expectPixel(frame, frame.px(columnX[2]), y, src, "blend-off") && ok;
        ok = expectPixel(frame, frame.px(columnX[3]), y, dst, "cull-back-ccw-front-culled") && ok;
        ok = expectPixel(frame, frame.px(columnX[4]), y, src, "cull-back-cw-front-visible") && ok;
        return ok;
    }

    auto teardown() -> void override {
        device().destroyBuffer(indexBuffer);
        device().destroyBuffer(vertexBuffer);
        for (auto* pipeline : pipelines) {
            device().destroyPipeline(pipeline);
        }
        device().destroyShaderModule(fragmentShader);
        device().destroyShaderModule(vertexShader);
    }

private:
    RhiShaderModule* vertexShader = nullptr;
    RhiShaderModule* fragmentShader = nullptr;
    std::array<RhiPipeline*, 5> pipelines = {};
    RhiBuffer* vertexBuffer = nullptr;
    RhiBuffer* indexBuffer = nullptr;
};

auto main(int argc, char** argv) -> int {
    BlendExample example;
    return example.run(argc, argv, "blend");
}
