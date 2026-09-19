// RHI example: push constants.
//
// Adds to quad: one pipeline, one push constant range visible to both stages,
// and one quad drawn three times with a different offset and colour pushed
// before each draw. The cheapest way to vary per-draw data; limits() says how
// much fits.
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-pushconstants --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"
#include "common/upload.h"

#include <array>
#include <cstdint>
#include <print>
#include <span>

static constexpr const char* vertexShaderSource = R"glsl(
#version 450

layout(push_constant) uniform Push {
    vec2 offset;
    vec4 color;
} push;

layout(location = 0) in vec2 inPosition;

void main() {
    gl_Position = vec4(inPosition + push.offset, 0.0, 1.0);
}
)glsl";

static constexpr const char* fragmentShaderSource = R"glsl(
#version 450

layout(push_constant) uniform Push {
    vec2 offset;
    vec4 color;
} push;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = push.color;
}
)glsl";

// Matches the GLSL block: vec2 at 0, vec4 aligned to 16.
struct Push {
    float offset[2];
    float pad[2];
    float color[4];
};

static constexpr float quadHalf = 0.2f;
static constexpr std::array<std::array<float, 2>, 3> offsets = {{{-0.6f, 0.0f}, {0.0f, 0.0f}, {0.6f, 0.0f}}};
static constexpr std::array<std::array<float, 3>, 3> colors = {{{0.9f, 0.3f, 0.2f}, {0.3f, 0.9f, 0.2f}, {0.2f, 0.3f, 0.9f}}};

class PushConstantsExample : public RhiExample {
protected:
    auto setup() -> bool override {
        if (device().limits().maxPushConstantSize < sizeof(Push)) {
            std::println(stderr, "device push constant limit {} < {}", device().limits().maxPushConstantSize, sizeof(Push));
            return false;
        }
        std::println("push constant block {} bytes, device limit {}", sizeof(Push), device().limits().maxPushConstantSize);

        auto vertexSpirv = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "push.vert");
        auto fragmentSpirv = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "push.frag");
        if (vertexSpirv.empty() || fragmentSpirv.empty()) {
            return false;
        }
        vertexShader = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vertexSpirv});
        fragmentShader = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fragmentSpirv});

        std::array<RhiVertexAttribute, 1> attributes = {{
            {.location = 0, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = 0},
        }};
        auto format = colorFormat();
        RhiGraphicsPipelineDesc pipelineDesc = {
            .vertexShader = vertexShader,
            .fragmentShader = fragmentShader,
            // One range, both stages read it. Offsets inside the block are the shader's business.
            .pushConstant = {.stage = RhiShaderStage::Vertex | RhiShaderStage::Fragment, .offset = 0, .size = sizeof(Push)},
            .colorFormats = {&format, 1},
            .vertexStride = sizeof(float) * 2,
            .vertexAttributes = attributes,
            .raster = {.cullMode = RhiCullMode::None},
            .depth = {.testEnable = false, .writeEnable = false},
        };
        pipeline = device().createGraphicsPipeline(pipelineDesc);
        if (pipeline == nullptr) {
            return false;
        }

        std::array<float, 8> vertices = {-quadHalf, -quadHalf, quadHalf, -quadHalf, quadHalf, quadHalf, -quadHalf, quadHalf};
        std::array<uint16_t, 6> indices = {0, 1, 2, 2, 3, 0};
        UploadBatch upload(device());
        vertexBuffer = upload.buffer(std::as_bytes(std::span(vertices)), RhiBufferUsage::Vertex);
        indexBuffer = upload.buffer(std::as_bytes(std::span(indices)), RhiBufferUsage::Index);
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
        cmd->bindIndexBuffer(indexBuffer, RhiIndexType::Uint16);
        for (size_t i = 0; i < 3; i++) {
            Push push = {
                .offset = {offsets[i][0], offsets[i][1]},
                .pad = {},
                .color = {colors[i][0], colors[i][1], colors[i][2], 1.0f},
            };
            cmd->pushConstants(pipeline, RhiShaderStage::Vertex | RhiShaderStage::Fragment, 0, sizeof(push), &push);
            cmd->drawIndexed(6, 1, 0, 0, 0);
        }
        cmd->endRendering();
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        bool ok = true;
        ok = expectPixel(frame, frame.px(offsets[0][0]), frame.py(0.0f), colors[0], "push-draw-0") && ok;
        ok = expectPixel(frame, frame.px(offsets[1][0]), frame.py(0.0f), colors[1], "push-draw-1") && ok;
        ok = expectPixel(frame, frame.px(offsets[2][0]), frame.py(0.0f), colors[2], "push-draw-2") && ok;
        return ok;
    }

    auto teardown() -> void override {
        device().destroyBuffer(indexBuffer);
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
    RhiBuffer* indexBuffer = nullptr;
};

auto main(int argc, char** argv) -> int {
    PushConstantsExample example;
    return example.run(argc, argv, "pushconstants");
}
