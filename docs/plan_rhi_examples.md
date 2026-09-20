# RHI examples (`src/rhi/examples/`)

**Status. Landed.**

## Current state

The RHI (`src/rhi/`, principles in `src/rhi/README.md`) has one consumer: the renderer inside `ngen-view`. Exercising an RHI change means running
the whole engine. There is no small program that shows the minimum an integrator must write, and no fast way to check a backend change in isolation.

An earlier attempt at RHI testing (`docs/plan_rhi_validation.md`, reverted in `ece9f44`) built a registry, runner, obs reporting and readback before
drawing anything. That is superseded by this plan: examples first, minimal, human-visible. Tests, if ever, grow out of an example that already works.

## Scope

**In**

- `src/rhi/examples/triangle.cpp`: one window, one swapchain, one pipeline, one triangle, resize handling, clean shutdown. Standalone, readable
  top to bottom as the integrator contract from the README made concrete.
- `src/rhi/examples/common/`: header-only SDL window glue (`RhiWindow` from an `SDL_Window*`) and shaderc wrapper (GLSL string to SPIR-V
  bytes). Each example is one translation unit. Examples include nothing outside `src/rhi/`.
- Shaders embedded as GLSL raw string literals in the example, compiled at startup with shaderc. The whole example, shaders included, is one file.
- Build target `ngen-example-triangle`, not the default target.
- Flags that make the example a verification tool for humans and agents: `--frames=N`, `--size=WxH`, `--check` (analytic pixel
  assertions from constants in the file), `--screenshot=PATH` (PNG via stb), `--resize-at=N`, `--validation`. Exit codes 0/1/2 and a
  one-line summary. Documented in `src/rhi/README.md` "Examples".
- RHI additions the above needs: `RhiCommandBuffer::copyTextureToBuffer` (readback is a command, same rule as uploads) and
  `RhiDeviceOptions { enableValidation }` on `RhiDevice::init` with `validationErrorCount()`, so validation is a runtime result instead
  of a compile-time define plus log grep. `ngen-view` gains `--validation`; `NGEN_ENABLE_VALIDATION` is gone.

**Out**

- Test harness, registry, pass/fail reporting, obs bus. None of it.
- Backend selection machinery. Examples construct `RhiDeviceVulkan` directly, like `main.cpp`. When a second backend exists, the build selects
  sources per platform; the example body does not change.

## Decisions

- **Examples use the backend directly.** `triangle.cpp` includes `rhidevicevulkan.h` and constructs `RhiDeviceVulkan` on the stack. No factory,
  no `#ifdef` chain; the build already links exactly one backend per platform. Mirrors `main.cpp`.
- **Examples reach only into `src/rhi/`.** Include paths for the target are `src/rhi`, `src/rhi/vulkan`, `src/rhi/examples`. Anything an example
  needs that is not RHI lives in `src/rhi/examples/common/`. This is why the SDL glue is duplicated rather than shared with `main.cpp`: the
  alternative is an example depending on `src/`, or the engine depending on example code.
- **SDL is the window layer for examples.** Same rule as the engine: SDL never enters `src/rhi/` proper (interface or backend). `examples/` is
  application code that happens to live next to the interface it demonstrates.
- **`RhiExample` base, header-only.** Pulled forward from "after the second example" once the flag set grew: window, device, swapchain,
  pacing, loop, readback, flags and exit code are identical for every example and were 300 lines of the 400. The base is one readable file
  with the integrator contract as numbered sections; an example overrides `setup`, `record`, `check`, `teardown` and is about 120 lines.
  Guard against bloat: only what every example needs enters the base. Feature helpers (depth, uploads, descriptors) start next to the
  example that needs them.
- **Checks are analytic.** Expected pixels come from constants in the example (clear colour, barycentric interpolation at the window centre),
  encoded per the swapchain format (sRGB, channel order). No golden images. Tolerance of 3 covers interpolation rounding.
- **Shaders are compiled at runtime from embedded GLSL.** The point of an example is to read it; shader code belongs in the file next to the
  pipeline that uses it. `libshaderc_shared` ships in the same package as `glslc`, which the build already requires, so the dependency is already
  present wherever ngen builds. Linked into example targets only; the engine keeps offline `.spv`. A D3D12 example would embed HLSL and call
  `dxcompiler` the same way. Rejected: SPIR-V array with GLSL in a comment (drifts silently); glslang submodule (large build for one example).

## Steps

1. **SDL glue.** `src/rhi/examples/common/windowsdl.h`, header-only:

   ```cpp
   #include "rhiwindow.h"
   inline auto makeRhiWindowSdl(SDL_Window* window) -> RhiWindow;
   ```

   Body is the function currently static in `src/main.cpp` (extensions from `SDL_Vulkan_GetInstanceExtensions`, surface via
   `SDL_Vulkan_CreateSurface`). `main.cpp` keeps its copy.

2. **Shader compiler wrapper.** `src/rhi/examples/common/shadercompile.h`, header-only:

   ```cpp
   #include "rhitypes.h"
   #include <cstddef>
   #include <string_view>
   #include <vector>

   // GLSL source to SPIR-V via shaderc. Prints the compiler log and returns empty on error.
   inline auto compileGlsl(RhiShaderStage stage, std::string_view source, const char* name) -> std::vector<std::byte>;
   ```

   Body: `shaderc_compiler_initialize`, options with `shaderc_env_version_vulkan_1_3`, `shaderc_compile_into_spv`, status check,
   copy bytes, release. About 30 lines.

3. **`common/rhiexample.h` and `triangle.cpp`.** The base carries these sections in order, each with a comment naming the README principle
   it satisfies; the example supplies shaders, pipeline, draw and checks:

   1. Parse `--frames=N` (default 0 = run until quit).
   2. `SDL_Init`, create window with `SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE`.
   3. `RhiDeviceVulkan device; device.init(makeRhiWindowSdl(window))`.
   4. `createSwapchain(extent)` from `SDL_GetWindowSizeInPixels`.
   5. Two `constexpr const char*` GLSL strings at file top: vertex shader with three hardcoded positions and colors from `gl_VertexIndex`, no
      vertex buffer; fragment shader passing the color through. `compileGlsl` each, `createShaderModule` with the bytes.
   6. `createGraphicsPipeline` with empty `descriptorSetLayouts`, `vertexStride = 0`, no attributes, `colorFormats = {swapchain color}`,
      `depthFormat = Undefined`, `raster.cullMode = None`.
   7. Per-frame-slot sync: `imageCount` command buffers and fences, `imageAvailable` semaphores per slot, `renderFinished` semaphores per
      swapchain image. Comment states the slot-vs-image rule from the README.
   8. Loop: poll SDL events (quit, resize noted); `waitForFence`, `resetFence`; `acquireNextImage`; on `OutOfDate` recreate with current
      window size and `continue`; record: barrier `Undefined -> ColorAttachment`, `beginRendering` with clear, `setViewport`/`setScissor`,
      `bindPipeline`, `draw(3, 1, 0, 0)`, `endRendering`, barrier `ColorAttachment -> PresentSrc`; `submitCommandBuffer`; `present`; on
      `OutOfDate` or `Suboptimal` recreate. Advance slot. Exit when frame count reached.
   9. Shutdown: `waitIdle`, destroy in reverse order, `swapchain->destroy()`, `device.destroy()`, SDL teardown.

4. **Build.** In `build.cpp`, after the `view` program:

   ```cpp
   auto exampleTriangle =
       cxx::program("ngen-example-triangle")
           .sources({"src/rhi/examples/triangle.cpp"})
           .include({
               "src/rhi",
               "src/rhi/vulkan",
               "src/rhi/examples",
           })
           .link(rhi_backend)
           .link("shaderc_shared")
           .link_flags(sdl3_libs);

   p.target(exampleTriangle);
   ```

   `ngen-view` stays the default target. No shader tool, no runtime file dependencies: the binary runs from any working directory.

5. **Readback and validation.** `copyTextureToBuffer` on `RhiCommandBuffer` (Vulkan: `vkCmdCopyImageToBuffer`, source in `TransferSrc`).
   `RhiDeviceOptions` on `init`; Vulkan checks `VK_LAYER_KHRONOS_validation` exists, installs a `VK_EXT_debug_utils` messenger that counts
   errors and warnings and prints them. `examples/common/readback.h` (create buffer, record barriers plus copy, resolve to RGBA8 with
   BGRA swizzle, sRGB encode helper, `expectPixel`) and `examples/common/pngwrite.h` (stb).

6. **README.** "Examples" section: build, unattended run, flags, exit codes, rules. Pointer under "What an integrator provides".

## Verification

- `./_out/ngen-build -p linux-vulkan -c debug ngen-example-triangle` builds. `ngen-view` build and headless three_cubes run unchanged.
- `SDL_VIDEODRIVER=offscreen ./_out/linux-vulkan/debug/ngen-example-triangle --frames=60 --check --validation` exits 0; both checks print
  `ok`; summary line reports `validation_errors=0`.
- `--resize-at=20 --frames=40 --check` logs one new `Swapchain extent` at half size and still exits 0 with checks passing at the new centre.
- `--screenshot=PATH` writes a PNG showing the red/green/blue triangle on dark grey.
- Negative check: change `centerColor` in the source, run `--check`: exit 2, `triangle-center` line says `FAIL`.
- Negative validation: comment out the `Undefined -> ColorAttachment` barrier, run `--validation`: exit 2, `validation_errors` nonzero.
- Henrik runs it windowed: colored triangle on a clear color, resize keeps rendering without a crash, close exits cleanly.
- `rg -n '#include "' src/rhi/examples/triangle.cpp` shows only headers under `src/rhi/`.
- Negative check: break the GLSL (remove a semicolon), run: exits nonzero with the shaderc log on stderr naming the line.

## Deferred / follow-ups

- **Second example** (textured quad: buffer upload, texture upload, sampler, descriptor set). Trigger: triangle lands. Decides whether a
  `common/` frame loop is worth extracting.
- **Readback.** `copyTextureToBuffer` on `RhiCommandBuffer` plus `--screenshot=path` in examples. Trigger: an example where eyeballing is not
  enough, or a regression that a pixel check would have caught.
- **Per-platform example sources** when a second backend exists: `select("platform", ...)` on the source list, same shape as `rhi-backend`.
- **Depth example** once `depthImage` removal from the swapchain (done) needs demonstrating: renderer-owned depth texture, recreate on resize.
