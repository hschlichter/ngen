// RHI example: line topology and line width.
//
// Adds to quad: RhiPrimitiveTopology::LineList and RhiRasterState::lineWidth.
// Wide lines are an optional device feature; the example reads limits() and
// falls back to 1 px, and --check adapts its expectations to what was used.
//
//   y=-0.3  wide line  (4 px when supported, else 1 px)
//   y= 0.3  thin line  (1 px)
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-lines --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"
#include "common/upload.h"

#include <algorithm>
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

static constexpr std::array<float, 3> wideColor = {0.9f, 0.8f, 0.2f};
static constexpr std::array<float, 3> thinColor = {0.2f, 0.8f, 0.9f};
// Placed so the line centre lands on a pixel centre at the default 720 px height
// (and at 360 after --resize-at); a thin line then covers exactly one known row.
static constexpr float wideY = -0.3f + (0.5f / 360.0f);
static constexpr float thinY = 0.3f + (0.5f / 360.0f);

class LinesExample : public RhiExample {
protected:
    auto setup() -> bool override {
        auto vertexSpirv = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "lines.vert");
        auto fragmentSpirv = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "lines.frag");
        if (vertexSpirv.empty() || fragmentSpirv.empty()) {
            return false;
        }
        vertexShader = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vertexSpirv});
        fragmentShader = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fragmentSpirv});

        const auto& limits = device().limits();
        wideWidth = limits.wideLines ? std::min(4.0f, limits.maxLineWidth) : 1.0f;
        std::println("wide lines {}, max width {}, using {} px", limits.wideLines ? "supported" : "not supported", limits.maxLineWidth, wideWidth);

        std::array<RhiVertexAttribute, 2> attributes = {{
            {.location = 0, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = offsetof(Vertex, x)},
            {.location = 1, .binding = 0, .format = RhiFormat::R32G32B32_SFLOAT, .offset = offsetof(Vertex, r)},
        }};
        auto format = colorFormat();
        std::array<float, 2> widths = {wideWidth, 1.0f};
        for (size_t i = 0; i < 2; i++) {
            RhiGraphicsPipelineDesc pipelineDesc = {
                .vertexShader = vertexShader,
                .fragmentShader = fragmentShader,
                .colorFormats = {&format, 1},
                .vertexStride = sizeof(Vertex),
                .vertexAttributes = attributes,
                .topology = RhiPrimitiveTopology::LineList,
                .raster = {.cullMode = RhiCullMode::None, .lineWidth = widths[i]},
                .depth = {.testEnable = false, .writeEnable = false},
            };
            pipelines[i] = device().createGraphicsPipeline(pipelineDesc);
            if (pipelines[i] == nullptr) {
                return false;
            }
        }

        // Two horizontal lines across the window, plus a diagonal for the eye.
        std::vector<Vertex> vertices = {
            {.x=-0.95f, .y=wideY, .r=wideColor[0], .g=wideColor[1], .b=wideColor[2]},
            {.x=0.95f, .y=wideY, .r=wideColor[0], .g=wideColor[1], .b=wideColor[2]},
            {.x=-0.95f, .y=thinY, .r=thinColor[0], .g=thinColor[1], .b=thinColor[2]},
            {.x=0.95f, .y=thinY, .r=thinColor[0], .g=thinColor[1], .b=thinColor[2]},
            {.x=-0.95f, .y=0.9f, .r=thinColor[0], .g=thinColor[1], .b=thinColor[2]},
            {.x=0.95f, .y=-0.9f, .r=wideColor[0], .g=wideColor[1], .b=wideColor[2]},
        };
        UploadBatch upload(device());
        vertexBuffer = upload.buffer(std::as_bytes(std::span(vertices)), RhiBufferUsage::Vertex);
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
        cmd->bindPipeline(pipelines[0]);
        cmd->draw(2, 1, 0, 0);
        cmd->bindPipeline(pipelines[1]);
        cmd->draw(4, 1, 2, 0);
        cmd->endRendering();
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        std::array<float, 3> clear = {clearColor[0], clearColor[1], clearColor[2]};
        auto x = frame.px(0.0f);
        auto wideRow = frame.py(wideY);
        auto thinRow = frame.py(thinY);
        bool ok = true;
        ok = expectPixel(frame, x, wideRow, wideColor, "wide-line-centre") && ok;
        ok = expectPixel(frame, x, thinRow, thinColor, "thin-line-centre") && ok;
        ok = expectPixel(frame, x, thinRow + 3, clear, "thin-line-3px-below-clear") && ok;
        if (wideWidth >= 3.0f) {
            ok = expectPixel(frame, x, wideRow + 1, wideColor, "wide-line-1px-below-covered") && ok;
        } else {
            std::println("check wide-line-1px-below-covered: skipped, wide lines not supported");
        }
        ok = expectPixel(frame, x, wideRow + 3, clear, "wide-line-3px-below-clear") && ok;
        return ok;
    }

    auto teardown() -> void override {
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
    std::array<RhiPipeline*, 2> pipelines = {};
    RhiBuffer* vertexBuffer = nullptr;
    float wideWidth = 1.0f;
};

auto main(int argc, char** argv) -> int {
    LinesExample example;
    return example.run(argc, argv, "lines");
}
