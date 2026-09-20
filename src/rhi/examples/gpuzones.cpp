// RHI example: nested GPU zones.
//
// Adds to timestamps: beginGpuZone/endGpuZone on the command buffer and
// collectGpuZones after the fence. Three zones per frame: "frame" wrapping a
// "compute" zone and a "draw" zone. --check asserts depth, ordering and
// containment, which is what a profiler timeline relies on.
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-gpuzones --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"
#include "common/upload.h"

#include <array>
#include <cstdint>
#include <print>
#include <span>
#include <string_view>
#include <vector>

static constexpr uint32_t imageSize = 256;

static constexpr const char* computeSource = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, rgba8) uniform writeonly image2D outImage;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    float v = 0.3;
    for (uint i = 0u; i < 64u; i++) {
        v = sin(v) * 0.5 + 0.5;
    }
    imageStore(outImage, p, vec4(v, v, v, 1.0));
}
)glsl";

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

class GpuZonesExample : public RhiExample {
protected:
    auto setup() -> bool override {
        if (!device().limits().timestamps) {
            std::println(stderr, "timestamps not supported on this device");
            return false;
        }
        auto cs = compileGlsl(RhiShaderStage::Compute, computeSource, "gpuzones.comp");
        auto vs = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "gpuzones.vert");
        auto fs = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "gpuzones.frag");
        if (cs.empty() || vs.empty() || fs.empty()) {
            return false;
        }
        shaders[0] = device().createShaderModule({.stage = RhiShaderStage::Compute, .code = cs});
        shaders[1] = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vs});
        shaders[2] = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fs});

        image = device().createTexture({.width = imageSize, .height = imageSize, .format = RhiFormat::R8G8B8A8_UNORM, .usage = RhiTextureUsage::Storage | RhiTextureUsage::Sampled});

        std::array<RhiDescriptorBinding, 1> storageBinding = {{{.binding = 0, .type = RhiDescriptorType::StorageImage, .stage = RhiShaderStage::Compute}}};
        std::array<RhiDescriptorBinding, 1> sampledBinding = {{{.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Fragment}}};
        layouts[0] = device().createDescriptorSetLayout(storageBinding);
        layouts[1] = device().createDescriptorSetLayout(sampledBinding);
        computePipeline = device().createComputePipeline({.shader = shaders[0], .descriptorSetLayouts = {&layouts[0], 1}});

        std::array<RhiVertexAttribute, 2> attributes = {{
            {.location = 0, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = 0},
            {.location = 1, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = sizeof(float) * 2},
        }};
        auto format = colorFormat();
        graphicsPipeline = device().createGraphicsPipeline({
            .vertexShader = shaders[1],
            .fragmentShader = shaders[2],
            .descriptorSetLayouts = {&layouts[1], 1},
            .colorFormats = {&format, 1},
            .vertexStride = sizeof(float) * 4,
            .vertexAttributes = attributes,
            .raster = {.cullMode = RhiCullMode::None},
            .depth = {.testEnable = false, .writeEnable = false},
        });
        if (computePipeline == nullptr || graphicsPipeline == nullptr) {
            return false;
        }

        std::array<float, 16> vertices = {-0.5f, -0.5f, 0, 0, 0.5f, -0.5f, 1, 0, 0.5f, 0.5f, 1, 1, -0.5f, 0.5f, 0, 1};
        std::array<uint16_t, 6> indices = {0, 1, 2, 2, 3, 0};
        {
            UploadBatch upload(device());
            vertexBuffer = upload.buffer(std::as_bytes(std::span(vertices)), RhiBufferUsage::Vertex);
            indexBuffer = upload.buffer(std::as_bytes(std::span(indices)), RhiBufferUsage::Index);
            upload.finish();
        }
        sampler = device().createSampler({.magFilter = RhiFilter::Nearest, .minFilter = RhiFilter::Nearest});
        std::array<RhiDescriptorBinding, 2> poolBindings = {{storageBinding[0], sampledBinding[0]}};
        pool = device().createDescriptorPool(2, poolBindings);
        device().allocateDescriptorSets(pool, layouts[0], {&sets[0], 1});
        device().allocateDescriptorSets(pool, layouts[1], {&sets[1], 1});
        std::array<RhiDescriptorWrite, 1> storageWrite = {{{.binding = 0, .type = RhiDescriptorType::StorageImage, .texture = image}}};
        std::array<RhiDescriptorWrite, 1> sampledWrite = {{{.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .texture = image, .sampler = sampler}}};
        device().updateDescriptorSet(sets[0], storageWrite);
        device().updateDescriptorSet(sets[1], sampledWrite);
        recorded.assign(frameCount(), false);
        return true;
    }

    // Zones must be read before the command buffer is reset for its next frame: begin()
    // clears them. The base calls this right after the slot's fence, before reset.
    auto slotReady(uint32_t slot, RhiCommandBuffer* cmd) -> void override {
        if (recorded[slot] && device().collectGpuZones(cmd, lastZones) && !lastZones.empty()) {
            samples++;
        }
    }

    auto record(RhiCommandBuffer* cmd, RhiTexture* backbuffer, RhiExtent2D extent) -> void override {
        recorded[frameSlot()] = true;

        cmd->beginGpuZone("frame");

        auto imageFrom = frameIndex() == 0 ? RhiTextureState::Undefined : RhiTextureState::ShaderReadOnly;
        std::array<RhiTextureBarrierDesc, 1> toGeneral = {{{.texture = image, .oldState = imageFrom, .newState = RhiTextureState::General}}};
        cmd->pipelineBarrier(toGeneral);

        cmd->beginGpuZone("compute");
        cmd->bindPipeline(computePipeline);
        cmd->bindDescriptorSet(computePipeline, 0, sets[0]);
        cmd->dispatch(imageSize / 8, imageSize / 8, 1);
        cmd->endGpuZone();

        std::array<RhiTextureBarrierDesc, 1> toSampled = {{{.texture = image, .oldState = RhiTextureState::General, .newState = RhiTextureState::ShaderReadOnly}}};
        cmd->pipelineBarrier(toSampled);

        cmd->beginGpuZone("draw");
        std::array<RhiRenderingAttachmentInfo, 1> colorAttachments = {{
            {.texture = backbuffer, .state = RhiTextureState::ColorAttachment, .clear = true, .clearColor = clearColor},
        }};
        cmd->beginRendering({.extent = extent, .colorAttachments = colorAttachments});
        cmd->setViewport(extent);
        cmd->setScissor(extent);
        cmd->bindPipeline(graphicsPipeline);
        cmd->bindDescriptorSet(graphicsPipeline, 0, sets[1]);
        cmd->bindVertexBuffer(vertexBuffer);
        cmd->bindIndexBuffer(indexBuffer, RhiIndexType::Uint16);
        cmd->drawIndexed(6, 1, 0, 0, 0);
        cmd->endRendering();
        cmd->endGpuZone();

        cmd->endGpuZone(); // frame
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        (void) frame;
        bool ok = true;
        auto report = [&](const char* label, bool pass) {
            std::println("check {}: {}", label, pass ? "ok" : "FAIL");
            ok = pass && ok;
        };
        report("zones-collected", samples > 0 && lastZones.size() == 3);
        if (lastZones.size() != 3) {
            return false;
        }
        const auto& frameZone = lastZones[0];
        const auto& computeZone = lastZones[1];
        const auto& drawZone = lastZones[2];
        report("names-in-record-order", std::string_view(frameZone.name) == "frame" && std::string_view(computeZone.name) == "compute" && std::string_view(drawZone.name) == "draw");
        report("depths", frameZone.depth == 0 && computeZone.depth == 1 && drawZone.depth == 1);
        report("children-inside-parent", computeZone.startNs >= frameZone.startNs && drawZone.endNs <= frameZone.endNs);
        report("compute-before-draw", computeZone.endNs <= drawZone.startNs);
        report("durations-positive", frameZone.endNs > frameZone.startNs && computeZone.endNs > computeZone.startNs && drawZone.endNs > drawZone.startNs);
        std::println("frame {:.3f} ms = compute {:.3f} + draw {:.3f} + gaps", (double) (frameZone.endNs - frameZone.startNs) * 1e-6, (double) (computeZone.endNs - computeZone.startNs) * 1e-6, (double) (drawZone.endNs - drawZone.startNs) * 1e-6);
        return ok;
    }

    auto teardown() -> void override {
        device().freeDescriptorSets(pool, sets);
        device().destroyDescriptorPool(pool);
        device().destroySampler(sampler);
        device().destroyBuffer(indexBuffer);
        device().destroyBuffer(vertexBuffer);
        device().destroyTexture(image);
        device().destroyPipeline(graphicsPipeline);
        device().destroyPipeline(computePipeline);
        for (auto* layout : layouts) {
            device().destroyDescriptorSetLayout(layout);
        }
        for (auto* shader : shaders) {
            device().destroyShaderModule(shader);
        }
    }

private:
    std::array<RhiShaderModule*, 3> shaders = {};
    std::array<RhiDescriptorSetLayout*, 2> layouts = {};
    std::array<RhiDescriptorSet*, 2> sets = {};
    RhiPipeline* computePipeline = nullptr;
    RhiPipeline* graphicsPipeline = nullptr;
    RhiTexture* image = nullptr;
    RhiBuffer* vertexBuffer = nullptr;
    RhiBuffer* indexBuffer = nullptr;
    RhiSampler* sampler = nullptr;
    RhiDescriptorPool* pool = nullptr;
    std::vector<bool> recorded;
    std::vector<RhiGpuZone> lastZones;
    uint32_t samples = 0;
};

auto main(int argc, char** argv) -> int {
    GpuZonesExample example;
    return example.run(argc, argv, "gpuzones");
}
