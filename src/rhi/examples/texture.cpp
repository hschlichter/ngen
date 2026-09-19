// RHI example: textures, samplers, descriptor sets.
//
// Adds to quad: a texture uploaded with copyBufferToTexture and layout
// barriers, four samplers with different filter and address modes, and a
// descriptor set per sampler bound with bindDescriptorSet. Four quads show the
// same 2x2 texture through each sampler.
//
//   top-left     nearest, repeat,        uv 0..1     four flat quadrants
//   top-right    linear,  repeat,        uv 0..1     blended centre
//   bottom-left  nearest, repeat,        uv -0.5..1.5  pattern wraps
//   bottom-right nearest, clamp-to-edge, uv -0.5..1.5  edge texels stretch
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-texture --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"
#include "common/upload.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <print>
#include <span>
#include <vector>

static constexpr const char* vertexShaderSource = R"glsl(
#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec2 fragUv;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    fragUv = inUv;
}
)glsl";

static constexpr const char* fragmentShaderSource = R"glsl(
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

// 2x2 texture, R8G8B8A8_UNORM so a byte of 51 samples as exactly 0.2 linear.
// Row-major, texel (x, y): (0,0) red, (1,0) green, (0,1) blue, (1,1) white.
static constexpr std::array<std::array<float, 3>, 4> texelColors = {{
    {1.0f, 0.2f, 0.2f},
    {0.2f, 1.0f, 0.2f},
    {0.2f, 0.2f, 1.0f},
    {1.0f, 1.0f, 1.0f},
}};

static constexpr float quadHalf = 0.4f;
static constexpr std::array<std::array<float, 2>, 4> quadCenters = {{
    {-0.5f, -0.5f},
    {0.5f, -0.5f},
    {-0.5f, 0.5f},
    {0.5f, 0.5f},
}};
// uv range per quad: min and max, applied on both axes.
static constexpr std::array<std::array<float, 2>, 4> quadUvRange = {{
    {0.0f, 1.0f},
    {0.0f, 1.0f},
    {-0.5f, 1.5f},
    {-0.5f, 1.5f},
}};

static auto texelBytes() -> std::vector<uint8_t> {
    std::vector<uint8_t> bytes;
    for (const auto& c : texelColors) {
        bytes.push_back((uint8_t) (c[0] * 255.0f + 0.5f));
        bytes.push_back((uint8_t) (c[1] * 255.0f + 0.5f));
        bytes.push_back((uint8_t) (c[2] * 255.0f + 0.5f));
        bytes.push_back(255);
    }
    return bytes;
}

class TextureExample : public RhiExample {
protected:
    auto setup() -> bool override {
        auto vertexSpirv = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "texture.vert");
        auto fragmentSpirv = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "texture.frag");
        if (vertexSpirv.empty() || fragmentSpirv.empty()) {
            return false;
        }
        vertexShader = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vertexSpirv});
        fragmentShader = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fragmentSpirv});

        // Descriptor set layout: what the shader binds at set 0. Pipeline layout is
        // built from the span of set layouts, index = set number.
        std::array<RhiDescriptorBinding, 1> bindings = {{
            {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Fragment},
        }};
        setLayout = device().createDescriptorSetLayout(bindings);

        std::array<RhiVertexAttribute, 2> attributes = {{
            {.location = 0, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = offsetof(Vertex, x)},
            {.location = 1, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = offsetof(Vertex, u)},
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

        // Geometry: four quads, each with its own uv range.
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        for (uint32_t q = 0; q < 4; q++) {
            auto cx = quadCenters[q][0];
            auto cy = quadCenters[q][1];
            auto uv0 = quadUvRange[q][0];
            auto uv1 = quadUvRange[q][1];
            auto base = (uint16_t) vertices.size();
            vertices.push_back({cx - quadHalf, cy - quadHalf, uv0, uv0});
            vertices.push_back({cx + quadHalf, cy - quadHalf, uv1, uv0});
            vertices.push_back({cx + quadHalf, cy + quadHalf, uv1, uv1});
            vertices.push_back({cx - quadHalf, cy + quadHalf, uv0, uv1});
            for (auto i : {0, 1, 2, 2, 3, 0}) {
                indices.push_back((uint16_t) (base + i));
            }
        }

        // Upload geometry and the texture in one batch. The texture comes back in
        // ShaderReadOnly, the layout a sampled texture must be in when bound.
        auto pixels = texelBytes();
        RhiTextureDesc texDesc = {
            .width = 2,
            .height = 2,
            .format = RhiFormat::R8G8B8A8_UNORM,
            .usage = RhiTextureUsage::Sampled,
        };
        {
            UploadBatch upload(device());
            vertexBuffer = upload.buffer(std::as_bytes(std::span(vertices)), RhiBufferUsage::Vertex);
            indexBuffer = upload.buffer(std::as_bytes(std::span(indices)), RhiBufferUsage::Index);
            texture = upload.texture(texDesc, std::as_bytes(std::span(pixels)));
            upload.finish();
        }

        // Samplers: the four variants the quads demonstrate.
        samplers[0] = device().createSampler({.magFilter = RhiFilter::Nearest, .minFilter = RhiFilter::Nearest});
        samplers[1] = device().createSampler({.magFilter = RhiFilter::Linear, .minFilter = RhiFilter::Linear});
        samplers[2] = device().createSampler({.magFilter = RhiFilter::Nearest, .minFilter = RhiFilter::Nearest, .addressU = RhiAddressMode::Repeat, .addressV = RhiAddressMode::Repeat});
        samplers[3] = device().createSampler({.magFilter = RhiFilter::Nearest, .minFilter = RhiFilter::Nearest, .addressU = RhiAddressMode::ClampToEdge, .addressV = RhiAddressMode::ClampToEdge});

        // Descriptor sets: one per sampler, all pointing at the same texture.
        pool = device().createDescriptorPool(4, bindings);
        descriptorSets = device().allocateDescriptorSets(pool, setLayout, 4);
        for (uint32_t i = 0; i < 4; i++) {
            std::array<RhiDescriptorWrite, 1> writes = {{
                {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .texture = texture, .sampler = samplers[i]},
            }};
            device().updateDescriptorSet(descriptorSets[i], writes);
        }
        return true;
    }

    auto record(RhiCommandBuffer* cmd, RhiTexture* backbuffer, RhiExtent2D extent) -> void override {
        std::array<RhiRenderingAttachmentInfo, 1> colorAttachments = {{
            {.texture = backbuffer, .layout = RhiImageLayout::ColorAttachment, .clear = true, .clearColor = clearColor},
        }};
        cmd->beginRendering({.extent = extent, .colorAttachments = colorAttachments});
        cmd->setViewport(extent);
        cmd->setScissor(extent);
        cmd->bindPipeline(pipeline);
        cmd->bindVertexBuffer(vertexBuffer);
        cmd->bindIndexBuffer(indexBuffer, RhiIndexType::Uint16);
        for (uint32_t q = 0; q < 4; q++) {
            cmd->bindDescriptorSet(pipeline, 0, descriptorSets[q]);
            cmd->drawIndexed(6, 1, q * 6, 0, 0);
        }
        cmd->endRendering();
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        // Point inside quad q at a fraction (fx, fy) of its extent, 0 = min edge, 1 = max edge.
        auto inQuad = [&](uint32_t q, float fx, float fy) {
            auto x = quadCenters[q][0] - quadHalf + fx * 2.0f * quadHalf;
            auto y = quadCenters[q][1] - quadHalf + fy * 2.0f * quadHalf;
            return std::array<uint32_t, 2>{frame.px(x), frame.py(y)};
        };
        auto average = std::array<float, 3>{0.0f, 0.0f, 0.0f};
        for (const auto& c : texelColors) {
            average[0] += c[0] * 0.25f;
            average[1] += c[1] * 0.25f;
            average[2] += c[2] * 0.25f;
        }

        bool ok = true;
        // Nearest, uv 0..1: quadrant centres hit exactly one texel each.
        auto p = inQuad(0, 0.25f, 0.25f);
        ok = expectPixel(frame, p[0], p[1], texelColors[0], "nearest-texel-0-0") && ok;
        p = inQuad(0, 0.75f, 0.75f);
        ok = expectPixel(frame, p[0], p[1], texelColors[3], "nearest-texel-1-1") && ok;
        // Linear, uv 0..1: the centre is the bilinear average of all four texels.
        p = inQuad(1, 0.5f, 0.5f);
        ok = expectPixel(frame, p[0], p[1], average, "linear-centre-average") && ok;
        // uv -0.5..1.5: fraction 0.125 is uv -0.25. Repeat wraps to 0.75 (texel 1);
        // clamp-to-edge pins to 0 (texel 0). Fraction 0.375 on v is uv 0.25 (texel 0).
        p = inQuad(2, 0.125f, 0.375f);
        ok = expectPixel(frame, p[0], p[1], texelColors[1], "repeat-wraps-to-texel-1-0") && ok;
        p = inQuad(3, 0.125f, 0.375f);
        ok = expectPixel(frame, p[0], p[1], texelColors[0], "clamp-pins-to-texel-0-0") && ok;
        return ok;
    }

    auto teardown() -> void override {
        device().destroyDescriptorPool(pool);
        for (auto* set : descriptorSets) {
            delete set;
        }
        for (auto* sampler : samplers) {
            device().destroySampler(sampler);
        }
        device().destroyTexture(texture);
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
    RhiTexture* texture = nullptr;
    std::array<RhiSampler*, 4> samplers = {};
    RhiDescriptorPool* pool = nullptr;
    std::vector<RhiDescriptorSet*> descriptorSets;
};

auto main(int argc, char** argv) -> int {
    TextureExample example;
    return example.run(argc, argv, "texture");
}
