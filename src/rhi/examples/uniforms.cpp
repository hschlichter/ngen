// RHI example: uniform buffers updated every frame.
//
// Adds to texture: a host-visible uniform buffer mapped once and rewritten each
// frame, one region per frame slot so the CPU never writes what an in-flight
// frame still reads, regions placed at minUniformBufferOffsetAlignment, and a
// descriptor set per slot pointing at its region via bufferOffset.
//
// A quad is drawn at an offset read from the UBO, with a colour that steps
// through a palette every 30 frames. --check asserts the palette entry for the
// last frame and that the quad moved away from the origin.
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-uniforms --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"
#include "common/upload.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <print>
#include <span>
#include <vector>

static constexpr const char* vertexShaderSource = R"glsl(
#version 450

layout(set = 0, binding = 0) uniform Params {
    vec4 color;
    vec2 offset;
} params;

layout(location = 0) in vec2 inPosition;
layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = vec4(inPosition + params.offset, 0.0, 1.0);
    fragColor = params.color.rgb;
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

// std140 layout of the shader block: vec4 at 0, vec2 at 16, padded to 32.
struct Params {
    float color[4];
    float offset[2];
    float pad[2];
};

static constexpr std::array<std::array<float, 3>, 3> palette = {{
    {0.9f, 0.3f, 0.2f},
    {0.2f, 0.9f, 0.3f},
    {0.3f, 0.2f, 0.9f},
}};
static constexpr std::array<float, 2> quadOffset = {0.4f, 0.0f};
static constexpr float quadHalf = 0.25f;

// One colour per framesPerColor frames: slow enough to watch, and the UBO is
// still rewritten every frame. --check derives its expectation from the same function.
static constexpr uint64_t framesPerColor = 30;

static auto paletteFor(uint64_t frame) -> std::array<float, 3> {
    return palette[(frame / framesPerColor) % palette.size()];
}

class UniformsExample : public RhiExample {
protected:
    auto setup() -> bool override {
        auto vertexSpirv = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "uniforms.vert");
        auto fragmentSpirv = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "uniforms.frag");
        if (vertexSpirv.empty() || fragmentSpirv.empty()) {
            return false;
        }
        vertexShader = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vertexSpirv});
        fragmentShader = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fragmentSpirv});

        std::array<RhiDescriptorBinding, 1> bindings = {{
            {.binding = 0, .type = RhiDescriptorType::UniformBuffer, .stage = RhiShaderStage::Vertex},
        }};
        setLayout = device().createDescriptorSetLayout(bindings);

        std::array<RhiVertexAttribute, 1> attributes = {{
            {.location = 0, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = 0},
        }};
        auto format = colorFormat();
        RhiGraphicsPipelineDesc pipelineDesc = {
            .vertexShader = vertexShader,
            .fragmentShader = fragmentShader,
            .descriptorSetLayouts = {&setLayout, 1},
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

        // Quad centred on the origin; the UBO offset moves it.
        std::array<float, 8> vertices = {-quadHalf, -quadHalf, quadHalf, -quadHalf, quadHalf, quadHalf, -quadHalf, quadHalf};
        std::array<uint16_t, 6> indices = {0, 1, 2, 2, 3, 0};
        {
            UploadBatch upload(device());
            vertexBuffer = upload.buffer(std::as_bytes(std::span(vertices)), RhiBufferUsage::Vertex);
            indexBuffer = upload.buffer(std::as_bytes(std::span(indices)), RhiBufferUsage::Index);
            upload.finish();
        }

        // One host-visible buffer, one region per frame slot. Regions start at a
        // multiple of the device's UBO offset alignment; the descriptor for slot i
        // points at region i. Mapped once, written every frame.
        auto alignment = device().limits().minUniformBufferOffsetAlignment;
        regionStride = (uint32_t) ((sizeof(Params) + alignment - 1) / alignment * alignment);
        std::println("uniform region stride {} bytes (alignment {})", regionStride, alignment);

        RhiBufferDesc uboDesc = {
            .size = (uint64_t) regionStride * frameCount(),
            .usage = RhiBufferUsage::Uniform,
            .memory = RhiMemoryUsage::CpuToGpu,
        };
        uniformBuffer = device().createBuffer(uboDesc);
        uniformMapped = static_cast<uint8_t*>(device().mapBuffer(uniformBuffer));

        pool = device().createDescriptorPool(frameCount(), bindings);
        descriptorSets.assign(frameCount(), nullptr);
        device().allocateDescriptorSets(pool, setLayout, descriptorSets);
        for (uint32_t slot = 0; slot < frameCount(); slot++) {
            std::array<RhiDescriptorWrite, 1> writes = {{
                {
                    .binding = 0,
                    .type = RhiDescriptorType::UniformBuffer,
                    .buffer = uniformBuffer,
                    .bufferOffset = (uint64_t) slot * regionStride,
                    .bufferRange = sizeof(Params),
                },
            }};
            device().updateDescriptorSet(descriptorSets[slot], writes);
        }
        return true;
    }

    auto record(RhiCommandBuffer* cmd, RhiTexture* backbuffer, RhiExtent2D extent) -> void override {
        // This slot's region is free: the base waited on the slot's fence before record().
        auto color = paletteFor(frameIndex());
        Params params = {
            .color = {color[0], color[1], color[2], 1.0f},
            .offset = {quadOffset[0], quadOffset[1]},
            .pad = {},
        };
        memcpy(uniformMapped + (size_t) frameSlot() * regionStride, &params, sizeof(params));

        std::array<RhiRenderingAttachmentInfo, 1> colorAttachments = {{
            {.texture = backbuffer, .state = RhiTextureState::ColorAttachment, .clear = true, .clearColor = clearColor},
        }};
        cmd->beginRendering({.extent = extent, .colorAttachments = colorAttachments});
        cmd->setViewport(extent);
        cmd->setScissor(extent);
        cmd->bindPipeline(pipeline);
        cmd->bindVertexBuffer(vertexBuffer);
        cmd->bindIndexBuffer(indexBuffer, RhiIndexType::Uint16);
        cmd->bindDescriptorSet(pipeline, 0, descriptorSets[frameSlot()]);
        cmd->drawIndexed(6, 1, 0, 0, 0);
        cmd->endRendering();
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        auto lastFrame = options().frames - 1;
        bool ok = true;
        ok = expectPixel(frame, frame.px(quadOffset[0]), frame.py(quadOffset[1]), paletteFor(lastFrame), "ubo-colour-of-last-frame") && ok;
        ok = expectPixel(frame, frame.px(0.0f), frame.py(0.0f), {clearColor[0], clearColor[1], clearColor[2]}, "ubo-offset-moved-quad") && ok;
        return ok;
    }

    auto teardown() -> void override {
        device().freeDescriptorSets(pool, descriptorSets);
        device().destroyDescriptorPool(pool);
        device().unmapBuffer(uniformBuffer);
        device().destroyBuffer(uniformBuffer);
        device().destroyBuffer(indexBuffer);
        device().destroyBuffer(vertexBuffer);
        device().destroyPipeline(pipeline);
        device().destroyDescriptorSetLayout(setLayout);
        device().destroyShaderModule(fragmentShader);
        device().destroyShaderModule(vertexShader);
    }

private:
    RhiShaderModule* vertexShader = nullptr;
    RhiShaderModule* fragmentShader = nullptr;
    RhiDescriptorSetLayout* setLayout = nullptr;
    RhiPipeline* pipeline = nullptr;
    RhiBuffer* vertexBuffer = nullptr;
    RhiBuffer* indexBuffer = nullptr;
    RhiBuffer* uniformBuffer = nullptr;
    uint8_t* uniformMapped = nullptr;
    uint32_t regionStride = 0;
    RhiDescriptorPool* pool = nullptr;
    std::vector<RhiDescriptorSet*> descriptorSets;
};

auto main(int argc, char** argv) -> int {
    UniformsExample example;
    return example.run(argc, argv, "uniforms");
}
