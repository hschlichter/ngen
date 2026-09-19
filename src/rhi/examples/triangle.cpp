// RHI example: one pipeline, one triangle, no buffers, no descriptors.
//
// The shared part (window, device, swapchain, frame loop, readback, flags) is
// RhiExample in common/rhiexample.h; read that first. This file is only what
// differs per example: shaders, pipeline, the draw, and what to assert.
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-triangle --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"

#include <array>
#include <print>

// Shaders live next to the pipeline that uses them. Compiled at startup with
// shaderc; the engine compiles offline, examples trade a runtime dependency for
// readability.
static constexpr const char* vertexShaderSource = R"glsl(
#version 450

layout(location = 0) out vec3 fragColor;

const vec2 positions[3] = vec2[](
    vec2( 0.0, -0.6),
    vec2( 0.6,  0.6),
    vec2(-0.6,  0.6)
);

const vec3 colors[3] = vec3[](
    vec3(1.0, 0.2, 0.2),
    vec3(0.2, 1.0, 0.2),
    vec3(0.2, 0.2, 1.0)
);

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragColor = colors[gl_VertexIndex];
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

// The window centre is NDC (0,0); barycentric weights there are (0.5, 0.25, 0.25),
// so the interpolated colour is 0.5*red + 0.25*green + 0.25*blue.
static constexpr std::array<float, 3> centerColor = {0.6f, 0.4f, 0.4f};

class TriangleExample : public RhiExample {
protected:
    auto setup() -> bool override {
        // Bytecode in; the RHI does no file IO and no compilation.
        auto vertexSpirv = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "triangle.vert");
        auto fragmentSpirv = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "triangle.frag");
        if (vertexSpirv.empty() || fragmentSpirv.empty()) {
            return false;
        }
        vertexShader = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vertexSpirv});
        fragmentShader = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fragmentSpirv});

        // No vertex buffer, no descriptors, no depth. Viewport and scissor are
        // dynamic so a resize does not rebuild the pipeline.
        auto format = colorFormat();
        RhiGraphicsPipelineDesc pipelineDesc = {
            .vertexShader = vertexShader,
            .fragmentShader = fragmentShader,
            .colorFormats = {&format, 1},
            .raster = {.cullMode = RhiCullMode::None},
            .depth = {.testEnable = false, .writeEnable = false},
        };
        pipeline = device().createGraphicsPipeline(pipelineDesc);
        return pipeline != nullptr;
    }

    auto record(RhiCommandBuffer* cmd, RhiTexture* backbuffer, RhiExtent2D extent) -> void override {
        std::array<RhiRenderingAttachmentInfo, 1> colorAttachments = {{
            {
                .texture = backbuffer,
                .state = RhiTextureState::ColorAttachment,
                .clear = true,
                .clearColor = clearColor,
            },
        }};
        cmd->beginRendering({.extent = extent, .colorAttachments = colorAttachments});
        cmd->setViewport(extent);
        cmd->setScissor(extent);
        cmd->bindPipeline(pipeline);
        cmd->draw(3, 1, 0, 0);
        cmd->endRendering();
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        bool ok = true;
        ok = expectPixel(frame, frame.extent.width / 2, frame.extent.height / 2, centerColor, "triangle-center") && ok;
        ok = expectPixel(frame, 2, 2, {clearColor[0], clearColor[1], clearColor[2]}, "clear-corner") && ok;
        return ok;
    }

    auto teardown() -> void override {
        device().destroyPipeline(pipeline);
        device().destroyShaderModule(fragmentShader);
        device().destroyShaderModule(vertexShader);
    }

private:
    RhiShaderModule* vertexShader = nullptr;
    RhiShaderModule* fragmentShader = nullptr;
    RhiPipeline* pipeline = nullptr;
};

auto main(int argc, char** argv) -> int {
    TriangleExample example;
    return example.run(argc, argv, "triangle");
}
