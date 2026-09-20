// RHI example: depth testing.
//
// Adds to quad: an example-owned depth texture attached alongside the colour
// target, recreated on resize through the resized() hook, and four pipelines
// that differ only in RhiDepthState (the fourth is the Equal test a depth prepass needs). Each column draws a near quad first and a
// far quad second where they overlap:
//
//   left    test on,  write on    near wins (depth rejects the far quad)
//   middle  test off              far wins (last draw wins)
//   right   test on,  write off   far wins (near left no depth behind)
//
// --depth-format=d32|d24s8|d32s8|stencil selects the depth format; every choice
// must give the same picture. Depth-stencil formats are optional per device, so the
// example asks supportsTextureFormat first and falls back to a supported one.
// "stencil" picks whichever packed depth-stencil format the device has.
//
// Unattended run: SDL_VIDEODRIVER=offscreen ngen-example-depth --frames=60 --check --validation

#include "common/rhiexample.h"
#include "common/shadercompile.h"
#include "common/upload.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <print>
#include <span>
#include <string_view>
#include <vector>

static constexpr const char* vertexShaderSource = R"glsl(
#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = vec4(inPosition, 1.0);
    fragColor = inColor;
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

struct Vertex {
    float x;
    float y;
    float z;
    float r;
    float g;
    float b;
};

static constexpr std::array<float, 3> nearColor = {0.9f, 0.2f, 0.2f};
static constexpr std::array<float, 3> farColor = {0.2f, 0.4f, 0.9f};
static constexpr float nearZ = 0.3f;
static constexpr float farZ = 0.7f;
static constexpr std::array<float, 3> equalColor = {0.2f, 0.9f, 0.3f};
static constexpr float quadHalf = 0.16f;
static constexpr float pairShift = 0.1f; // near quad offset (-,-), far quad (+,+); overlap centred on the column
static constexpr std::array<float, 4> columnX = {-0.72f, -0.24f, 0.24f, 0.72f};

static auto pushQuad(std::vector<Vertex>& out, float cx, float cy, float z, std::array<float, 3> c) -> void {
    out.push_back({cx - quadHalf, cy - quadHalf, z, c[0], c[1], c[2]});
    out.push_back({cx + quadHalf, cy - quadHalf, z, c[0], c[1], c[2]});
    out.push_back({cx + quadHalf, cy + quadHalf, z, c[0], c[1], c[2]});
    out.push_back({cx - quadHalf, cy + quadHalf, z, c[0], c[1], c[2]});
}

class DepthExample : public RhiExample {
protected:
    auto parseArg(std::string_view arg) -> bool override {
        if (arg == "--depth-format=d32") {
            requestedFormat = RhiFormat::D32_SFLOAT;
            return true;
        }
        if (arg == "--depth-format=d24s8") {
            requestedFormat = RhiFormat::D24_UNORM_S8_UINT;
            return true;
        }
        if (arg == "--depth-format=d32s8") {
            requestedFormat = RhiFormat::D32_SFLOAT_S8_UINT;
            return true;
        }
        if (arg == "--depth-format=stencil") {
            requestedFormat = RhiFormat::Undefined; // resolved in setup
            wantStencil = true;
            return true;
        }
        return false;
    }

    auto chooseDepthFormat() -> RhiFormat {
        auto usage = RhiTextureUsageFlags(RhiTextureUsage::DepthAttachment);
        if (wantStencil) {
            for (auto candidate : {RhiFormat::D24_UNORM_S8_UINT, RhiFormat::D32_SFLOAT_S8_UINT}) {
                if (device().supportsTextureFormat(candidate, usage)) {
                    return candidate;
                }
            }
            std::println(stderr, "no depth-stencil format supported; falling back to D32_SFLOAT");
            return RhiFormat::D32_SFLOAT;
        }
        if (device().supportsTextureFormat(requestedFormat, usage)) {
            return requestedFormat;
        }
        std::println("requested depth format {} not supported on this device; falling back to D32_SFLOAT", depthFormatName(requestedFormat));
        return RhiFormat::D32_SFLOAT;
    }

    auto setup() -> bool override {
        depthFormat = chooseDepthFormat();
        std::println("depth format: {}", depthFormatName(depthFormat));

        auto vertexSpirv = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "depth.vert");
        auto fragmentSpirv = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "depth.frag");
        if (vertexSpirv.empty() || fragmentSpirv.empty()) {
            return false;
        }
        vertexShader = device().createShaderModule({.stage = RhiShaderStage::Vertex, .code = vertexSpirv});
        fragmentShader = device().createShaderModule({.stage = RhiShaderStage::Fragment, .code = fragmentSpirv});

        std::array<RhiVertexAttribute, 2> attributes = {{
            {.location = 0, .binding = 0, .format = RhiFormat::R32G32B32_SFLOAT, .offset = offsetof(Vertex, x)},
            {.location = 1, .binding = 0, .format = RhiFormat::R32G32B32_SFLOAT, .offset = offsetof(Vertex, r)},
        }};
        auto format = colorFormat();
        std::array<RhiDepthState, 4> depthStates = {{
            {.testEnable = true, .writeEnable = true},
            {.testEnable = false, .writeEnable = false},
            {.testEnable = true, .writeEnable = false},
            {.testEnable = true, .writeEnable = false, .compareOp = RhiCompareOp::Equal}, // depth prepass contract
        }};
        for (size_t i = 0; i < 4; i++) {
            RhiGraphicsPipelineDesc pipelineDesc = {
                .vertexShader = vertexShader,
                .fragmentShader = fragmentShader,
                .colorFormats = {&format, 1},
                .depthFormat = depthFormat,
                .vertexStride = sizeof(Vertex),
                .vertexAttributes = attributes,
                .raster = {.cullMode = RhiCullMode::None},
                .depth = depthStates[i],
            };
            pipelines[i] = device().createGraphicsPipeline(pipelineDesc);
            if (pipelines[i] == nullptr) {
                return false;
            }
        }

        // Per column: near quad (4 verts) then far quad (4 verts); 12 indices. The Equal
        // column adds a third quad at the near quad's exact position and depth in equalColor.
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        for (auto cx : columnX) {
            auto base = (uint16_t) vertices.size();
            pushQuad(vertices, cx - pairShift, -pairShift, nearZ, nearColor);
            pushQuad(vertices, cx + pairShift, pairShift, farZ, farColor);
            for (auto i : {0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4}) {
                indices.push_back((uint16_t) (base + i));
            }
        }
        {
            auto base = (uint16_t) vertices.size();
            pushQuad(vertices, columnX[3] - pairShift, -pairShift, nearZ, equalColor);
            for (auto i : {0, 1, 2, 2, 3, 0}) {
                indices.push_back((uint16_t) (base + i));
            }
        }
        {
            UploadBatch upload(device());
            vertexBuffer = upload.buffer(std::as_bytes(std::span(vertices)), RhiBufferUsage::Vertex);
            indexBuffer = upload.buffer(std::as_bytes(std::span(indices)), RhiBufferUsage::Index);
            upload.finish();
        }

        createDepthTexture(swapchain()->extent());
        return true;
    }

    // The depth texture must match the swapchain size; the base calls this after
    // a successful recreate with the GPU idle, so immediate destroy is safe.
    auto resized(RhiExtent2D extent) -> void override {
        device().destroyTexture(depthTexture);
        createDepthTexture(extent);
    }

    auto record(RhiCommandBuffer* cmd, RhiTexture* backbuffer, RhiExtent2D extent) -> void override {
        // Depth contents are not kept between frames: start from Undefined and clear.
        std::array<RhiTextureBarrierDesc, 1> depthBarrier = {{
            {.texture = depthTexture, .oldState = RhiTextureState::Undefined, .newState = RhiTextureState::DepthStencilAttachment},
        }};
        cmd->pipelineBarrier(depthBarrier);

        std::array<RhiRenderingAttachmentInfo, 1> colorAttachments = {{
            {.texture = backbuffer, .state = RhiTextureState::ColorAttachment, .clear = true, .clearColor = clearColor},
        }};
        RhiRenderingAttachmentInfo depthAttachment = {
            .texture = depthTexture,
            .state = RhiTextureState::DepthStencilAttachment,
            .clear = true,
            .clearDepth = 1.0f,
        };
        cmd->beginRendering({.extent = extent, .colorAttachments = colorAttachments, .depthAttachment = &depthAttachment});
        cmd->setViewport(extent);
        cmd->setScissor(extent);
        cmd->bindVertexBuffer(vertexBuffer);
        cmd->bindIndexBuffer(indexBuffer, RhiIndexType::Uint16);
        for (uint32_t column = 0; column < 3; column++) {
            cmd->bindPipeline(pipelines[column]);
            cmd->drawIndexed(12, 1, column * 12, 0, 0);
        }
        // Equal column: the near quad lays down depth with Less+write, then the far quad
        // (depth 0.7 against cleared 1.0, rejected) and the equalColor quad (same depth as
        // near, accepted) go through the Equal pipeline. Indices: near at 36, far at 42, equal at 48.
        cmd->bindPipeline(pipelines[0]);
        cmd->drawIndexed(6, 1, 3 * 12, 0, 0);
        cmd->bindPipeline(pipelines[3]);
        cmd->drawIndexed(6, 1, (3 * 12) + 6, 0, 0);
        cmd->drawIndexed(6, 1, 4 * 12, 0, 0);
        cmd->endRendering();
    }

    auto check(const RhiExampleFrame& frame) -> bool override {
        bool ok = true;
        ok = expectPixel(frame, frame.px(columnX[0]), frame.py(0.0f), nearColor, "test-on-write-on-near-wins") && ok;
        ok = expectPixel(frame, frame.px(columnX[1]), frame.py(0.0f), farColor, "test-off-last-draw-wins") && ok;
        ok = expectPixel(frame, frame.px(columnX[2]), frame.py(0.0f), farColor, "test-on-write-off-far-wins") && ok;
        ok = expectPixel(frame, frame.px(columnX[3] - pairShift), frame.py(-pairShift), equalColor, "equal-same-depth-passes") && ok;
        ok = expectPixel(frame, frame.px(columnX[3] + pairShift + (quadHalf * 0.5f)), frame.py(pairShift + (quadHalf * 0.5f)), {clearColor[0], clearColor[1], clearColor[2]}, "equal-other-depth-rejected") && ok;
        // Outside the overlap each quad is visible on its own.
        ok = expectPixel(frame, frame.px(columnX[0] - pairShift - quadHalf * 0.5f), frame.py(-pairShift - quadHalf * 0.5f), nearColor, "near-alone") && ok;
        ok = expectPixel(frame, frame.px(columnX[0] + pairShift + quadHalf * 0.5f), frame.py(pairShift + quadHalf * 0.5f), farColor, "far-alone") && ok;
        return ok;
    }

    auto teardown() -> void override {
        device().destroyTexture(depthTexture);
        device().destroyBuffer(indexBuffer);
        device().destroyBuffer(vertexBuffer);
        for (auto* pipeline : pipelines) {
            device().destroyPipeline(pipeline);
        }
        device().destroyShaderModule(fragmentShader);
        device().destroyShaderModule(vertexShader);
    }

private:
    static auto depthFormatName(RhiFormat format) -> const char* {
        switch (format) {
            case RhiFormat::D32_SFLOAT:
                return "D32_SFLOAT";
            case RhiFormat::D24_UNORM_S8_UINT:
                return "D24_UNORM_S8_UINT";
            case RhiFormat::D32_SFLOAT_S8_UINT:
                return "D32_SFLOAT_S8_UINT";
            default:
                return "?";
        }
    }

    auto createDepthTexture(RhiExtent2D extent) -> void {
        RhiTextureDesc desc = {
            .width = extent.width,
            .height = extent.height,
            .format = depthFormat,
            .usage = RhiTextureUsage::DepthAttachment,
        };
        depthTexture = device().createTexture(desc);
    }

    RhiFormat requestedFormat = RhiFormat::D32_SFLOAT;
    bool wantStencil = false;
    RhiFormat depthFormat = RhiFormat::D32_SFLOAT;
    RhiShaderModule* vertexShader = nullptr;
    RhiShaderModule* fragmentShader = nullptr;
    std::array<RhiPipeline*, 4> pipelines = {};
    RhiBuffer* vertexBuffer = nullptr;
    RhiBuffer* indexBuffer = nullptr;
    RhiTexture* depthTexture = nullptr;
};

auto main(int argc, char** argv) -> int {
    DepthExample example;
    return example.run(argc, argv, "depth");
}
