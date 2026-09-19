// RHI example: GPU timestamps.
//
// Adds to compute: a query pool per frame slot, reset and written on the command
// buffer around a deliberately heavy compute dispatch, read back once the slot's
// fence has signalled, converted with limits().timestampPeriodNs. The measured
// time is printed every 30 frames; --check asserts it is positive and plausible.
//
// The dispatch is a brute-force loop over a storage image so the interval is
// large enough to be unmistakable. A quad shows the result to keep the frame honest.
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-timestamps --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"
#include "common/upload.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <print>
#include <span>
#include <vector>

static constexpr uint32_t imageSize = 512;
static constexpr uint32_t iterations = 256; // per pixel; makes the dispatch take measurable time

// Compute: iterated sine per pixel; output brightness is deterministic and the
// centre pixel is asserted by --check.
static constexpr const char* computeSource = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, rgba8) uniform writeonly image2D outImage;
layout(push_constant) uniform Push { uint iterations; } push;

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    float v = float(p.x + p.y) * 0.001;
    for (uint i = 0u; i < push.iterations; i++) {
        v = sin(v) * 0.5 + 0.5;
    }
    // v = sin(v) * 0.5 + 0.5 converges to about 0.8879 for any start; write that as grey.
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

// Fixed point of v = sin(v) * 0.5 + 0.5, reached long before 256 iterations.
static constexpr float convergedGrey = 0.8879f;
static constexpr float quadHalf = 0.5f;

class TimestampsExample : public RhiExample {
protected:
    auto setup() -> bool override {
        const auto& limits = device().limits();
        if (!limits.timestamps) {
            std::println(stderr, "timestamps not supported on this device");
            return false;
        }
        std::println("timestamp period {} ns per tick", limits.timestampPeriodNs);

        auto cs = compileGlsl(RhiShaderStage::Compute, computeSource, "timestamps.comp");
        auto vs = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "timestamps.vert");
        auto fs = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "timestamps.frag");
        if (cs.empty() || vs.empty() || fs.empty()) {
            return false;
        }
        shaders[0] = device().createShaderModule({.stage = RhiShaderStage::Compute, .code = cs});
        shaders[1] = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vs});
        shaders[2] = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fs});

        RhiTextureDesc imageDesc = {.width = imageSize, .height = imageSize, .format = RhiFormat::R8G8B8A8_UNORM, .usage = RhiTextureUsage::Storage | RhiTextureUsage::Sampled};
        image = device().createTexture(imageDesc);

        std::array<RhiDescriptorBinding, 1> storageBinding = {{{.binding = 0, .type = RhiDescriptorType::StorageImage, .stage = RhiShaderStage::Compute}}};
        std::array<RhiDescriptorBinding, 1> sampledBinding = {{{.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Fragment}}};
        layouts[0] = device().createDescriptorSetLayout(storageBinding);
        layouts[1] = device().createDescriptorSetLayout(sampledBinding);

        computePipeline = device().createComputePipeline({
            .shader = shaders[0],
            .descriptorSetLayouts = {&layouts[0], 1},
            .pushConstant = {.stage = RhiShaderStage::Compute, .offset = 0, .size = sizeof(uint32_t)},
        });

        std::array<RhiVertexAttribute, 2> attributes = {{
            {.location = 0, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = 0},
            {.location = 1, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = sizeof(float) * 2},
        }};
        auto format = colorFormat();
        RhiGraphicsPipelineDesc graphicsDesc = {
            .vertexShader = shaders[1],
            .fragmentShader = shaders[2],
            .descriptorSetLayouts = {&layouts[1], 1},
            .colorFormats = {&format, 1},
            .vertexStride = sizeof(float) * 4,
            .vertexAttributes = attributes,
            .raster = {.cullMode = RhiCullMode::None},
            .depth = {.testEnable = false, .writeEnable = false},
        };
        graphicsPipeline = device().createGraphicsPipeline(graphicsDesc);
        if (computePipeline == nullptr || graphicsPipeline == nullptr) {
            return false;
        }

        std::array<float, 16> vertices = {-quadHalf, -quadHalf, 0, 0, quadHalf, -quadHalf, 1, 0, quadHalf, quadHalf, 1, 1, -quadHalf, quadHalf, 0, 1};
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

        // One pool per frame slot: two timestamps around the dispatch, two around the draw.
        queryPools.resize(frameCount());
        for (auto& qp : queryPools) {
            qp = device().createQueryPool(4);
        }
        return true;
    }

    auto record(RhiCommandBuffer* cmd, RhiTexture* backbuffer, RhiExtent2D extent) -> void override {
        auto slot = frameSlot();
        auto* qp = queryPools[slot];

        // The base waited on this slot's fence, so the slot's previous results are complete.
        std::array<uint64_t, 4> ticks = {};
        if (frameIndex() >= frameCount() && device().readTimestamps(qp, 0, ticks)) {
            auto period = (double) device().limits().timestampPeriodNs * 1e-6;
            lastComputeMs = (double) (ticks[1] - ticks[0]) * period;
            lastDrawMs = (double) (ticks[3] - ticks[2]) * period;
            samples++;
            if (frameIndex() % 30 == 0) {
                std::println("frame {}: compute {:.3f} ms, draw {:.3f} ms", frameIndex(), lastComputeMs, lastDrawMs);
            }
        }

        // Reset must happen outside rendering; the whole pool is rewritten each frame.
        cmd->resetQueryPool(qp, 0, 4);

        auto imageFrom = frameIndex() == 0 ? RhiTextureState::Undefined : RhiTextureState::ShaderReadOnly;
        std::array<RhiTextureBarrierDesc, 1> toGeneral = {{{.texture = image, .oldState = imageFrom, .newState = RhiTextureState::General}}};
        cmd->pipelineBarrier(toGeneral);

        cmd->writeTimestamp(qp, 0);
        cmd->bindPipeline(computePipeline);
        cmd->bindDescriptorSet(computePipeline, 0, sets[0]);
        uint32_t iterationCount = iterations;
        cmd->pushConstants(computePipeline, RhiShaderStage::Compute, 0, sizeof(iterationCount), &iterationCount);
        cmd->dispatch(imageSize / 8, imageSize / 8, 1);
        cmd->writeTimestamp(qp, 1);

        std::array<RhiTextureBarrierDesc, 1> toSampled = {{{.texture = image, .oldState = RhiTextureState::General, .newState = RhiTextureState::ShaderReadOnly}}};
        cmd->pipelineBarrier(toSampled);

        std::array<RhiRenderingAttachmentInfo, 1> colorAttachments = {{
            {.texture = backbuffer, .state = RhiTextureState::ColorAttachment, .clear = true, .clearColor = clearColor},
        }};
        cmd->writeTimestamp(qp, 2);
        cmd->beginRendering({.extent = extent, .colorAttachments = colorAttachments});
        cmd->setViewport(extent);
        cmd->setScissor(extent);
        cmd->bindPipeline(graphicsPipeline);
        cmd->bindDescriptorSet(graphicsPipeline, 0, sets[1]);
        cmd->bindVertexBuffer(vertexBuffer);
        cmd->bindIndexBuffer(indexBuffer, RhiIndexType::Uint16);
        cmd->drawIndexed(6, 1, 0, 0, 0);
        cmd->endRendering();
        cmd->writeTimestamp(qp, 3);
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        bool ok = true;
        ok = expectPixel(frame, frame.px(0.0f), frame.py(0.0f), {convergedGrey, convergedGrey, convergedGrey}, "compute-output-grey") && ok;

        // Timing checks: positive, below a second, and the loop of 256 sines over 262144
        // pixels must take longer than one sampled quad.
        bool haveSamples = samples > 0;
        std::println("check timestamps-read: {} samples {}", samples, haveSamples ? "ok" : "FAIL");
        ok = haveSamples && ok;
        bool computePlausible = lastComputeMs > 0.0 && lastComputeMs < 1000.0;
        std::println("check compute-time-plausible: {:.3f} ms {}", lastComputeMs, computePlausible ? "ok" : "FAIL");
        ok = computePlausible && ok;
        bool drawPlausible = lastDrawMs > 0.0 && lastDrawMs < 1000.0;
        std::println("check draw-time-plausible: {:.3f} ms {}", lastDrawMs, drawPlausible ? "ok" : "FAIL");
        ok = drawPlausible && ok;
        bool ordered = lastComputeMs > lastDrawMs;
        std::println("check compute-slower-than-draw: {}", ordered ? "ok" : "FAIL");
        ok = ordered && ok;
        return ok;
    }

    auto teardown() -> void override {
        for (auto* qp : queryPools) {
            device().destroyQueryPool(qp);
        }
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
    std::vector<RhiQueryPool*> queryPools;
    double lastComputeMs = 0.0;
    double lastDrawMs = 0.0;
    uint32_t samples = 0;
};

auto main(int argc, char** argv) -> int {
    TimestampsExample example;
    return example.run(argc, argv, "timestamps");
}
