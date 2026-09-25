// RHI example: descriptor arrays indexed per draw ("bindless" textures).
//
// Adds to texture: one binding holding an array of eight textures
// (RhiDescriptorBinding::count, RhiDescriptorWrite::arrayElement) and a shader
// that picks the element per draw. Eight quads share one descriptor set bound
// once; each draw passes its index as firstInstance, the vertex shader forwards
// gl_InstanceIndex as a flat varying, and the fragment shader samples
// textures[index]. The index is dynamically uniform (one value per draw), which
// needs only shaderSampledImageArrayDynamicIndexing.
//
//   row 0   quads 0..3   textures 0..3
//   row 1   quads 4..7   textures 4..7
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-bindless --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"
#include "common/upload.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <print>
#include <span>
#include <vector>

static constexpr uint32_t textureCount = 8;

static constexpr const char* vertexShaderSource = R"glsl(
#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 0) flat out uint fragTexture;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    // firstInstance carries the texture index; gl_InstanceIndex includes it.
    fragTexture = uint(gl_InstanceIndex);
}
)glsl";

static constexpr const char* fragmentShaderSource = R"glsl(
#version 450

layout(set = 0, binding = 0) uniform sampler2D textures[8];
layout(location = 0) flat in uint fragTexture;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(textures[fragTexture], vec2(0.5));
}
)glsl";

struct Vertex {
    float x;
    float y;
};

// One 1x1 R8G8B8A8_UNORM texture per quad. Channels are multiples of 0.2 so the
// bytes (multiples of 51) sample back exactly.
static constexpr std::array<std::array<float, 3>, textureCount> textureColors = {{
    {1.0f, 0.2f, 0.2f},
    {0.2f, 1.0f, 0.2f},
    {0.2f, 0.2f, 1.0f},
    {1.0f, 1.0f, 0.2f},
    {1.0f, 0.2f, 1.0f},
    {0.2f, 1.0f, 1.0f},
    {0.6f, 0.4f, 0.2f},
    {1.0f, 1.0f, 1.0f},
}};

static constexpr float quadHalf = 0.2f;

static auto quadCenter(uint32_t q) -> std::array<float, 2> {
    auto column = (float) (q % 4);
    auto row = (float) (q / 4);
    return {-0.75f + column * 0.5f, -0.5f + row * 1.0f};
}

class BindlessExample : public RhiExample {
protected:
    auto setup() -> bool override {
        auto vertexSpirv = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "bindless.vert");
        auto fragmentSpirv = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "bindless.frag");
        if (vertexSpirv.empty() || fragmentSpirv.empty()) {
            return false;
        }
        if (device().limits().maxPerStageSampledImages < textureCount) {
            std::println(stderr, "bindless: device allows {} sampled images per stage, need {}", device().limits().maxPerStageSampledImages, textureCount);
            return false;
        }
        vertexShader = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vertexSpirv});
        fragmentShader = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fragmentSpirv});

        // One binding, eight elements: the array the fragment shader indexes.
        std::array<RhiDescriptorBinding, 1> bindings = {{
            {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Fragment, .count = textureCount},
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

        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        for (uint32_t q = 0; q < textureCount; q++) {
            auto c = quadCenter(q);
            auto base = (uint16_t) vertices.size();
            vertices.push_back({c[0] - quadHalf, c[1] - quadHalf});
            vertices.push_back({c[0] + quadHalf, c[1] - quadHalf});
            vertices.push_back({c[0] + quadHalf, c[1] + quadHalf});
            vertices.push_back({c[0] - quadHalf, c[1] + quadHalf});
            for (auto i : {0, 1, 2, 2, 3, 0}) {
                indices.push_back((uint16_t) (base + i));
            }
        }

        {
            UploadBatch upload(device());
            vertexBuffer = upload.buffer(std::as_bytes(std::span(vertices)), RhiBufferUsage::Vertex);
            indexBuffer = upload.buffer(std::as_bytes(std::span(indices)), RhiBufferUsage::Index);
            for (uint32_t t = 0; t < textureCount; t++) {
                const auto& c = textureColors[t];
                std::array<uint8_t, 4> texel = {
                    (uint8_t) (c[0] * 255.0f + 0.5f),
                    (uint8_t) (c[1] * 255.0f + 0.5f),
                    (uint8_t) (c[2] * 255.0f + 0.5f),
                    255,
                };
                RhiTextureDesc desc = {
                    .width = 1,
                    .height = 1,
                    .format = RhiFormat::R8G8B8A8_UNORM,
                    .usage = RhiTextureUsage::Sampled,
                };
                textures[t] = upload.texture(desc, std::as_bytes(std::span(texel)));
            }
            upload.finish();
        }

        sampler = device().createSampler({.magFilter = RhiFilter::Nearest, .minFilter = RhiFilter::Nearest});

        // One set; every array element written, one write per element.
        pool = device().createDescriptorPool(1, bindings);
        device().allocateDescriptorSets(pool, setLayout, {&descriptorSet, 1});
        std::vector<RhiDescriptorWrite> writes;
        for (uint32_t t = 0; t < textureCount; t++) {
            writes.push_back({.binding = 0, .arrayElement = t, .type = RhiDescriptorType::CombinedImageSampler, .texture = textures[t], .sampler = sampler});
        }
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
        for (uint32_t q = 0; q < textureCount; q++) {
            cmd->drawIndexed(6, 1, q * 6, 0, q);
        }
        cmd->endRendering();
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        bool ok = true;
        // The descriptor set was bound once; every quad still shows its own texture.
        ok = expectCount("stats-descriptor-binds", frame.stats.descriptorBinds, 1) && ok;
        for (uint32_t q = 0; q < textureCount; q++) {
            auto c = quadCenter(q);
            auto name = std::format("quad-{}-samples-texture-{}", q, q);
            ok = expectPixel(frame, frame.px(c[0]), frame.py(c[1]), textureColors[q], name.c_str()) && ok;
        }
        return ok;
    }

    auto teardown() -> void override {
        device().freeDescriptorSets(pool, {&descriptorSet, 1});
        device().destroyDescriptorPool(pool);
        device().destroySampler(sampler);
        for (auto* texture : textures) {
            device().destroyTexture(texture);
        }
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
    std::array<RhiTexture*, textureCount> textures = {};
    RhiSampler* sampler = nullptr;
    RhiDescriptorPool* pool = nullptr;
    RhiDescriptorSet* descriptorSet = nullptr;
};

auto main(int argc, char** argv) -> int {
    BindlessExample example;
    return example.run(argc, argv, "bindless");
}
