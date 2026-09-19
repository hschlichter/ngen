// Minimal RHI example: one window, one swapchain, one pipeline, one triangle.
//
// This file is the reference implementation of the integrator contract in
// src/rhi/README.md ("What an integrator provides"). Read it top to bottom; each
// section names the rule it satisfies. Everything included here lives under
// src/rhi/. The RHI executes; this program schedules.
//
// Run:  ngen-example-triangle [--frames=N]
//       N > 0 renders N frames and exits 0 (for unattended runs under
//       SDL_VIDEODRIVER=offscreen with validation layers).

#include "common/shadercompile.h"
#include "common/windowsdl.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "rhidevicevulkan.h"
#include "rhiswapchain.h"

#include <SDL3/SDL.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <print>
#include <string_view>
#include <vector>

// Shaders live next to the pipeline that uses them. Compiled at startup with
// shaderc; the engine compiles offline, examples trade a runtime dependency for
// readability.
static constexpr const char* vertexShaderSource = R"glsl(
#version 450

layout(location = 0) out vec3 fragColor;

const vec2 positions[3] = vec2[](
    vec2( 0.0, -0.6),
    vec2( 0.6,  0.6),
    vec2(-0.6,  0.6)
);

const vec3 colors[3] = vec3[](
    vec3(1.0, 0.2, 0.2),
    vec3(0.2, 1.0, 0.2),
    vec3(0.2, 0.2, 1.0)
);

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragColor = colors[gl_VertexIndex];
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

static auto parseFrames(int argc, char** argv) -> uint64_t {
    for (int i = 1; i < argc; i++) {
        std::string_view arg = argv[i];
        constexpr std::string_view prefix = "--frames=";
        if (arg.starts_with(prefix)) {
            uint64_t value = 0;
            auto digits = arg.substr(prefix.size());
            std::from_chars(digits.data(), digits.data() + digits.size(), value);
            return value;
        }
    }
    return 0;
}

static auto windowExtent(SDL_Window* window) -> RhiExtent2D {
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    return {.width = (uint32_t) w, .height = (uint32_t) h};
}

auto main(int argc, char** argv) -> int {
    auto maxFrames = parseFrames(argc, argv);

    // 1. Window. SDL is this program's window layer; the RHI never includes it.
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::println(stderr, "SDL_Init failed: {}", SDL_GetError());
        return 1;
    }
    auto* window = SDL_CreateWindow("ngen RHI example: triangle", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        std::println(stderr, "SDL_CreateWindow failed: {}", SDL_GetError());
        return 1;
    }

    // 2. Device. The application picks the backend and hands the window layer's
    //    contract (RhiWindow) to it.
    RhiDeviceVulkan device;
    if (!device.init(makeRhiWindowSdl(window))) {
        std::println(stderr, "RhiDevice init failed");
        return 1;
    }

    // 3. Swapchain. Size comes from the caller; the backend never asks the window.
    auto* swapchain = device.createSwapchain(windowExtent(window));
    if (swapchain == nullptr) {
        std::println(stderr, "createSwapchain failed");
        return 1;
    }

    // 4. Shaders. Bytecode in; the RHI does no file IO and no compilation.
    auto vertexSpirv = compileGlsl(RhiShaderStage::Vertex, vertexShaderSource, "triangle.vert");
    auto fragmentSpirv = compileGlsl(RhiShaderStage::Fragment, fragmentShaderSource, "triangle.frag");
    if (vertexSpirv.empty() || fragmentSpirv.empty()) {
        return 1;
    }
    auto* vertexShader = device.createShaderModule({.stage = RhiShaderStage::Vertex, .code = vertexSpirv});
    auto* fragmentShader = device.createShaderModule({.stage = RhiShaderStage::Fragment, .code = fragmentSpirv});

    // 5. Pipeline. No vertex buffer, no descriptors, no depth. Viewport and scissor
    //    are dynamic so a resize does not rebuild the pipeline.
    auto colorFormat = swapchain->colorFormat();
    RhiGraphicsPipelineDesc pipelineDesc = {
        .vertexShader = vertexShader,
        .fragmentShader = fragmentShader,
        .colorFormats = {&colorFormat, 1},
        .raster = {.cullMode = RhiCullMode::None},
        .depth = {.testEnable = false, .writeEnable = false},
    };
    auto* pipeline = device.createGraphicsPipeline(pipelineDesc);
    if (pipeline == nullptr) {
        std::println(stderr, "createGraphicsPipeline failed");
        return 1;
    }

    // 6. Frame pacing. The RHI has no frame concept; this program decides how many
    //    frames are in flight. Command buffers, fences and the image-available
    //    semaphore belong to the frame slot. The render-finished semaphore belongs
    //    to the swapchain image, because present consumes it per image.
    auto frameCount = swapchain->imageCount();
    std::vector<RhiCommandBuffer*> commandBuffers(frameCount);
    std::vector<RhiFence*> inFlightFences(frameCount);
    std::vector<RhiSemaphore*> imageAvailable(frameCount);
    std::vector<RhiSemaphore*> renderFinished(frameCount);
    for (uint32_t i = 0; i < frameCount; i++) {
        commandBuffers[i] = device.createCommandBuffer();
        inFlightFences[i] = device.createFence(true);
        imageAvailable[i] = device.createSemaphore();
        renderFinished[i] = device.createSemaphore();
    }

    auto recreateSwapchain = [&] -> void {
        auto extent = windowExtent(window);
        if (extent.width == 0 || extent.height == 0) {
            return; // minimized; keep the old swapchain until we have a size
        }
        if (!swapchain->recreate(extent)) {
            std::println(stderr, "swapchain recreate failed");
        }
    };

    // 7. Frame loop.
    uint32_t slot = 0;
    uint64_t renderedFrames = 0;
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            }
        }
        if (!running) {
            break;
        }

        // Wait until this slot's previous frame is done before reusing its command buffer.
        device.waitForFence(inFlightFences[slot]);

        auto acquired = swapchain->acquireNextImage(imageAvailable[slot]);
        if (!acquired) {
            if (acquired.error() == RhiError::OutOfDate) {
                recreateSwapchain();
                continue;
            }
            std::println(stderr, "acquireNextImage failed");
            break;
        }
        auto imageIndex = *acquired;
        device.resetFence(inFlightFences[slot]);

        auto* cmd = commandBuffers[slot];
        auto* backbuffer = swapchain->image(imageIndex);
        auto extent = swapchain->extent();

        cmd->reset();
        cmd->begin();
        cmd->beginLabel("triangle");

        // Layouts are explicit. The swapchain image starts undefined each frame and
        // must end in PresentSrc.
        std::array<RhiBarrierDesc, 1> toColor = {{
            {.texture = backbuffer, .oldLayout = RhiImageLayout::Undefined, .newLayout = RhiImageLayout::ColorAttachment},
        }};
        cmd->pipelineBarrier(toColor);

        std::array<RhiRenderingAttachmentInfo, 1> colorAttachments = {{
            {
                .texture = backbuffer,
                .layout = RhiImageLayout::ColorAttachment,
                .clear = true,
                .clearColor = {0.08f, 0.08f, 0.10f, 1.0f},
            },
        }};
        cmd->beginRendering({.extent = extent, .colorAttachments = colorAttachments});
        cmd->setViewport(extent);
        cmd->setScissor(extent);
        cmd->bindPipeline(pipeline);
        cmd->draw(3, 1, 0, 0);
        cmd->endRendering();

        std::array<RhiBarrierDesc, 1> toPresent = {{
            {.texture = backbuffer, .oldLayout = RhiImageLayout::ColorAttachment, .newLayout = RhiImageLayout::PresentSrc},
        }};
        cmd->pipelineBarrier(toPresent);

        cmd->endLabel();
        cmd->end();

        device.submitCommandBuffer(cmd, {
                                            .waitSemaphore = imageAvailable[slot],
                                            .signalSemaphore = renderFinished[imageIndex],
                                            .fence = inFlightFences[slot],
                                        });

        auto presented = device.present(swapchain, renderFinished[imageIndex], imageIndex);
        if (!presented) {
            if (presented.error() == RhiError::OutOfDate || presented.error() == RhiError::Suboptimal) {
                recreateSwapchain();
            } else {
                std::println(stderr, "present failed");
                break;
            }
        }

        slot = (slot + 1) % frameCount;
        renderedFrames++;
        if (maxFrames > 0 && renderedFrames >= maxFrames) {
            running = false;
        }
    }

    // 8. Shutdown. Nothing may be destroyed while the GPU can still touch it;
    //    waitIdle is the right tool here and only here.
    device.waitIdle();
    for (uint32_t i = 0; i < frameCount; i++) {
        device.destroySemaphore(renderFinished[i]);
        device.destroySemaphore(imageAvailable[i]);
        device.destroyFence(inFlightFences[i]);
        device.destroyCommandBuffer(commandBuffers[i]);
    }
    device.destroyPipeline(pipeline);
    device.destroyShaderModule(fragmentShader);
    device.destroyShaderModule(vertexShader);
    swapchain->destroy();
    delete swapchain;
    device.destroy();

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
