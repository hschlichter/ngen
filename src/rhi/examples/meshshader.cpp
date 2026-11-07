// RHI example: mesh shading.
//
// Adds to indirect: a pipeline with a task and a mesh shader instead of a vertex shader, drawn with
// drawMeshTasksIndirect (quads 0..3, the count on the CPU) and drawMeshTasksIndirectCount (quads 4..7, the count on the GPU). Each
// command (RhiDrawMeshTasksIndirectCommand) launches one task workgroup and is followed by a word of the example's own, the quad it
// draws: the commands are 16 bytes apart, and the task shader reads the word at gl_DrawID (counted from each call's first command, so
// the second call's task shader adds a push-constant base). The task shader culls a quad whose centre lies right of a push-constant line
// by launching no mesh workgroup; the mesh shader builds the quad's four vertices and two triangles. The GPU count stops one command
// short.
//
//   row 0   quads 0..3   quad 3 culled by the task shader
//   row 1   quads 4..7   quad 7 culled by the task shader and past the GPU count
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-meshshader --frames=60 --check --validation

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
static constexpr uint32_t gpuCount = 7;
static constexpr uint32_t firstCallCount = 4;
// Quads whose centre x lies right of this are culled by the task shader (column 3: quads 3 and 7).
static constexpr float cullRightOf = 0.5f;

static constexpr const char* taskShaderSource = R"glsl(
#version 460
#extension GL_EXT_mesh_shader : require

layout(local_size_x = 1) in;

layout(push_constant) uniform Push {
    float cullRightOf;
    uint firstCommand;
} push;

// The commands buffer, read as words: four per command, the fourth the quad.
layout(std430, set = 0, binding = 1) readonly buffer Commands {
    uint words[];
};

struct Payload {
    uint quad;
};
taskPayloadSharedEXT Payload payload;

vec2 quadCenter(uint q) {
    return vec2(-0.75 + float(q % 4u) * 0.5, q < 4u ? -0.5 : 0.5);
}

void main() {
    uint quad = words[(push.firstCommand + gl_DrawID) * 4u + 3u];
    payload.quad = quad;
    EmitMeshTasksEXT(quadCenter(quad).x > push.cullRightOf ? 0u : 1u, 1u, 1u);
}
)glsl";

static constexpr const char* meshShaderSource = R"glsl(
#version 450
#extension GL_EXT_mesh_shader : require

layout(local_size_x = 1) in;
layout(triangles, max_vertices = 4, max_primitives = 2) out;

layout(std430, set = 0, binding = 0) readonly buffer Colors {
    vec4 colors[];
};

struct Payload {
    uint quad;
};
taskPayloadSharedEXT Payload payload;

layout(location = 0) flat out vec4 fragColor[];

const float quadHalf = 0.2;

void main() {
    uint q = payload.quad;
    vec2 c = vec2(-0.75 + float(q % 4u) * 0.5, q < 4u ? -0.5 : 0.5);
    SetMeshOutputsEXT(4u, 2u);
    gl_MeshVerticesEXT[0].gl_Position = vec4(c + vec2(-quadHalf, -quadHalf), 0.0, 1.0);
    gl_MeshVerticesEXT[1].gl_Position = vec4(c + vec2(quadHalf, -quadHalf), 0.0, 1.0);
    gl_MeshVerticesEXT[2].gl_Position = vec4(c + vec2(quadHalf, quadHalf), 0.0, 1.0);
    gl_MeshVerticesEXT[3].gl_Position = vec4(c + vec2(-quadHalf, quadHalf), 0.0, 1.0);
    for (uint v = 0; v < 4u; v++) {
        fragColor[v] = colors[q];
    }
    gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
    gl_PrimitiveTriangleIndicesEXT[1] = uvec3(2, 3, 0);
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

static auto quadCenter(uint32_t q) -> std::array<float, 2> {
    auto column = (float) (q % 4);
    auto row = q < 4 ? 0.0f : 1.0f;
    return {-0.75f + (column * 0.5f), -0.5f + row};
}

// The task shader's push constants.
struct TaskPush {
    float cullRightOf = 0.0f;
    uint32_t firstCommand = 0;
};

// A command and the example's own word after it.
struct QuadCommand {
    RhiDrawMeshTasksIndirectCommand draw;
    uint32_t quad = 0;
};
static_assert(sizeof(QuadCommand) == 16);

class MeshShaderExample : public RhiExample {
protected:
    auto setup() -> bool override {
        auto taskSpirv = compileGlsl(RhiShaderStage::Task, taskShaderSource, "meshshader.task");
        auto meshSpirv = compileGlsl(RhiShaderStage::Mesh, meshShaderSource, "meshshader.mesh");
        auto fragmentSpirv = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "meshshader.frag");
        if (taskSpirv.empty() || meshSpirv.empty() || fragmentSpirv.empty()) {
            return false;
        }
        taskShader = device().createShaderModule({.stage = RhiShaderStage::Task, .code = taskSpirv});
        meshShader = device().createShaderModule({.stage = RhiShaderStage::Mesh, .code = meshSpirv});
        fragmentShader = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fragmentSpirv});

        std::array<RhiDescriptorBinding, 2> bindings = {{
            {.binding = 0, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Mesh},
            {.binding = 1, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Task},
        }};
        setLayout = device().createDescriptorSetLayout(bindings);

        auto format = colorFormat();
        RhiGraphicsPipelineDesc pipelineDesc = {
            .fragmentShader = fragmentShader,
            .taskShader = taskShader,
            .meshShader = meshShader,
            .descriptorSetLayouts = {&setLayout, 1},
            .pushConstant = {.stage = RhiShaderStage::Task, .offset = 0, .size = sizeof(TaskPush)},
            .colorFormats = {&format, 1},
            .raster = {.cullMode = RhiCullMode::None},
            .depth = {.testEnable = false, .writeEnable = false},
        };
        pipeline = device().createGraphicsPipeline(pipelineDesc);
        if (pipeline == nullptr) {
            return false;
        }

        std::vector<QuadCommand> commands;
        for (uint32_t q = 0; q < quadCount; q++) {
            commands.push_back({.draw = {.groupCountX = 1, .groupCountY = 1, .groupCountZ = 1}, .quad = q});
        }
        std::array<uint32_t, 1> count = {gpuCount - firstCallCount};
        {
            UploadBatch upload(device());
            colorBuffer = upload.buffer(std::as_bytes(std::span(quadColors)), RhiBufferUsage::Storage);
            commandBuffer = upload.buffer(std::as_bytes(std::span(commands)), RhiBufferUsage::Indirect | RhiBufferUsage::Storage);
            countBuffer = upload.buffer(std::as_bytes(std::span(count)), RhiBufferUsage::Indirect);
            upload.finish();
        }

        pool = device().createDescriptorPool(1, bindings);
        device().allocateDescriptorSets(pool, setLayout, {&descriptorSet, 1});
        std::array<RhiDescriptorWrite, 2> writes = {{
            {.binding = 0, .type = RhiDescriptorType::StorageBuffer, .buffer = colorBuffer},
            {.binding = 1, .type = RhiDescriptorType::StorageBuffer, .buffer = commandBuffer},
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
        TaskPush push = {.cullRightOf = cullRightOf, .firstCommand = 0};
        cmd->pushConstants(pipeline, RhiShaderStage::Task, 0, sizeof(push), &push);
        cmd->drawMeshTasksIndirect(commandBuffer, 0, firstCallCount, sizeof(QuadCommand));
        push.firstCommand = firstCallCount;
        cmd->pushConstants(pipeline, RhiShaderStage::Task, 0, sizeof(push), &push);
        cmd->drawMeshTasksIndirectCount(commandBuffer, firstCallCount * sizeof(QuadCommand), sizeof(QuadCommand), countBuffer, 0, quadCount - firstCallCount);
        cmd->endRendering();
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        bool ok = true;
        ok = expectCount("stats-indirect-draws", frame.stats.indirectDraws, 2) && ok;
        ok = expectCount("stats-direct-draws", frame.stats.draws, 0) && ok;
        for (uint32_t q = 0; q < quadCount; q++) {
            auto c = quadCenter(q);
            bool culled = c[0] > cullRightOf;
            bool pastCount = q >= gpuCount;
            std::array<float, 3> expected = {clearColor[0], clearColor[1], clearColor[2]};
            if (!culled && !pastCount) {
                expected = {quadColors[q][0], quadColors[q][1], quadColors[q][2]};
            }
            auto name = std::format("quad-{}-{}", q, culled ? "culled" : (pastCount ? "past-gpu-count" : "drawn"));
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
        device().destroyPipeline(pipeline);
        device().destroyDescriptorSetLayout(setLayout);
        device().destroyShaderModule(fragmentShader);
        device().destroyShaderModule(meshShader);
        device().destroyShaderModule(taskShader);
    }

private:
    RhiShaderModule* taskShader = nullptr;
    RhiShaderModule* meshShader = nullptr;
    RhiShaderModule* fragmentShader = nullptr;
    RhiDescriptorSetLayout* setLayout = nullptr;
    RhiPipeline* pipeline = nullptr;
    RhiBuffer* colorBuffer = nullptr;
    RhiBuffer* commandBuffer = nullptr;
    RhiBuffer* countBuffer = nullptr;
    RhiDescriptorPool* pool = nullptr;
    RhiDescriptorSet* descriptorSet = nullptr;
};

auto main(int argc, char** argv) -> int {
    MeshShaderExample example;
    return example.run(argc, argv, "meshshader");
}
