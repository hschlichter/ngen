// RHI example: indirect indexed draws.
//
// Adds to bindless: draw arguments read by the GPU from a buffer
// (RhiDrawIndexedIndirectCommand, RhiBufferUsage::Indirect). Eight quads share
// one vertex and index buffer; each command draws one quad and carries its
// index in firstInstance, which the vertex shader uses to fetch a colour from a
// storage buffer. Two calls per frame:
//
//   drawIndexedIndirect       commands 0..3, drawCount 4 on the CPU
//   drawIndexedIndirectCount  commands 4..7, maxDrawCount 4, count 3 read on the GPU
//
// Quad 7 has a valid command but is past the GPU count, so it stays clear colour.
//
//   row 0   quads 0..3   first call
//   row 1   quads 4..7   second call, quad 7 not drawn
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-indirect --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"
#include "common/upload.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <vector>

static constexpr uint32_t quadCount = 8;
static constexpr uint32_t firstCallCount = 4;
static constexpr uint32_t secondCallMax = 4;
static constexpr uint32_t secondCallGpuCount = 3;

static constexpr const char* vertexShaderSource = R"glsl(
#version 450

layout(std430, set = 0, binding = 0) readonly buffer Colors {
    vec4 colors[];
};

layout(location = 0) in vec2 inPosition;
layout(location = 0) flat out vec4 fragColor;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    // firstInstance of the command selects the colour; gl_InstanceIndex includes it.
    fragColor = colors[gl_InstanceIndex];
}
)glsl";

static constexpr const char* fragmentShaderSource = R"glsl(
#version 450

layout(location = 0) flat in vec4 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = fragColor;
}
)glsl";

struct Vertex {
    float x;
    float y;
};

// Linear colours in multiples of 0.2, stored as float4 for std430.
static constexpr std::array<std::array<float, 4>, quadCount> quadColors = {{
    {1.0f, 0.2f, 0.2f, 1.0f},
    {0.2f, 1.0f, 0.2f, 1.0f},
    {0.2f, 0.2f, 1.0f, 1.0f},
    {1.0f, 1.0f, 0.2f, 1.0f},
    {1.0f, 0.2f, 1.0f, 1.0f},
    {0.2f, 1.0f, 1.0f, 1.0f},
    {0.6f, 0.4f, 0.2f, 1.0f},
    {1.0f, 1.0f, 1.0f, 1.0f},
}};

static constexpr float quadHalf = 0.2f;

static auto quadCenter(uint32_t q) -> std::array<float, 2> {
    auto column = (float) (q % 4);
    auto row = q < 4 ? 0.0f : 1.0f;
    return {-0.75f + (column * 0.5f), -0.5f + row};
}

class IndirectExample : public RhiExample {
protected:
    auto setup() -> bool override {
        auto vertexSpirv = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "indirect.vert");
        auto fragmentSpirv = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "indirect.frag");
        if (vertexSpirv.empty() || fragmentSpirv.empty()) {
            return false;
        }
        vertexShader = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vertexSpirv});
        fragmentShader = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fragmentSpirv});

        std::array<RhiDescriptorBinding, 1> bindings = {{
            {.binding = 0, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Vertex},
        }};
        setLayout = device().createDescriptorSetLayout(bindings);

        std::array<RhiVertexAttribute, 1> attributes = {{
            {.location = 0, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = offsetof(Vertex, x)},
        }};
        auto format = colorFormat();
        RhiGraphicsPipelineDesc pipelineDesc = {
            .vertexShader = vertexShader,
            .fragmentShader = fragmentShader,
            .descriptorSetLayouts = {&setLayout, 1},
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

        // All quads in one buffer pair; command q addresses quad q by firstIndex.
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        std::vector<RhiDrawIndexedIndirectCommand> commands;
        for (uint32_t q = 0; q < quadCount; q++) {
            auto c = quadCenter(q);
            auto base = (uint16_t) vertices.size();
            vertices.push_back({c[0] - quadHalf, c[1] - quadHalf});
            vertices.push_back({c[0] + quadHalf, c[1] - quadHalf});
            vertices.push_back({c[0] + quadHalf, c[1] + quadHalf});
            vertices.push_back({c[0] - quadHalf, c[1] + quadHalf});
            for (auto i : {0, 1, 2, 2, 3, 0}) {
                indices.push_back((uint16_t) (base + i));
            }
            commands.push_back({
                .indexCount = 6,
                .instanceCount = 1,
                .firstIndex = q * 6,
                .vertexOffset = 0,
                .firstInstance = q,
            });
        }
        std::array<uint32_t, 1> gpuCount = {secondCallGpuCount};

        {
            UploadBatch upload(device());
            vertexBuffer = upload.buffer(std::as_bytes(std::span(vertices)), RhiBufferUsage::Vertex);
            indexBuffer = upload.buffer(std::as_bytes(std::span(indices)), RhiBufferUsage::Index);
            colorBuffer = upload.buffer(std::as_bytes(std::span(quadColors)), RhiBufferUsage::Storage);
            commandBuffer = upload.buffer(std::as_bytes(std::span(commands)), RhiBufferUsage::Indirect);
            countBuffer = upload.buffer(std::as_bytes(std::span(gpuCount)), RhiBufferUsage::Indirect);
            upload.finish();
        }

        pool = device().createDescriptorPool(1, bindings);
        device().allocateDescriptorSets(pool, setLayout, {&descriptorSet, 1});
        std::array<RhiDescriptorWrite, 1> writes = {{
            {.binding = 0, .type = RhiDescriptorType::StorageBuffer, .buffer = colorBuffer},
        }};
        device().updateDescriptorSet(descriptorSet, writes);
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
        cmd->bindDescriptorSet(pipeline, 0, descriptorSet);
        cmd->bindVertexBuffer(vertexBuffer);
        cmd->bindIndexBuffer(indexBuffer, RhiIndexType::Uint16);
        cmd->drawIndexedIndirect(commandBuffer, 0, firstCallCount);
        auto secondOffset = (uint64_t) firstCallCount * sizeof(RhiDrawIndexedIndirectCommand);
        cmd->drawIndexedIndirectCount(commandBuffer, secondOffset, countBuffer, 0, secondCallMax);
        cmd->endRendering();
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        bool ok = true;
        ok = expectCount("stats-indirect-draws", frame.stats.indirectDraws, 2) && ok;
        ok = expectCount("stats-direct-draws", frame.stats.draws, 0) && ok;
        auto drawnCount = firstCallCount + secondCallGpuCount;
        for (uint32_t q = 0; q < quadCount; q++) {
            auto c = quadCenter(q);
            std::array<float, 3> expected = {clearColor[0], clearColor[1], clearColor[2]};
            if (q < drawnCount) {
                expected = {quadColors[q][0], quadColors[q][1], quadColors[q][2]};
            }
            auto name = std::format("quad-{}-{}", q, q < drawnCount ? "drawn" : "past-gpu-count");
            ok = expectPixel(frame, frame.px(c[0]), frame.py(c[1]), expected, name.c_str()) && ok;
        }
        return ok;
    }

    auto teardown() -> void override {
        device().freeDescriptorSets(pool, {&descriptorSet, 1});
        device().destroyDescriptorPool(pool);
        device().destroyBuffer(countBuffer);
        device().destroyBuffer(commandBuffer);
        device().destroyBuffer(colorBuffer);
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
    RhiBuffer* colorBuffer = nullptr;
    RhiBuffer* commandBuffer = nullptr;
    RhiBuffer* countBuffer = nullptr;
    RhiDescriptorPool* pool = nullptr;
    RhiDescriptorSet* descriptorSet = nullptr;
};

auto main(int argc, char** argv) -> int {
    IndirectExample example;
    return example.run(argc, argv, "indirect");
}
