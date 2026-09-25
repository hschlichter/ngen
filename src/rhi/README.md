# RHI

`src/rhi/` is the render hardware interface: a thin, header-only abstraction over one graphics API at a time.
`RhiDevice`, `RhiCommandBuffer` and `RhiSwapchain` are abstract classes; each backend lives in a sibling folder
(`vulkan/`, later `d3d12/`, `metal/`) and is the only code that includes that API's headers. The build links exactly
one backend per platform.

This file states the principles the interface is built on, how the interface is organised, how the Vulkan backend
implements it, and what an integrator (an engine, an app, a test harness) is expected to provide. Read it before adding
to the interface or writing code on top of it.

## Layout

| File | Contents |
|---|---|
| `rhitypes.h` | Enums, flag types, descs, the opaque resource classes, barrier and submit structs, `RhiCommandStats`, `RhiDeviceLimits` |
| `rhidevice.h` | `RhiDevice`: creates and destroys everything, descriptor updates, submission, presentation, mapping, queries, limits |
| `rhicommandbuffer.h` | `RhiCommandBuffer`: recording (rendering, barriers, copies, draws, indirect draws, dispatch, timestamps, GPU zones, labels) |
| `rhiswapchain.h` | `RhiSwapchain`: acquire, image access, recreate |
| `rhiwindow.h` | `RhiWindow`: the contract with whoever owns the window |
| `vulkan/` | The Vulkan backend: `RhiDeviceVulkan`, `RhiCommandBufferVulkan`, `RhiSwapchainVulkan`, resource structs in `rhiresourcesvulkan.h` |
| `examples/` | One program per feature plus `common/` (window glue, upload, readback, shader compile, PNG, the `RhiExample` base) |

The interface is header-only. Only the backend folder has `.cpp` files and includes the graphics API's headers.

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

## The interface

### Object model

`RhiDevice` is the factory and owner of everything: `createX` returns a pointer to an opaque class (`RhiBuffer`,
`RhiTexture`, `RhiSampler`, `RhiShaderModule`, `RhiPipeline`, `RhiDescriptorSetLayout`, `RhiDescriptorPool`,
`RhiDescriptorSet`, `RhiQueryPool`, `RhiSemaphore`, `RhiFence`, `RhiCommandBuffer`, `RhiSwapchain`), and a matching
`destroyX` frees it immediately. The classes carry no methods; the backend downcasts them to its own structs. Creation
takes a `*Desc` with defaults; failures return `nullptr` (resources) or `std::expected` (`init`, `recreate`, acquire,
present) after logging the native error.

### Resources

- **Buffers.** `RhiBufferDesc { size, usage, memory }`. Usage flags: `TransferSrc`, `TransferDst`, `Vertex`, `Index`,
  `Uniform`, `Storage`, `Indirect`. Memory is `GpuOnly` (device-local, filled by copies) or `CpuToGpu` (host-visible and
  coherent; `mapBuffer` returns a pointer that stays valid until `unmapBuffer` or destroy). Readback buffers use
  `CpuToGpu` as well.
- **Textures.** `RhiTextureDesc { width, height, format, usage, mipLevels, arrayLayers, sampleCount, dimension }`;
  dimensions `Texture2D`, `Texture2DArray`, `TextureCube`. Usage flags: `Sampled`, `ColorAttachment`, `DepthAttachment`,
  `Storage`, `TransferSrc`, `TransferDst`. Optional formats are checked with `supportsTextureFormat(format, usage)`.
  Textures are always device-local and filled with `copyBufferToTexture`.
- **Samplers.** Filters, mip mode, address modes, anisotropy (ignored when `limits().samplerAnisotropy` is false),
  LOD bias and range, and an optional compare op for shadow sampling.
- **Shaders.** `RhiShaderDesc { stage, code, entryPoint }` with bytecode in the backend's format.

### States and barriers

Synchronisation is expressed as state transitions, not stage and access masks. `RhiTextureState` (`Undefined`,
`ColorAttachment`, `DepthStencilAttachment`, `ShaderReadOnly`, `General`, `TransferSrc`, `TransferDst`, `PresentSrc`) and
`RhiBufferState` (`Undefined`, `VertexRead`, `IndexRead`, `UniformRead`, `StorageRead`, `StorageWrite`, `TransferSrc`,
`TransferDst`, `IndirectRead`) name what a resource is used for; `pipelineBarrier(textureBarriers, bufferBarriers)`
moves resources from an old to a new state and the backend derives layouts, stages and accesses. The caller tracks the
current state; the RHI does not. Shader states cover every shader stage (vertex, fragment, compute), which is
conservative but correct.

### Descriptors

Vulkan-shaped: a `RhiDescriptorSetLayout` from `RhiDescriptorBinding { binding, type, stage, count }`, a
`RhiDescriptorPool` sized for `maxSets` sets of those bindings, sets allocated from the pool, and `updateDescriptorSet`
with `RhiDescriptorWrite { binding, arrayElement, type, buffer/offset/range or texture/sampler }`. Types:
`UniformBuffer`, `StorageBuffer`, `CombinedImageSampler` (texture in `ShaderReadOnly`), `StorageImage` (texture in
`General`). `count > 1` makes an array binding: every element must be written before the set is bound, and shaders may
index it with a value that is uniform per draw. Sets are bound per pipeline with `bindDescriptorSet(pipeline, index, set)`.
A set must not be updated while a submitted command buffer still uses it.

### Pipelines

`createGraphicsPipeline(RhiGraphicsPipelineDesc)`: vertex and fragment shaders, set layouts, one push constant range,
colour and depth formats (dynamic rendering: no render pass objects), one vertex binding with a stride and attributes,
topology (`TriangleList`, `LineList`), raster (cull mode, front face, line width), depth and one blend state.
`createComputePipeline(RhiComputePipelineDesc)`: one shader, set layouts, one push constant range. Both return
`RhiPipeline`; `bindPipeline` picks the bind point, and `bindDescriptorSet`/`pushConstants` use the pipeline's layout.

### Recording

A command buffer is recorded between `begin()` and `end()` and reset for reuse. Rendering is dynamic:
`beginRendering(RhiRenderingInfo)` names the colour and depth attachments with their state, load (clear or keep) and
clear values; draws happen inside, dispatches and copies outside. Commands:

- viewport and scissor (with offset, for atlas tiles), pipeline, vertex and index buffer, descriptor set, push constants
- `draw`, `drawIndexed`, `drawIndexedIndirect(commands, offset, drawCount)` and
  `drawIndexedIndirectCount(commands, offset, count, countOffset, maxDrawCount)` reading packed
  `RhiDrawIndexedIndirectCommand`s (layout of `VkDrawIndexedIndirectCommand` and `D3D12_DRAW_INDEXED_ARGUMENTS`), each
  with its own `firstInstance`
- `dispatch`
- `copyBuffer`, `copyBufferToTexture`, `copyTextureToBuffer`, `blitTexture` (per-side mip level and filter)
- `pipelineBarrier`

### Timing, statistics and labels

- **Timestamps.** `createQueryPool`, `resetQueryPool`/`writeTimestamp` on a command buffer, `readTimestamps` after the
  fence; `limits().timestampPeriodNs` converts ticks.
- **GPU zones.** `beginGpuZone(name)`/`endGpuZone()` nest on a command buffer; `collectGpuZones(cmd, out)` returns them
  with depth and start/end in nanoseconds once the fence has passed. `calibrateGpuClock` samples the GPU and CPU clocks
  together so zones can be placed on the CPU timeline.
- **Command stats.** `RhiCommandStats` counts, since `begin()`: draws, dispatches, barriers, pipeline binds, descriptor
  binds, vertex/index buffer binds, indirect draw calls, copies and estimated primitives. Indirect calls count once; their
  draws and primitives are GPU-side and not counted.
- **Labels.** `beginLabel`/`endLabel` for RenderDoc and validation messages; no-ops without debug support.

### Submission and presentation

`submitCommandBuffer(cmd, { waitSemaphore, signalSemaphore, fence })` submits one command buffer on the single queue.
`acquireNextImage(semaphore)` returns the swapchain image index, `present(swapchain, waitSemaphore, index)` presents it;
both report `OutOfDate`/`Suboptimal` for the caller to `recreate(extent)`. `waitForFence`/`resetFence` pace the CPU.
`limits()` reports device name, driver, alignment and push constant limits, line width, anisotropy, timestamp support,
calibration and the per-stage sampled-image limit. `validationErrorCount()` turns validation output into a number a
program can check.

## Vulkan backend

`vulkan/` implements the interface on Vulkan and is the only code that includes Vulkan headers.

- **Device.** `RhiDeviceVulkan::init` creates the instance (with `VK_EXT_debug_utils` when available, and the
  Khronos validation layer when `RhiDeviceOptions::enableValidation` is set), picks the first physical device with a
  graphics queue that can present to the window's surface, and creates one queue and one command pool.
- **Required features.** API 1.2 or newer, `synchronization2`, `dynamicRendering`, `shaderSampledImageArrayDynamicIndexing`,
  `multiDrawIndirect`, `drawIndirectFirstInstance` and `drawIndirectCount`; `init` fails with a message when one is
  missing. `wideLines`, `samplerAnisotropy` and `VK_EXT_calibrated_timestamps` are enabled when present and reported in
  `limits()`.
- **Memory.** One `vkAllocateMemory` per buffer and texture, device-local for `GpuOnly` and textures, host-visible and
  coherent for `CpuToGpu`. There is no suballocator.
- **Barriers.** `pipelineBarrier` maps each state to a layout (textures), stage mask and access mask and records one
  `vkCmdPipelineBarrier2` for all texture and buffer barriers.
- **Command buffers.** Each `RhiCommandBufferVulkan` owns a timestamp query pool for up to 128 GPU zones, reset at
  `begin()`, and counts `RhiCommandStats` as commands are recorded.
- **Swapchain.** FIFO present mode, an sRGB surface format when available; `recreate` rebuilds the images, which
  invalidates earlier `image(i)` pointers.
- **Validation.** Messages go to stderr; errors and warnings are counted, and `validationErrorCount()` reports the errors.

## What an integrator provides

`src/rhi/examples/common/rhiexample.h` is the reference implementation of this list, numbered sections in run order.
`src/rhi/examples/triangle.cpp` is the smallest program on top of it: shaders, pipeline, draw, checks.

1. A window and an `RhiWindow` filled from it.
2. A concrete backend instance, `init(window)`, then `createSwapchain(extent)` with the framebuffer size in pixels.
3. Frame pacing: N command buffers, N fences, semaphores per frame slot and per swapchain image, and the decision of
   which resource is indexed by which. Fences and command buffers belong to the frame slot; the render-finished
   semaphore belongs to the swapchain image because `present` consumes it per image.
4. An upload path: staging buffers, `copyBuffer` / `copyBufferToTexture` on a command buffer, barriers
   `Undefined -> TransferDst -> ShaderReadOnly` for textures, a submit and a fence wait.
5. Deferred destruction keyed by frame, or a `waitIdle` before every destroy if stalls are acceptable.
   Readback is the same shape in reverse: `copyTextureToBuffer` into a host-visible buffer, wait on the frame's fence, map.
6. Compiled shader bytecode and the code to read it.
   Optional formats (depth-stencil, compressed) checked with `supportsTextureFormat` before use.
7. `recreate(extent)` on `RhiError::OutOfDate` from `acquireNextImage` or `present`, followed by dropping anything
   that referenced the old swapchain images.

## Examples

`src/rhi/examples/` holds small programs, one `.cpp` each, that use the RHI the way an integrator would. They are the
verification loop for RHI changes, for humans and for agents. No registry, no reporting: each example is a plain program
with flags and an exit code.

`common/rhiexample.h` is the shared part: window, device, swapchain, frame pacing, frame loop, readback, flags, exit code.
An example derives `RhiExample` and overrides `setup`, `record`, `check`, `teardown`. Read `rhiexample.h` top to bottom for
the integrator contract; read an example for what a specific feature needs. Only what every example needs goes in the base.

Build and run unattended:

```sh
./_out/ngen-build -p linux-vulkan -c debug ngen-example-triangle
SDL_VIDEODRIVER=offscreen ./_out/linux-vulkan/debug/ngen-example-triangle --frames=60 --check --validation
```

The whole set, after any RHI change (the default target builds only `ngen-view`, so a sweep without this runs stale binaries):

```sh
./_out/ngen-build -p linux-vulkan -c debug examples
for t in triangle quad texture uniforms depth rendertarget pushconstants blend lines mipcube compute timestamps gpuzones bindless indirect; do
  SDL_VIDEODRIVER=offscreen ./_out/linux-vulkan/debug/ngen-example-$t --frames=10 --check --validation >/dev/null 2>&1; echo "$t=$?"
done
```

Flags every example supports:

- `--frames=N` render N frames then exit. `0` (default) runs until the window closes.
- `--size=WxH` window size in pixels. Fixed default so output is deterministic across runs.
- `--check` read the last frame back and assert pixel values derived from constants in the source. Exit 2 on mismatch.
- `--screenshot=PATH` read the last frame back and write a PNG. Humans look at it; agents read it.
- `--resize-at=N` resize the window at frame N to exercise swapchain recreation.
- `--validation` enable the backend validation layer; any error message fails the run with exit 2.

Exit codes: `0` ok, `1` setup failed (device, swapchain, shader, pipeline), `2` a check or validation failed. The last
stdout line is a one-line summary: `triangle: ok frames=60 validation_errors=0`.

Examples, each adding one concept to the previous:

| target | shows |
|---|---|
| `ngen-example-triangle` | pipeline, draw, clear, present, resize, `RhiCommandStats` counts |
| `ngen-example-quad` | staging upload, vertex attributes, `drawIndexed` with uint16 and uint32 |
| `ngen-example-texture` | `copyBufferToTexture`, samplers, descriptor sets |
| `ngen-example-uniforms` | per-frame-slot uniform buffer, `bufferOffset`, `minUniformBufferOffsetAlignment` |
| `ngen-example-depth` | example-owned depth texture, `resized()`, four `RhiDepthState`s including `Equal`, `supportsTextureFormat`, compare sampler |
| `ngen-example-rendertarget` | render to texture, sample it, `blitTexture`, every layout transition |
| `ngen-example-pushconstants` | one push constant range read by both stages, `maxPushConstantSize` |
| `ngen-example-blend` | blend factors, cull mode, front face |
| `ngen-example-lines` | `LineList`, `lineWidth`, `wideLines` fallback |
| `ngen-example-mipcube` | mip levels, array layers, cube faces, one copy per subresource, blit from a mip level, sampler `mipLodBias` and `minLod` |
| `ngen-example-compute` | compute pipelines, storage image and buffer, `dispatch`, buffer barriers |
| `ngen-example-timestamps` | query pools, `writeTimestamp`, readback after the fence, `timestampPeriodNs`, `calibrateGpuClock` |
| `ngen-example-gpuzones` | nested `beginGpuZone`/`endGpuZone`, `collectGpuZones` depth, order and containment |
| `ngen-example-indirect` | `drawIndexedIndirect` and `drawIndexedIndirectCount`, `RhiDrawIndexedIndirectCommand` with per-command `firstInstance`, a GPU count below `maxDrawCount`, `indirectDraws` stat |
| `ngen-example-bindless` | array binding (`RhiDescriptorBinding::count`, `RhiDescriptorWrite::arrayElement`) indexed per draw through `firstInstance`, one descriptor bind for eight textures, `maxPerStageSampledImages` |

Run them all: `for t in triangle quad texture uniforms depth rendertarget pushconstants blend lines mipcube compute timestamps gpuzones bindless indirect; do
SDL_VIDEODRIVER=offscreen ./_out/linux-vulkan/debug/ngen-example-$t --frames=10 --check --validation || echo "$t FAILED"; done`

Rules for examples: include only headers under `src/rhi/` and `examples/common/` (plus `stb_image_write.h` for PNG);
shaders embedded as GLSL strings and compiled at startup with shaderc; no animation or time dependence so a screenshot
is byte-stable; every check derives its expected value from a constant in the same file, never from a stored image;
helpers a feature needs (depth buffer, uploads, descriptors) start next to the example that needs them and move into
`common/` only when a second example repeats them.

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

Kept honest rather than papered over.

- Single queue, single command pool. `createCommandBuffer` is not thread safe.
- `init` checks for API 1.2, but `synchronization2` and `dynamicRendering` are enabled as core features without their
  extensions, which a 1.2 device only accepts with `VK_KHR_synchronization2` and `VK_KHR_dynamic_rendering`. In practice
  the backend needs a Vulkan 1.3 device.
- One device memory allocation per resource, no suballocator; fine at current resource counts, a limit for streaming.
- Barriers are resource-state transitions (`RhiTextureState` for textures, `RhiBufferState` for buffers); the backend
  derives stages and access. No explicit masks, no split barriers, no queue ownership transfer.
- One blend state for all color attachments.
- One push constant range per pipeline.
- `blitTexture` takes a mip level per side (`RhiBlitRegion`) and a filter; layers are still 0 only.
- No compressed formats (BC/ASTC). Add when an asset path produces them.
- Resource objects are virtual-dtor classes returned by raw pointer; `swapchain->image(i)` pointers are invalidated by
  `recreate`. Opaque generational handles would fix this; deferred until a second backend makes the cost worth it.
- Compute shares the graphics queue. No async compute, no indirect dispatch (indirect draws exist).
- Indirect draws are counted per call in `RhiCommandStats::indirectDraws`; their draws and primitives are GPU-side and
  not in `draws`/`primitives`. Callers that know them report them (ngen: `FrameGraphContext::addIndirectStats`).
- Descriptor model is Vulkan-shaped (pool, layout, set, write). D3D12 and Metal can implement it, but it is not their
  native shape; revisit when a second backend exists.
- Array bindings are fixed-size and must be fully written before binding; indexing must be dynamically uniform (one
  value per draw). No runtime-sized arrays, partially bound sets, update-after-bind or non-uniform indexing: every draw
  reads one material, so dynamically uniform indexing (a core 1.0 feature) covers bindless materials and multi-draw
  indirect alike. Add descriptor indexing when textures stream or a scene outgrows a fixed array.
