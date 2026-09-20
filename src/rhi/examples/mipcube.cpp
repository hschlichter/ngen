// RHI example: mip levels, array layers, cube maps.
//
// Adds to texture: textures with more than one subresource, each mip and layer
// uploaded with its own copyBufferToTexture, and three sampler types in one
// shader selected by a push constant:
//
//   top row      mip 0, 1, 2 of a 4x4 texture via textureLod   (RhiTextureDimension::Texture2D, mipLevels 3)
//                layer 0, 1 of a 2D array via sampler2DArray  (Texture2DArray, arrayLayers 2)
//   bottom row   faces +X -X +Y -Y +Z -Z via samplerCube       (TextureCube, arrayLayers 6)
//
// Every subresource is a flat colour, so each quad shows exactly one of them.
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-mipcube --frames=60 --check --validation

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
layout(set = 0, binding = 0) uniform sampler2D mipTex;
layout(set = 0, binding = 1) uniform sampler2DArray arrayTex;
layout(set = 0, binding = 2) uniform samplerCube cubeTex;

layout(push_constant) uniform Push {
    int mode;      // 0 = mip level, 1 = array layer, 2 = cube face
    float select;  // lod or layer
    vec2 pad;
    vec4 direction;
} push;

layout(location = 0) in vec2 fragUv;
layout(location = 0) out vec4 outColor;

void main() {
    if (push.mode == 0) {
        outColor = textureLod(mipTex, fragUv, push.select);
    } else if (push.mode == 1) {
        outColor = texture(arrayTex, vec3(fragUv, push.select));
    } else {
        outColor = texture(cubeTex, push.direction.xyz);
    }
}
)glsl";

struct Push {
    int32_t mode;
    float select;
    float pad[2];
    float direction[4];
};

struct Vertex {
    float x;
    float y;
    float u;
    float v;
};

// One flat colour per subresource. Bytes are exact multiples of 51 so UNORM
// storage and linear expectation agree.
static constexpr std::array<std::array<float, 3>, 3> mipColors = {{{1.0f, 0.2f, 0.2f}, {0.2f, 1.0f, 0.2f}, {0.2f, 0.2f, 1.0f}}};
static constexpr std::array<std::array<float, 3>, 2> layerColors = {{{1.0f, 1.0f, 0.2f}, {0.2f, 1.0f, 1.0f}}};
static constexpr std::array<std::array<float, 3>, 6> faceColors = {{
    {1.0f, 0.4f, 0.4f}, // +X
    {0.4f, 0.0f, 0.0f}, // -X
    {0.4f, 1.0f, 0.4f}, // +Y
    {0.0f, 0.4f, 0.0f}, // -Y
    {0.4f, 0.4f, 1.0f}, // +Z
    {0.0f, 0.0f, 0.4f}, // -Z
}};
static constexpr std::array<std::array<float, 3>, 6> faceDirections = {{
    {1.0f, 0.0f, 0.0f},
    {-1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
    {0.0f, -1.0f, 0.0f},
    {0.0f, 0.0f, 1.0f},
    {0.0f, 0.0f, -1.0f},
}};

static constexpr uint32_t blitCorner = 16; // pixels; the top row of quads starts further in
static constexpr float quadHalf = 0.12f;
static constexpr float topRowY = -0.4f;
static constexpr float bottomRowY = 0.4f;
static constexpr std::array<float, 5> topRowX = {-0.8f, -0.4f, 0.0f, 0.4f, 0.8f};
static constexpr std::array<float, 6> bottomRowX = {-0.75f, -0.45f, -0.15f, 0.15f, 0.45f, 0.75f};

// Tightly packed RGBA8 of one flat colour.
static auto flatPixels(uint32_t width, uint32_t height, std::array<float, 3> color) -> std::vector<uint8_t> {
    std::vector<uint8_t> bytes;
    bytes.reserve((size_t) width * height * 4);
    for (uint32_t i = 0; i < width * height; i++) {
        bytes.push_back((uint8_t) (color[0] * 255.0f + 0.5f));
        bytes.push_back((uint8_t) (color[1] * 255.0f + 0.5f));
        bytes.push_back((uint8_t) (color[2] * 255.0f + 0.5f));
        bytes.push_back(255);
    }
    return bytes;
}

class MipCubeExample : public RhiExample {
protected:
    auto setup() -> bool override {
        auto vertexSpirv = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "mipcube.vert");
        auto fragmentSpirv = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "mipcube.frag");
        if (vertexSpirv.empty() || fragmentSpirv.empty()) {
            return false;
        }
        vertexShader = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vertexSpirv});
        fragmentShader = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fragmentSpirv});

        std::array<RhiDescriptorBinding, 3> bindings = {{
            {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Fragment},
            {.binding = 1, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Fragment},
            {.binding = 2, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Fragment},
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
            .pushConstant = {.stage = RhiShaderStage::Fragment, .offset = 0, .size = sizeof(Push)},
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

        // 11 quads: 3 mips + 2 layers on the top row, 6 faces on the bottom row.
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        auto addQuad = [&](float cx, float cy) {
            auto base = (uint16_t) vertices.size();
            vertices.push_back({cx - quadHalf, cy - quadHalf, 0.0f, 0.0f});
            vertices.push_back({cx + quadHalf, cy - quadHalf, 1.0f, 0.0f});
            vertices.push_back({cx + quadHalf, cy + quadHalf, 1.0f, 1.0f});
            vertices.push_back({cx - quadHalf, cy + quadHalf, 0.0f, 1.0f});
            for (auto i : {0, 1, 2, 2, 3, 0}) {
                indices.push_back((uint16_t) (base + i));
            }
        };
        for (auto x : topRowX) {
            addQuad(x, topRowY);
        }
        for (auto x : bottomRowX) {
            addQuad(x, bottomRowY);
        }

        // Pixel data for every subresource, kept alive until the upload finishes.
        auto mip0 = flatPixels(4, 4, mipColors[0]);
        auto mip1 = flatPixels(2, 2, mipColors[1]);
        auto mip2 = flatPixels(1, 1, mipColors[2]);
        std::array<UploadBatch::Subresource, 3> mipLevels = {{
            {.mipLevel = 0, .arrayLayer = 0, .width = 4, .height = 4, .pixels = std::as_bytes(std::span(mip0))},
            {.mipLevel = 1, .arrayLayer = 0, .width = 2, .height = 2, .pixels = std::as_bytes(std::span(mip1))},
            {.mipLevel = 2, .arrayLayer = 0, .width = 1, .height = 1, .pixels = std::as_bytes(std::span(mip2))},
        }};
        std::array<std::vector<uint8_t>, 2> layerPixels = {flatPixels(1, 1, layerColors[0]), flatPixels(1, 1, layerColors[1])};
        std::array<UploadBatch::Subresource, 2> layers = {{
            {.mipLevel = 0, .arrayLayer = 0, .width = 1, .height = 1, .pixels = std::as_bytes(std::span(layerPixels[0]))},
            {.mipLevel = 0, .arrayLayer = 1, .width = 1, .height = 1, .pixels = std::as_bytes(std::span(layerPixels[1]))},
        }};
        std::array<std::vector<uint8_t>, 6> facePixels;
        std::array<UploadBatch::Subresource, 6> faces;
        for (uint32_t f = 0; f < 6; f++) {
            facePixels[f] = flatPixels(1, 1, faceColors[f]);
            faces[f] = {.mipLevel = 0, .arrayLayer = f, .width = 1, .height = 1, .pixels = std::as_bytes(std::span(facePixels[f]))};
        }

        {
            UploadBatch upload(device());
            vertexBuffer = upload.buffer(std::as_bytes(std::span(vertices)), RhiBufferUsage::Vertex);
            indexBuffer = upload.buffer(std::as_bytes(std::span(indices)), RhiBufferUsage::Index);
            mipTexture = upload.textureSubresources({.width = 4, .height = 4, .format = RhiFormat::R8G8B8A8_UNORM, .usage = RhiTextureUsage::Sampled | RhiTextureUsage::TransferSrc, .mipLevels = 3}, mipLevels);
            arrayTexture = upload.textureSubresources({.width = 1, .height = 1, .format = RhiFormat::R8G8B8A8_UNORM, .usage = RhiTextureUsage::Sampled, .arrayLayers = 2, .dimension = RhiTextureDimension::Texture2DArray}, layers);
            cubeTexture = upload.textureSubresources({.width = 1, .height = 1, .format = RhiFormat::R8G8B8A8_UNORM, .usage = RhiTextureUsage::Sampled, .arrayLayers = 6, .dimension = RhiTextureDimension::TextureCube}, faces);
            upload.finish();
        }

        // Nearest everything, and a LOD range wide enough for textureLod to reach mip 2.
        sampler = device().createSampler({.magFilter = RhiFilter::Nearest, .minFilter = RhiFilter::Nearest, .mipmapMode = RhiMipmapMode::Nearest, .maxLod = 8.0f});
        pool = device().createDescriptorPool(1, bindings);
        descriptorSets.assign(1, nullptr);
        device().allocateDescriptorSets(pool, setLayout, descriptorSets);
        std::array<RhiDescriptorWrite, 3> writes = {{
            {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .texture = mipTexture, .sampler = sampler},
            {.binding = 1, .type = RhiDescriptorType::CombinedImageSampler, .texture = arrayTexture, .sampler = sampler},
            {.binding = 2, .type = RhiDescriptorType::CombinedImageSampler, .texture = cubeTexture, .sampler = sampler},
        }};
        device().updateDescriptorSet(descriptorSets[0], writes);
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
        cmd->bindDescriptorSet(pipeline, 0, descriptorSets[0]);

        uint32_t quad = 0;
        auto drawQuad = [&](Push push) {
            cmd->pushConstants(pipeline, RhiShaderStage::Fragment, 0, sizeof(push), &push);
            cmd->drawIndexed(6, 1, quad * 6, 0, 0);
            quad++;
        };
        for (int mip = 0; mip < 3; mip++) {
            drawQuad({.mode = 0, .select = (float) mip, .pad = {}, .direction = {}});
        }
        for (int layer = 0; layer < 2; layer++) {
            drawQuad({.mode = 1, .select = (float) layer, .pad = {}, .direction = {}});
        }
        for (const auto& dir : faceDirections) {
            drawQuad({.mode = 2, .select = 0.0f, .pad = {}, .direction = {dir[0], dir[1], dir[2], 0.0f}});
        }
        cmd->endRendering();

        // Blit with a mip level: mip 1 (2x2, mipColors[1]) magnified into the backbuffer's
        // top-left corner, nearest filter, so the corner reads as one flat colour.
        std::array<RhiTextureBarrierDesc, 2> toBlit = {{
            {.texture = mipTexture, .oldState = RhiTextureState::ShaderReadOnly, .newState = RhiTextureState::TransferSrc},
            {.texture = backbuffer, .oldState = RhiTextureState::ColorAttachment, .newState = RhiTextureState::TransferDst},
        }};
        cmd->pipelineBarrier(toBlit);
        cmd->blitTexture(mipTexture, backbuffer, {.mipLevel = 1, .extent = {2, 2}}, {.mipLevel = 0, .extent = {blitCorner, blitCorner}}, RhiFilter::Nearest);
        std::array<RhiTextureBarrierDesc, 2> fromBlit = {{
            {.texture = mipTexture, .oldState = RhiTextureState::TransferSrc, .newState = RhiTextureState::ShaderReadOnly},
            {.texture = backbuffer, .oldState = RhiTextureState::TransferDst, .newState = RhiTextureState::ColorAttachment},
        }};
        cmd->pipelineBarrier(fromBlit);
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        bool ok = true;
        ok = expectPixel(frame, blitCorner / 2, blitCorner / 2, mipColors[1], "blit-mip-1") && ok;
        std::array<const char*, 3> mipNames = {"mip-0", "mip-1", "mip-2"};
        for (size_t i = 0; i < 3; i++) {
            ok = expectPixel(frame, frame.px(topRowX[i]), frame.py(topRowY), mipColors[i], mipNames[i]) && ok;
        }
        std::array<const char*, 2> layerNames = {"array-layer-0", "array-layer-1"};
        for (size_t i = 0; i < 2; i++) {
            ok = expectPixel(frame, frame.px(topRowX[3 + i]), frame.py(topRowY), layerColors[i], layerNames[i]) && ok;
        }
        std::array<const char*, 6> faceNames = {"cube-face-+x", "cube-face--x", "cube-face-+y", "cube-face--y", "cube-face-+z", "cube-face--z"};
        for (size_t i = 0; i < 6; i++) {
            ok = expectPixel(frame, frame.px(bottomRowX[i]), frame.py(bottomRowY), faceColors[i], faceNames[i]) && ok;
        }
        return ok;
    }

    auto teardown() -> void override {
        device().freeDescriptorSets(pool, descriptorSets);
        device().destroyDescriptorPool(pool);
        device().destroySampler(sampler);
        device().destroyTexture(cubeTexture);
        device().destroyTexture(arrayTexture);
        device().destroyTexture(mipTexture);
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
    RhiTexture* mipTexture = nullptr;
    RhiTexture* arrayTexture = nullptr;
    RhiTexture* cubeTexture = nullptr;
    RhiSampler* sampler = nullptr;
    RhiDescriptorPool* pool = nullptr;
    std::vector<RhiDescriptorSet*> descriptorSets;
};

auto main(int argc, char** argv) -> int {
    MipCubeExample example;
    return example.run(argc, argv, "mipcube");
}
