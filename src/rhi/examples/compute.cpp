// RHI example: compute pipelines, storage buffers, storage images, buffer barriers.
//
// Two compute dispatches run every frame before the graphics pass:
//
//   fillImage    writes a checkerboard into a storage image (RhiImageLayout::General),
//                which a quad on the left then samples
//   fillVertices writes positions and colours into a storage buffer that doubles as
//                the vertex buffer of the quad on the right (RhiBufferUsage::Storage | Vertex)
//
// Every hazard is an explicit barrier: image ShaderReadOnly -> General -> ShaderReadOnly,
// buffer VertexRead -> StorageWrite -> VertexRead. --check derives its expectations from
// the same formulas the shaders use.
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-compute --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"
#include "common/upload.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <print>
#include <span>
#include <vector>

static constexpr uint32_t imageSize = 64;
static constexpr uint32_t cellSize = 8;
static constexpr std::array<float, 3> cellColorA = {0.9f, 0.6f, 0.1f};
static constexpr std::array<float, 3> cellColorB = {0.1f, 0.3f, 0.8f};
static constexpr std::array<float, 3> computedQuadColor = {0.2f, 0.9f, 0.4f};
static constexpr float quadHalf = 0.35f;
static constexpr std::array<float, 2> quadCenterX = {-0.5f, 0.5f};

// Compute 1: checkerboard into a storage image. Cell colours match cellColorA/B.
static constexpr const char* fillImageSource = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, rgba8) uniform writeonly image2D outImage;

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    bool a = ((p.x / 8 + p.y / 8) % 2) == 0;
    vec3 color = a ? vec3(0.9, 0.6, 0.1) : vec3(0.1, 0.3, 0.8);
    imageStore(outImage, p, vec4(color, 1.0));
}
)glsl";

// Compute 2: four vertices of a quad into a storage buffer. Layout matches the
// C++ Vertex struct and the graphics pipeline's attributes. Centre and half size
// match quadCenterX[1] and quadHalf; colour matches computedQuadColor.
static constexpr const char* fillVerticesSource = R"glsl(
#version 450
layout(local_size_x = 4) in;

struct Vertex {
    vec2 position;
    vec2 uv;
    vec4 color;
};
layout(std430, set = 0, binding = 0) buffer Vertices {
    Vertex vertices[];
};

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= 4u) {
        return;
    }
    vec2 corner = vec2((i == 1u || i == 2u) ? 1.0 : -1.0, (i >= 2u) ? 1.0 : -1.0);
    vertices[i].position = vec2(0.5, 0.0) + corner * 0.35;
    vertices[i].uv = corner * 0.5 + 0.5;
    vertices[i].color = vec4(0.2, 0.9, 0.4, 1.0);
}
)glsl";

// Graphics: colour from the vertex, or from the sampled image when useTexture is set.
static constexpr const char* vertexShaderSource = R"glsl(
#version 450
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec4 fragColor;
void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    fragUv = inUv;
    fragColor = inColor;
}
)glsl";

static constexpr const char* fragmentShaderSource = R"glsl(
#version 450
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform Push { int useTexture; } push;
layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = push.useTexture != 0 ? texture(tex, fragUv) : fragColor;
}
)glsl";

struct Vertex {
    float position[2];
    float uv[2];
    float color[4];
};

class ComputeExample : public RhiExample {
protected:
    auto setup() -> bool override {
        auto imageCs = compileGlsl(RhiShaderStage::Compute, fillImageSource, "fillimage.comp");
        auto verticesCs = compileGlsl(RhiShaderStage::Compute, fillVerticesSource, "fillvertices.comp");
        auto vs = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "compute.vert");
        auto fs = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "compute.frag");
        if (imageCs.empty() || verticesCs.empty() || vs.empty() || fs.empty()) {
            return false;
        }
        shaders[0] = device().createShaderModule({.stage = RhiShaderStage::Compute, .code = imageCs});
        shaders[1] = device().createShaderModule({.stage = RhiShaderStage::Compute, .code = verticesCs});
        shaders[2] = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vs});
        shaders[3] = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fs});

        if (!device().supportsTextureFormat(RhiFormat::R8G8B8A8_UNORM, RhiTextureUsage::Storage | RhiTextureUsage::Sampled)) {
            std::println(stderr, "R8G8B8A8_UNORM not usable as a storage image on this device");
            return false;
        }

        // Resources the compute shaders write. The image is also sampled; the
        // buffer is also a vertex buffer.
        RhiTextureDesc imageDesc = {
            .width = imageSize,
            .height = imageSize,
            .format = RhiFormat::R8G8B8A8_UNORM,
            .usage = RhiTextureUsage::Storage | RhiTextureUsage::Sampled,
        };
        storageImage = device().createTexture(imageDesc);
        RhiBufferDesc computedDesc = {
            .size = sizeof(Vertex) * 4,
            .usage = RhiBufferUsage::Storage | RhiBufferUsage::Vertex,
            .memory = RhiMemoryUsage::GpuOnly,
        };
        computedVertices = device().createBuffer(computedDesc);

        // One descriptor set layout per pipeline, one set each.
        std::array<RhiDescriptorBinding, 1> imageBinding = {{{.binding = 0, .type = RhiDescriptorType::StorageImage, .stage = RhiShaderStage::Compute}}};
        std::array<RhiDescriptorBinding, 1> bufferBinding = {{{.binding = 0, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute}}};
        std::array<RhiDescriptorBinding, 1> sampledBinding = {{{.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Fragment}}};
        layouts[0] = device().createDescriptorSetLayout(imageBinding);
        layouts[1] = device().createDescriptorSetLayout(bufferBinding);
        layouts[2] = device().createDescriptorSetLayout(sampledBinding);

        // Compute pipelines: shader plus layout, nothing else.
        fillImagePipeline = device().createComputePipeline({.shader = shaders[0], .descriptorSetLayouts = {layouts.data(), 1}});
        fillVerticesPipeline = device().createComputePipeline({.shader = shaders[1], .descriptorSetLayouts = {&layouts[1], 1}});

        std::array<RhiVertexAttribute, 3> attributes = {{
            {.location = 0, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = offsetof(Vertex, position)},
            {.location = 1, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = offsetof(Vertex, uv)},
            {.location = 2, .binding = 0, .format = RhiFormat::R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, color)},
        }};
        auto format = colorFormat();
        RhiGraphicsPipelineDesc graphicsDesc = {
            .vertexShader = shaders[2],
            .fragmentShader = shaders[3],
            .descriptorSetLayouts = {&layouts[2], 1},
            .pushConstant = {.stage = RhiShaderStage::Fragment, .offset = 0, .size = sizeof(int32_t)},
            .colorFormats = {&format, 1},
            .vertexStride = sizeof(Vertex),
            .vertexAttributes = attributes,
            .raster = {.cullMode = RhiCullMode::None},
            .depth = {.testEnable = false, .writeEnable = false},
        };
        graphicsPipeline = device().createGraphicsPipeline(graphicsDesc);
        if (fillImagePipeline == nullptr || fillVerticesPipeline == nullptr || graphicsPipeline == nullptr) {
            return false;
        }

        // The left quad is uploaded normally; the right one is computed each frame.
        std::array<Vertex, 4> leftQuad = {{
            {.position={quadCenterX[0] - quadHalf, -quadHalf}, .uv={0.0f, 0.0f}, .color={1, 1, 1, 1}},
            {.position={quadCenterX[0] + quadHalf, -quadHalf}, .uv={1.0f, 0.0f}, .color={1, 1, 1, 1}},
            {.position={quadCenterX[0] + quadHalf, quadHalf}, .uv={1.0f, 1.0f}, .color={1, 1, 1, 1}},
            {.position={quadCenterX[0] - quadHalf, quadHalf}, .uv={0.0f, 1.0f}, .color={1, 1, 1, 1}},
        }};
        std::array<uint16_t, 6> indices = {0, 1, 2, 2, 3, 0};
        {
            UploadBatch upload(device());
            uploadedVertices = upload.buffer(std::as_bytes(std::span(leftQuad)), RhiBufferUsage::Vertex);
            indexBuffer = upload.buffer(std::as_bytes(std::span(indices)), RhiBufferUsage::Index);
            upload.finish();
        }

        sampler = device().createSampler({.magFilter = RhiFilter::Nearest, .minFilter = RhiFilter::Nearest});
        std::array<RhiDescriptorBinding, 3> poolBindings = {{imageBinding[0], bufferBinding[0], sampledBinding[0]}};
        pool = device().createDescriptorPool(3, poolBindings);
        for (uint32_t i = 0; i < 3; i++) {
            sets[i] = device().allocateDescriptorSets(pool, layouts[i], 1)[0];
        }
        std::array<RhiDescriptorWrite, 1> imageWrite = {{{.binding = 0, .type = RhiDescriptorType::StorageImage, .texture = storageImage}}};
        std::array<RhiDescriptorWrite, 1> bufferWrite = {{{.binding = 0, .type = RhiDescriptorType::StorageBuffer, .buffer = computedVertices}}};
        std::array<RhiDescriptorWrite, 1> sampledWrite = {{{.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .texture = storageImage, .sampler = sampler}}};
        device().updateDescriptorSet(sets[0], imageWrite);
        device().updateDescriptorSet(sets[1], bufferWrite);
        device().updateDescriptorSet(sets[2], sampledWrite);
        return true;
    }

    auto record(RhiCommandBuffer* cmd, RhiTexture* backbuffer, RhiExtent2D extent) -> void override {
        // Before compute: image to General for imageStore; buffer from last frame's
        // vertex read to this frame's storage write. First frame comes from Undefined.
        auto imageFrom = frameIndex() == 0 ? RhiImageLayout::Undefined : RhiImageLayout::ShaderReadOnly;
        auto bufferFrom = frameIndex() == 0 ? RhiBufferState::Undefined : RhiBufferState::VertexRead;
        std::array<RhiBarrierDesc, 1> imageToGeneral = {{{.texture = storageImage, .oldLayout = imageFrom, .newLayout = RhiImageLayout::General}}};
        std::array<RhiBufferBarrierDesc, 1> bufferToWrite = {{{.buffer = computedVertices, .oldState = bufferFrom, .newState = RhiBufferState::StorageWrite}}};
        cmd->pipelineBarrier(imageToGeneral, bufferToWrite);

        cmd->bindPipeline(fillImagePipeline);
        cmd->bindDescriptorSet(fillImagePipeline, 0, sets[0]);
        cmd->dispatch(imageSize / 8, imageSize / 8, 1);

        cmd->bindPipeline(fillVerticesPipeline);
        cmd->bindDescriptorSet(fillVerticesPipeline, 0, sets[1]);
        cmd->dispatch(1, 1, 1);

        // After compute: image to sampled, buffer to vertex input.
        std::array<RhiBarrierDesc, 1> imageToSampled = {{{.texture = storageImage, .oldLayout = RhiImageLayout::General, .newLayout = RhiImageLayout::ShaderReadOnly}}};
        std::array<RhiBufferBarrierDesc, 1> bufferToVertex = {{{.buffer = computedVertices, .oldState = RhiBufferState::StorageWrite, .newState = RhiBufferState::VertexRead}}};
        cmd->pipelineBarrier(imageToSampled, bufferToVertex);

        std::array<RhiRenderingAttachmentInfo, 1> colorAttachments = {{
            {.texture = backbuffer, .layout = RhiImageLayout::ColorAttachment, .clear = true, .clearColor = clearColor},
        }};
        cmd->beginRendering({.extent = extent, .colorAttachments = colorAttachments});
        cmd->setViewport(extent);
        cmd->setScissor(extent);
        cmd->bindPipeline(graphicsPipeline);
        cmd->bindDescriptorSet(graphicsPipeline, 0, sets[2]);
        cmd->bindIndexBuffer(indexBuffer, RhiIndexType::Uint16);

        int32_t useTexture = 1;
        cmd->pushConstants(graphicsPipeline, RhiShaderStage::Fragment, 0, sizeof(useTexture), &useTexture);
        cmd->bindVertexBuffer(uploadedVertices);
        cmd->drawIndexed(6, 1, 0, 0, 0);

        useTexture = 0;
        cmd->pushConstants(graphicsPipeline, RhiShaderStage::Fragment, 0, sizeof(useTexture), &useTexture);
        cmd->bindVertexBuffer(computedVertices);
        cmd->drawIndexed(6, 1, 0, 0, 0);
        cmd->endRendering();
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        // Left quad maps uv 0..1 over 2*quadHalf; cell (cx, cy) centre is at texel ((cx+0.5)*cellSize).
        auto cellCentre = [&](uint32_t cx, uint32_t cy) {
            auto u = ((float) cx + 0.5f) * (float) cellSize / (float) imageSize;
            auto v = ((float) cy + 0.5f) * (float) cellSize / (float) imageSize;
            auto x = quadCenterX[0] - quadHalf + (u * 2.0f * quadHalf);
            auto y = -quadHalf + (v * 2.0f * quadHalf);
            return std::array<uint32_t, 2>{frame.px(x), frame.py(y)};
        };
        bool ok = true;
        auto p = cellCentre(0, 0);
        ok = expectPixel(frame, p[0], p[1], cellColorA, "storage-image-cell-0-0") && ok;
        p = cellCentre(1, 0);
        ok = expectPixel(frame, p[0], p[1], cellColorB, "storage-image-cell-1-0") && ok;
        p = cellCentre(3, 2);
        ok = expectPixel(frame, p[0], p[1], cellColorB, "storage-image-cell-3-2") && ok;
        ok = expectPixel(frame, frame.px(quadCenterX[1]), frame.py(0.0f), computedQuadColor, "storage-buffer-vertices") && ok;
        std::array<float, 3> clear = {clearColor[0], clearColor[1], clearColor[2]};
        ok = expectPixel(frame, frame.px(quadCenterX[1] + quadHalf + 0.05f), frame.py(0.0f), clear, "computed-quad-edge-where-expected") && ok;
        return ok;
    }

    auto teardown() -> void override {
        device().destroyDescriptorPool(pool);
        for (auto* set : sets) {
            delete set;
        }
        device().destroySampler(sampler);
        device().destroyBuffer(indexBuffer);
        device().destroyBuffer(uploadedVertices);
        device().destroyBuffer(computedVertices);
        device().destroyTexture(storageImage);
        device().destroyPipeline(graphicsPipeline);
        device().destroyPipeline(fillVerticesPipeline);
        device().destroyPipeline(fillImagePipeline);
        for (auto* layout : layouts) {
            device().destroyDescriptorSetLayout(layout);
        }
        for (auto* shader : shaders) {
            device().destroyShaderModule(shader);
        }
    }

private:
    std::array<RhiShaderModule*, 4> shaders = {};
    std::array<RhiDescriptorSetLayout*, 3> layouts = {};
    std::array<RhiDescriptorSet*, 3> sets = {};
    RhiPipeline* fillImagePipeline = nullptr;
    RhiPipeline* fillVerticesPipeline = nullptr;
    RhiPipeline* graphicsPipeline = nullptr;
    RhiTexture* storageImage = nullptr;
    RhiBuffer* computedVertices = nullptr;
    RhiBuffer* uploadedVertices = nullptr;
    RhiBuffer* indexBuffer = nullptr;
    RhiSampler* sampler = nullptr;
    RhiDescriptorPool* pool = nullptr;
};

auto main(int argc, char** argv) -> int {
    ComputeExample example;
    return example.run(argc, argv, "compute");
}
