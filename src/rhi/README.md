# RHI

`src/rhi/` is the render hardware interface: a thin, header-only abstraction over one graphics API at a time.
`RhiDevice`, `RhiCommandBuffer` and `RhiSwapchain` are abstract classes; each backend lives in a sibling folder
(`vulkan/`, later `d3d12/`, `metal/`) and is the only code that includes that API's headers. The build links exactly
one backend per platform.

This file states the principles the interface is built on and what an integrator (an engine, an app, a test harness)
is expected to provide. Read it before adding to the interface or writing code on top of it.

## Principles

### The RHI executes; the engine schedules

The RHI does what it is told, when it is told, and nothing more. It never waits, batches, defers or caches on the
caller's behalf. Every operation maps to a small number of backend calls with no hidden synchronization.

Concretely:

- `destroyX()` destroys now. If a frame in flight still references the resource, that is the caller's bug. The engine
  knows its frame pacing; the RHI does not. ngen keeps a `DeletionQueue` in `src/renderer/` that tags each pending
  destroy with a frame number and drains it once that frame's fence has signaled.
- Uploads are commands. `RhiCommandBuffer::copyBuffer` and `copyBufferToTexture` record into a command buffer the
  caller owns. Staging buffers, layout barriers, submission and the fence wait are the caller's. `RhiTextureDesc` has
  no `initialData`; `RhiDevice` has no `copyBuffer`. ngen's `GpuUploader` in `src/renderer/` shows the expected shape:
  one command buffer, N copies, one submit, one fence wait, then free staging.
- `waitIdle()` exists for shutdown and swapchain recreation. It is not a synchronization primitive for normal frames.
- No frame concept. The RHI does not know what a frame is; fences and semaphores are the only sync objects, and the
  engine decides how many are in flight.

Why: hidden waits were the first thing removed from this interface. `copyBuffer` used to `vkQueueWaitIdle` per call,
so uploading a scene stalled the GPU once per buffer. Making the cost visible at the call site keeps it honest.

### No window layer in the RHI

Nothing under `src/rhi/` includes SDL, GLFW, xlib, wayland or win32. A backend knows its graphics API and nothing
else. Whoever owns the window fills in `RhiWindow` (`rhiwindow.h`): the instance extensions the window layer needs and
a `createSurface` callback that receives and returns backend-native handles as opaque pointers. The header documents
what each backend expects. ngen's SDL glue is a static function in `src/main.cpp`.

Swapchain size is passed in (`createSwapchain(extent)`, `recreate(extent)`). The backend never asks the window.

### The application chooses the backend

There is no factory. The application constructs the concrete type (`RhiDeviceVulkan`) and hands `RhiDevice*` to the
engine. Everything below `main.cpp` sees only the interface. Renderer code must not include anything from a backend
folder.

### No engine or app concepts

ImGui, scenes, materials, frame graphs, editor UI: none of it belongs here. The RHI vocabulary is buffers, textures,
samplers, shaders, pipelines, descriptor sets, command buffers, fences, semaphores, swapchains. If a type needs a name
from a higher layer to make sense, it is in the wrong folder. `ImGuiBackend` lives in `src/renderer/` for this reason.

### Bytecode in, no file IO

`createShaderModule` takes `RhiShaderDesc`: stage, a span of compiled bytecode in the backend's native format
(SPIR-V for Vulkan), entry point. Loading files and cross-compiling shader languages happen outside. ngen's
`loadShaderModule` in `src/renderer/shaderloader.cpp` does the file part.

### Errors are classes, not codes

Fallible calls return `std::expected<T, RhiError>`. `RhiError` names the class the caller can branch on: `Failed`,
`OutOfDate`, `Suboptimal`, `DeviceLost`. The backend logs the native code to stderr before returning; the caller never
sees a `VkResult`.

### Descs with defaults, flags with types

Every `*Desc` struct is designable with designated initializers and has defaults for optional fields. Flag enums opt in
to `RhiFlags<E>` via `RhiFlagEnum<E>`; `A | B` yields `RhiFlags<E>` and `.has(E)` tests a bit. The old bool-returning
`operator&` was removed because `usage & (A | B)` silently meant "any of".

## What an integrator provides

1. A window and an `RhiWindow` filled from it.
2. A concrete backend instance, `init(window)`, then `createSwapchain(extent)` with the framebuffer size in pixels.
3. Frame pacing: N command buffers, N fences, semaphores per frame slot and per swapchain image, and the decision of
   which resource is indexed by which. Fences and command buffers belong to the frame slot; the render-finished
   semaphore belongs to the swapchain image because `present` consumes it per image.
4. An upload path: staging buffers, `copyBuffer` / `copyBufferToTexture` on a command buffer, barriers
   `Undefined -> TransferDst -> ShaderReadOnly` for textures, a submit and a fence wait.
5. Deferred destruction keyed by frame, or a `waitIdle` before every destroy if stalls are acceptable.
6. Compiled shader bytecode and the code to read it.
7. `recreate(extent)` on `RhiError::OutOfDate` from `acquireNextImage` or `present`, followed by dropping anything
   that referenced the old swapchain images.

## Adding to the interface

Before adding a method or field, ask:

- Does it wait, block or hide a submit? Then it belongs in the engine, not here.
- Does it name a window system, a UI library, or an engine type? Then it belongs one layer up.
- Can D3D12 and Metal implement it without faking it? If not, either generalize it or document the limitation next to
  the declaration (see `lineWidth` in `RhiRasterState`).
- Does the Vulkan backend need something from the renderer to implement it? Then the layering is wrong.

Header-only interface changes touch every backend and every pass. Prefer adding a field with a default over changing a
signature.

## Known gaps

Kept honest rather than papered over. See the review that produced this file for the reasoning.

- Single queue, single command pool. `createCommandBuffer` is not thread safe.
- Barriers are layout-only image barriers; no buffer barriers, no explicit stage or access masks.
- One blend state for all color attachments.
- One push constant range per pipeline.
- Textures are 2D, one mip, one layer. `RhiSamplerDesc` is empty; every sampler is linear/repeat.
- `bindIndexBuffer` assumes 32-bit indices. `bindVertexBuffer` has no offset or slot.
- `RhiSwapchain::depthImage()` exists; depth is a renderer concern and will move out.
- Resource objects are virtual-dtor classes returned by raw pointer; `swapchain->image(i)` pointers are invalidated by
  `recreate`.
