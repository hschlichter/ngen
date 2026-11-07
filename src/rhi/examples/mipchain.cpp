// RHI example: a mip chain written by compute, one level from the one below.
//
// Adds to compute: storage views of single mip levels (RhiDescriptorWrite::mipLevel), the R32_SFLOAT format, and the barrier that makes
// one dispatch's storage writes visible to the next while the texture stays General (General -> General). A 16x16 texture with 5 levels:
//
//   level 0  value(x, y) = 1 - (x + y) / 64, written by the first dispatch
//   level k  each texel the minimum of the 2x2 texels of level k - 1 below it, one dispatch per level
//
// Then 5 quads, left to right, show texel (0, 0) of levels 0..4 as grey: the minimum over the first 2^k x 2^k texels of level 0,
// 1 - (2^k - 1) / 32. --check reads each quad's centre.
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-mipchain --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"

#include <array>
#include <cstdint>
#include <format>
#include <print>
#include <vector>

static constexpr uint32_t textureSize = 16;
static constexpr uint32_t levelCount = 5;

// The value of texel (0, 0) at a level: the minimum of 1 - (x + y) / 64 over [0, 2^level)^2, at x = y = 2^level - 1.
static auto expectedValue(uint32_t level) -> float {
    return 1.0f - ((float) ((1u << level) - 1) / 32.0f);
}

static constexpr const char* fillSource = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1, r32f) uniform writeonly image2D target;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    imageStore(target, p, vec4(1.0 - float(p.x + p.y) / 64.0));
}
)glsl";

static constexpr const char* reduceSource = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, r32f) uniform readonly image2D source;
layout(set = 0, binding = 1, r32f) uniform writeonly image2D target;
layout(push_constant) uniform Push { uint size; } push;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(push.size) || p.y >= int(push.size)) {
        return;
    }
    float a = imageLoad(source, p * 2).r;
    float b = imageLoad(source, p * 2 + ivec2(1, 0)).r;
    float c = imageLoad(source, p * 2 + ivec2(0, 1)).r;
    float d = imageLoad(source, p * 2 + ivec2(1, 1)).r;
    imageStore(target, p, vec4(min(min(a, b), min(c, d))));
}
)glsl";

// A quad per level, from gl_VertexIndex: no vertex buffer.
static constexpr const char* vertexShaderSource = R"glsl(
#version 450
layout(push_constant) uniform Push { int level; } push;
void main() {
    vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(1, 1), vec2(0, 1), vec2(0, 0));
    vec2 c = corners[gl_VertexIndex];
    float left = -0.95 + float(push.level) * 0.38;
    gl_Position = vec4(left + c.x * 0.3, -0.3 + c.y * 0.6, 0.0, 1.0);
}
)glsl";

static constexpr const char* fragmentShaderSource = R"glsl(
#version 450
layout(set = 0, binding = 0) uniform sampler2D chain;
layout(push_constant) uniform Push { int level; } push;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(vec3(texelFetch(chain, ivec2(0, 0), push.level).r), 1.0);
}
)glsl";

class MipChainExample : public RhiExample {
protected:
    auto setup() -> bool override {
        auto fill = compileGlsl(RhiShaderStage::Compute, fillSource, "mipchain.fill.comp");
        auto reduce = compileGlsl(RhiShaderStage::Compute, reduceSource, "mipchain.reduce.comp");
        auto vs = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "mipchain.vert");
        auto fs = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "mipchain.frag");
        if (fill.empty() || reduce.empty() || vs.empty() || fs.empty()) {
            return false;
        }
        if (!device().supportsTextureFormat(RhiFormat::R32_SFLOAT, RhiTextureUsage::Storage | RhiTextureUsage::Sampled)) {
            std::println(stderr, "R32_SFLOAT not usable as a storage image on this device");
            return false;
        }
        shaders[0] = device().createShaderModule({.stage = RhiShaderStage::Compute, .code = fill});
        shaders[1] = device().createShaderModule({.stage = RhiShaderStage::Compute, .code = reduce});
        shaders[2] = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vs});
        shaders[3] = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fs});

        chain = device().createTexture({
            .width = textureSize,
            .height = textureSize,
            .format = RhiFormat::R32_SFLOAT,
            .usage = RhiTextureUsage::Storage | RhiTextureUsage::Sampled,
            .mipLevels = levelCount,
        });

        // One layout for both compute pipelines: the level below and the level written, each a single-level storage view.
        std::array<RhiDescriptorBinding, 2> computeBindings = {{
            {.binding = 0, .type = RhiDescriptorType::StorageImage, .stage = RhiShaderStage::Compute},
            {.binding = 1, .type = RhiDescriptorType::StorageImage, .stage = RhiShaderStage::Compute},
        }};
        std::array<RhiDescriptorBinding, 1> sampledBindings = {{
            {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Fragment},
        }};
        computeLayout = device().createDescriptorSetLayout(computeBindings);
        sampledLayout = device().createDescriptorSetLayout(sampledBindings);
        fillPipeline = device().createComputePipeline({.shader = shaders[0], .descriptorSetLayouts = {&computeLayout, 1}});
        reducePipeline = device().createComputePipeline({
            .shader = shaders[1],
            .descriptorSetLayouts = {&computeLayout, 1},
            .pushConstant = {.stage = RhiShaderStage::Compute, .offset = 0, .size = sizeof(uint32_t)},
        });
        auto format = colorFormat();
        drawPipeline = device().createGraphicsPipeline({
            .vertexShader = shaders[2],
            .fragmentShader = shaders[3],
            .descriptorSetLayouts = {&sampledLayout, 1},
            .pushConstant = {.stage = RhiShaderStage::Vertex | RhiShaderStage::Fragment, .offset = 0, .size = sizeof(int32_t)},
            .colorFormats = {&format, 1},
            .raster = {.cullMode = RhiCullMode::None},
            .depth = {.testEnable = false, .writeEnable = false},
        });
        if (fillPipeline == nullptr || reducePipeline == nullptr || drawPipeline == nullptr) {
            return false;
        }

        sampler = device().createSampler({.magFilter = RhiFilter::Nearest, .minFilter = RhiFilter::Nearest, .mipmapMode = RhiMipmapMode::Nearest});
        std::array<RhiDescriptorBinding, 3> poolBindings = {{computeBindings[0], computeBindings[1], sampledBindings[0]}};
        pool = device().createDescriptorPool(levelCount + 1, poolBindings);
        levelSets.assign(levelCount, nullptr);
        for (uint32_t level = 0; level < levelCount; level++) {
            device().allocateDescriptorSets(pool, computeLayout, {&levelSets[level], 1});
            std::array<RhiDescriptorWrite, 2> writes = {{
                {.binding = 0, .type = RhiDescriptorType::StorageImage, .texture = chain, .mipLevel = level > 0 ? level - 1 : 0},
                {.binding = 1, .type = RhiDescriptorType::StorageImage, .texture = chain, .mipLevel = level},
            }};
            device().updateDescriptorSet(levelSets[level], writes);
        }
        device().allocateDescriptorSets(pool, sampledLayout, {&sampledSet, 1});
        std::array<RhiDescriptorWrite, 1> sampledWrite = {{{.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .texture = chain, .sampler = sampler}}};
        device().updateDescriptorSet(sampledSet, sampledWrite);
        return true;
    }

    auto record(RhiCommandBuffer* cmd, RhiTexture* backbuffer, RhiExtent2D extent) -> void override {
        auto from = frameIndex() == 0 ? RhiTextureState::Undefined : RhiTextureState::ShaderReadOnly;
        std::array<RhiTextureBarrierDesc, 1> toGeneral = {{{.texture = chain, .oldState = from, .newState = RhiTextureState::General}}};
        cmd->pipelineBarrier(toGeneral);

        cmd->bindPipeline(fillPipeline);
        cmd->bindDescriptorSet(fillPipeline, 0, levelSets[0]);
        cmd->dispatch(textureSize / 8, textureSize / 8, 1);
        cmd->bindPipeline(reducePipeline);
        for (uint32_t level = 1; level < levelCount; level++) {
            // The level below is written: make its writes visible to this dispatch's reads; the texture stays General.
            std::array<RhiTextureBarrierDesc, 1> between = {{{.texture = chain, .oldState = RhiTextureState::General, .newState = RhiTextureState::General}}};
            cmd->pipelineBarrier(between);
            uint32_t size = textureSize >> level;
            cmd->bindDescriptorSet(reducePipeline, 0, levelSets[level]);
            cmd->pushConstants(reducePipeline, RhiShaderStage::Compute, 0, sizeof(size), &size);
            cmd->dispatch((size + 7) / 8, (size + 7) / 8, 1);
        }
        std::array<RhiTextureBarrierDesc, 1> toSampled = {{{.texture = chain, .oldState = RhiTextureState::General, .newState = RhiTextureState::ShaderReadOnly}}};
        cmd->pipelineBarrier(toSampled);

        std::array<RhiRenderingAttachmentInfo, 1> colorAttachments = {{
            {.texture = backbuffer, .state = RhiTextureState::ColorAttachment, .clear = true, .clearColor = clearColor},
        }};
        cmd->beginRendering({.extent = extent, .colorAttachments = colorAttachments});
        cmd->setViewport(extent);
        cmd->setScissor(extent);
        cmd->bindPipeline(drawPipeline);
        cmd->bindDescriptorSet(drawPipeline, 0, sampledSet);
        for (int32_t level = 0; level < (int32_t) levelCount; level++) {
            cmd->pushConstants(drawPipeline, RhiShaderStage::Vertex | RhiShaderStage::Fragment, 0, sizeof(level), &level);
            cmd->draw(6, 1, 0, 0);
        }
        cmd->endRendering();
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        bool ok = true;
        for (uint32_t level = 0; level < levelCount; level++) {
            float x = -0.95f + ((float) level * 0.38f) + 0.15f;
            auto value = expectedValue(level);
            std::array<float, 3> expected = {value, value, value};
            auto name = std::format("level-{}-texel-0-0", level);
            ok = expectPixel(frame, frame.px(x), frame.py(0.0f), expected, name.c_str()) && ok;
        }
        return ok;
    }

    auto teardown() -> void override {
        device().freeDescriptorSets(pool, levelSets);
        device().freeDescriptorSets(pool, {&sampledSet, 1});
        device().destroyDescriptorPool(pool);
        device().destroySampler(sampler);
        device().destroyTexture(chain);
        device().destroyPipeline(drawPipeline);
        device().destroyPipeline(reducePipeline);
        device().destroyPipeline(fillPipeline);
        device().destroyDescriptorSetLayout(sampledLayout);
        device().destroyDescriptorSetLayout(computeLayout);
        for (auto* shader : shaders) {
            device().destroyShaderModule(shader);
        }
    }

private:
    std::array<RhiShaderModule*, 4> shaders = {};
    RhiDescriptorSetLayout* computeLayout = nullptr;
    RhiDescriptorSetLayout* sampledLayout = nullptr;
    RhiPipeline* fillPipeline = nullptr;
    RhiPipeline* reducePipeline = nullptr;
    RhiPipeline* drawPipeline = nullptr;
    RhiTexture* chain = nullptr;
    RhiSampler* sampler = nullptr;
    RhiDescriptorPool* pool = nullptr;
    std::vector<RhiDescriptorSet*> levelSets;
    RhiDescriptorSet* sampledSet = nullptr;
};

auto main(int argc, char** argv) -> int {
    MipChainExample example;
    return example.run(argc, argv, "mipchain");
}
