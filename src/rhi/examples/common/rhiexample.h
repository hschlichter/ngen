#pragma once

// RhiExample: the part of every RHI example that is the same. Header-only.
//
// This file is the reference implementation of the integrator contract in
// src/rhi/README.md ("What an integrator provides"). Read it top to bottom,
// then an example (triangle.cpp) for the per-program part: shaders, pipeline,
// draw, checks. The RHI executes; this class schedules.
//
// Sections, in run() order:
//   1. options        flags shared by every example
//   2. window         SDL, the window layer; the RHI never sees it
//   3. device         backend chosen here, validation is a runtime option
//   4. swapchain      size passed in, never queried by the backend
//   5. frame pacing   slot-owned command buffer, fence, image-available semaphore;
//                     image-owned render-finished semaphore
//   6. frame loop     resize, acquire, record, submit, present, optional readback
//   7. checks         analytic pixel assertions and screenshot on the last frame
//   8. shutdown       waitIdle, then destroy in reverse order
//
// Usage: derive, override setup / record / check / teardown, call run().
//
// Flags every example accepts:
//   --frames=N        render N frames then exit (0 = until the window closes)
//   --size=WxH        window size in pixels, default 1280x720
//   --check           read the last frame back and run check()
//   --screenshot=PATH read the last frame back and write it as PNG
//   --resize-at=N     resize the window at frame N to exercise swapchain recreation
//   --validation      enable the backend validation layer; any error fails the run
// Exit codes: 0 ok, 1 setup failed, 2 a check or validation failed.

#include "pngwrite.h"
#include "readback.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "rhidevicevulkan.h"
#include "rhiswapchain.h"
#include "rhitypes.h"
#include "windowsdl.h"

#include <SDL3/SDL.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>
#include <vector>

struct RhiExampleOptions {
    uint64_t frames = 0;
    uint32_t width = 1280;
    uint32_t height = 720;
    bool check = false;
    std::string screenshot;
    uint64_t resizeAt = 0;
    bool validation = false;
};

// Last frame as tightly packed RGBA8, bytes as the swapchain stored them.
struct RhiExampleFrame {
    std::vector<uint8_t> rgba;
    RhiExtent2D extent = {};
    RhiFormat format = RhiFormat::Undefined;
    RhiCommandStats stats; // counters after record(), before the base's readback copy

    // NDC [-1,1] to pixel index; y is down in both.
    auto px(float ndcX) const -> uint32_t { return (uint32_t) ((ndcX + 1.0f) * 0.5f * (float) extent.width); }
    auto py(float ndcY) const -> uint32_t { return (uint32_t) ((ndcY + 1.0f) * 0.5f * (float) extent.height); }
};

class RhiExample {
public:
    virtual ~RhiExample() = default;

    auto run(int argc, char** argv, const char* name) -> int;

protected:
    // Create shaders, pipelines, buffers. Return false to exit 1.
    virtual auto setup() -> bool = 0;
    // Record one frame. backbuffer is in ColorAttachment layout on entry and must be on exit.
    virtual auto record(RhiCommandBuffer* cmd, RhiTexture* backbuffer, RhiExtent2D extent) -> void = 0;
    // Assert pixel values of the last frame. Return false to exit 2. Only called with --check.
    virtual auto check(const RhiExampleFrame& frame) -> bool { return true; }
    // Destroy what setup created. GPU is idle when called.
    virtual auto teardown() -> void = 0;
    // Example-specific flag. Return true if consumed.
    virtual auto parseArg(std::string_view arg) -> bool { return false; }
    // Swapchain was recreated at this size; GPU is idle. Recreate size-dependent resources here.
    virtual auto resized(RhiExtent2D extent) -> void {}
    // The slot's fence has signalled and its command buffer has not been reset yet: read back
    // anything the slot's previous frame produced (queries, GPU zones) here.
    virtual auto slotReady(uint32_t slot, RhiCommandBuffer* cmd) -> void {}

    auto device() -> RhiDevice& { return rhiDevice; }
    auto swapchain() -> RhiSwapchain* { return rhiSwapchain; }
    auto colorFormat() const -> RhiFormat { return swapchainFormat; }
    auto frameCount() const -> uint32_t { return slotCount; }
    auto options() const -> const RhiExampleOptions& { return opts; }
    // Valid inside record(): which slot's resources this frame owns, and the frame number (0-based).
    auto frameSlot() const -> uint32_t { return currentSlot; }
    auto frameIndex() const -> uint64_t { return currentFrame; }

    // One pixel assertion; linear colour is encoded to what the swapchain stores.
    auto expectPixel(const RhiExampleFrame& frame, uint32_t x, uint32_t y, std::array<float, 3> linearRgb, const char* label) -> bool {
        return ::expectPixel(frame.rgba, frame.extent, x, y, expectedBytes(frame.format, linearRgb), pixelTolerance, label);
    }

    // Exact count check, for RhiCommandStats and other integers the example knows in advance.
    auto expectCount(const char* label, uint64_t actual, uint64_t expected) -> bool {
        bool ok = actual == expected;
        std::println("check {}: {} expected {} {}", label, actual, expected, ok ? "ok" : "FAIL");
        return ok;
    }

    // Clear colour applied by the base before record() is called.
    std::array<float, 4> clearColor = {0.08f, 0.08f, 0.10f, 1.0f};
    int pixelTolerance = 3;

private:
    static auto parseUnsigned(std::string_view text) -> uint64_t {
        uint64_t value = 0;
        std::from_chars(text.data(), text.data() + text.size(), value);
        return value;
    }

    auto parseOptions(int argc, char** argv) -> void;
    auto windowExtent() const -> RhiExtent2D;
    auto recreateSwapchain() -> void;

    RhiExampleOptions opts;
    SDL_Window* window = nullptr;
    RhiDeviceVulkan rhiDevice;
    RhiSwapchain* rhiSwapchain = nullptr;
    RhiFormat swapchainFormat = RhiFormat::Undefined;
    uint32_t slotCount = 0;
    std::vector<RhiCommandBuffer*> commandBuffers;
    std::vector<RhiFence*> inFlightFences;
    std::vector<RhiSemaphore*> imageAvailable;
    std::vector<RhiSemaphore*> renderFinished;
    bool resizePending = false;
    uint32_t currentSlot = 0;
    uint64_t currentFrame = 0;
};

inline auto RhiExample::parseOptions(int argc, char** argv) -> void {
    for (int i = 1; i < argc; i++) {
        std::string_view arg = argv[i];
        if (arg.starts_with("--frames=")) {
            opts.frames = parseUnsigned(arg.substr(9));
        } else if (arg.starts_with("--size=")) {
            auto spec = arg.substr(7);
            auto x = spec.find('x');
            if (x != std::string_view::npos) {
                opts.width = (uint32_t) parseUnsigned(spec.substr(0, x));
                opts.height = (uint32_t) parseUnsigned(spec.substr(x + 1));
            }
        } else if (arg == "--check") {
            opts.check = true;
        } else if (arg.starts_with("--screenshot=")) {
            opts.screenshot = std::string(arg.substr(13));
        } else if (arg.starts_with("--resize-at=")) {
            opts.resizeAt = parseUnsigned(arg.substr(12));
        } else if (arg == "--validation") {
            opts.validation = true;
        } else if (!parseArg(arg)) {
            std::println(stderr, "unknown argument: {}", arg);
        }
    }
}

inline auto RhiExample::windowExtent() const -> RhiExtent2D {
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    return {.width = (uint32_t) w, .height = (uint32_t) h};
}

// Window layers do not all report OutOfDate on resize (Wayland does not), so the
// size-changed event is the primary trigger and OutOfDate / Suboptimal from the
// backend is the fallback. Both set resizePending; this drains it.
inline auto RhiExample::recreateSwapchain() -> void {
    resizePending = false;
    auto extent = windowExtent();
    if (extent.width == 0 || extent.height == 0) {
        return; // minimized; keep the old swapchain until we have a size
    }
    auto current = rhiSwapchain->extent();
    if (extent.width == current.width && extent.height == current.height) {
        return; // size event without a size change (window shown, moved); nothing to do
    }
    rhiDevice.waitIdle(); // recreate tears down images the in-flight frames may use
    if (!rhiSwapchain->recreate(extent)) {
        std::println(stderr, "swapchain recreate failed");
        return;
    }
    resized(rhiSwapchain->extent());
}

inline auto RhiExample::run(int argc, char** argv, const char* name) -> int {
    // 1. Options.
    parseOptions(argc, argv);
    bool wantReadback = opts.check || !opts.screenshot.empty();
    if (wantReadback && opts.frames == 0) {
        opts.frames = 1;
    }

    // 2. Window. SDL is this program's window layer; the RHI never includes it.
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::println(stderr, "SDL_Init failed: {}", SDL_GetError());
        return 1;
    }
    std::string title = std::string("ngen RHI example: ") + name;
    window = SDL_CreateWindow(title.c_str(), (int) opts.width, (int) opts.height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        std::println(stderr, "SDL_CreateWindow failed: {}", SDL_GetError());
        return 1;
    }

    // 3. Device. The application picks the backend and hands the window layer's
    //    contract (RhiWindow) to it. Validation is a runtime option.
    if (!rhiDevice.init(makeRhiWindowSdl(window), {.enableValidation = opts.validation})) {
        std::println(stderr, "RhiDevice init failed");
        return 1;
    }

    // 4. Swapchain. Size comes from the caller; the backend never asks the window.
    rhiSwapchain = rhiDevice.createSwapchain(windowExtent());
    if (rhiSwapchain == nullptr) {
        std::println(stderr, "createSwapchain failed");
        return 1;
    }
    swapchainFormat = rhiSwapchain->colorFormat();

    // 5. Frame pacing. The RHI has no frame concept; this program decides how many
    //    frames are in flight. Command buffers, fences and the image-available
    //    semaphore belong to the frame slot. The render-finished semaphore belongs
    //    to the swapchain image, because present consumes it per image.
    slotCount = rhiSwapchain->imageCount();
    commandBuffers.resize(slotCount);
    inFlightFences.resize(slotCount);
    imageAvailable.resize(slotCount);
    renderFinished.resize(slotCount);
    for (uint32_t i = 0; i < slotCount; i++) {
        commandBuffers[i] = rhiDevice.createCommandBuffer();
        inFlightFences[i] = rhiDevice.createFence(true);
        imageAvailable[i] = rhiDevice.createSemaphore();
        renderFinished[i] = rhiDevice.createSemaphore();
    }

    // Example resources: shaders, pipelines, buffers.
    if (!setup()) {
        std::println(stderr, "{}: setup failed", name);
        return 1;
    }

    // Readback target for --check / --screenshot, filled on the last frame and
    // sized for the swapchain at that moment.
    RhiBuffer* readbackBuffer = nullptr;
    RhiExampleFrame lastFrame;

    // 6. Frame loop.
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
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                resizePending = true;
            }
        }
        if (!running) {
            break;
        }
        if (opts.resizeAt > 0 && renderedFrames == opts.resizeAt) {
            std::println("resize requested at frame {}", renderedFrames);
            SDL_SetWindowSize(window, (int) opts.width / 2, (int) opts.height / 2);
            opts.resizeAt = 0;
        }
        if (resizePending) {
            recreateSwapchain();
        }

        // Wait until this slot's previous frame is done before reusing its command buffer.
        rhiDevice.waitForFence(inFlightFences[slot]);
        slotReady(slot, commandBuffers[slot]);

        auto acquired = rhiSwapchain->acquireNextImage(imageAvailable[slot]);
        if (!acquired) {
            if (acquired.error() == RhiError::OutOfDate) {
                resizePending = true;
                continue;
            }
            std::println(stderr, "acquireNextImage failed");
            break;
        }
        auto imageIndex = *acquired;
        rhiDevice.resetFence(inFlightFences[slot]);

        auto* cmd = commandBuffers[slot];
        auto* backbuffer = rhiSwapchain->image(imageIndex);
        auto extent = rhiSwapchain->extent();
        bool isLastFrame = opts.frames > 0 && renderedFrames + 1 == opts.frames;
        bool readbackThisFrame = wantReadback && isLastFrame;

        currentSlot = slot;
        currentFrame = renderedFrames;

        cmd->reset();
        cmd->begin();
        cmd->beginLabel(name);

        // Layouts are explicit. The swapchain image starts undefined each frame and
        // must end in PresentSrc. The example records between those two barriers.
        std::array<RhiTextureBarrierDesc, 1> toColor = {{
            {.texture = backbuffer, .oldState = RhiTextureState::Undefined, .newState = RhiTextureState::ColorAttachment},
        }};
        cmd->pipelineBarrier(toColor);

        record(cmd, backbuffer, extent);

        if (readbackThisFrame) {
            lastFrame.extent = extent;
            lastFrame.format = swapchainFormat;
            lastFrame.stats = cmd->stats();
            readbackBuffer = createReadbackBuffer(rhiDevice, extent);
            recordReadback(cmd, backbuffer, readbackBuffer, extent, RhiTextureState::ColorAttachment, RhiTextureState::PresentSrc);
        } else {
            std::array<RhiTextureBarrierDesc, 1> toPresent = {{
                {.texture = backbuffer, .oldState = RhiTextureState::ColorAttachment, .newState = RhiTextureState::PresentSrc},
            }};
            cmd->pipelineBarrier(toPresent);
        }

        cmd->endLabel();
        cmd->end();

        rhiDevice.submitCommandBuffer(cmd, {
                                               .waitSemaphore = imageAvailable[slot],
                                               .signalSemaphore = renderFinished[imageIndex],
                                               .fence = inFlightFences[slot],
                                           });

        auto presented = rhiDevice.present(rhiSwapchain, renderFinished[imageIndex], imageIndex);
        if (!presented) {
            if (presented.error() == RhiError::OutOfDate || presented.error() == RhiError::Suboptimal) {
                resizePending = true;
            } else {
                std::println(stderr, "present failed");
                break;
            }
        }

        if (readbackThisFrame) {
            // The copy is in this frame's submit; wait on its fence, then map.
            rhiDevice.waitForFence(inFlightFences[slot]);
            lastFrame.rgba = resolveReadback(rhiDevice, readbackBuffer, lastFrame.extent, lastFrame.format);
        }

        slot = (slot + 1) % slotCount;
        renderedFrames++;
        if (isLastFrame) {
            running = false;
        }
    }

    // 7. Checks. Expected values come from the example's own constants, never
    //    from a stored image.
    int exitCode = 0;
    if (opts.check && !lastFrame.rgba.empty()) {
        if (!check(lastFrame)) {
            exitCode = 2;
        }
    }
    if (!opts.screenshot.empty() && !lastFrame.rgba.empty()) {
        if (!writePng(opts.screenshot.c_str(), lastFrame.rgba, lastFrame.extent.width, lastFrame.extent.height)) {
            exitCode = 2;
        }
    }
    auto validationErrors = rhiDevice.validationErrorCount();
    if (opts.validation && validationErrors > 0) {
        std::println(stderr, "validation errors: {}", validationErrors);
        exitCode = 2;
    }

    // 8. Shutdown. Nothing may be destroyed while the GPU can still touch it;
    //    waitIdle is the right tool here and only here (and in recreate).
    rhiDevice.waitIdle();
    teardown();
    if (readbackBuffer != nullptr) {
        rhiDevice.destroyBuffer(readbackBuffer);
    }
    for (uint32_t i = 0; i < slotCount; i++) {
        rhiDevice.destroySemaphore(renderFinished[i]);
        rhiDevice.destroySemaphore(imageAvailable[i]);
        rhiDevice.destroyFence(inFlightFences[i]);
        rhiDevice.destroyCommandBuffer(commandBuffers[i]);
    }
    rhiSwapchain->destroy();
    delete rhiSwapchain;
    rhiSwapchain = nullptr;
    rhiDevice.destroy();

    SDL_DestroyWindow(window);
    SDL_Quit();

    std::println("{}: {} frames={} validation_errors={}", name, exitCode == 0 ? "ok" : "FAILED", renderedFrames, validationErrors);
    return exitCode;
}
