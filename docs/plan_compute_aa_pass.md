# Compute AA pass (first frame-graph compute node)

**Status. Landed.**

## Current state

The RHI has compute since `docs/plan_rhi_compute.md`: `createComputePipeline`, `dispatch`, storage descriptors, `RhiTextureState::General`,
buffer barriers. Nothing in the renderer uses it. The frame graph (`src/renderer/framegraph.*`) knows only graphics accesses
(`FgAccessFlags`: colour, depth, shader read, transfer, present) and only texture resources; `FgBufferHandle` is a stub.

`AAPass` (`src/renderer/passes/aapass.cpp`) is a placeholder: it blits `sceneColor` to `sceneColorAA` unchanged. Its own comment says a real
implementation would read `sceneColor` in a shader and write `sceneColorAA`. That is exactly a compute pass, and the slot in the frame
(after lighting, before debug lines, gizmos and UI) is already there.

This plan is also the consumer that `src/rhi/README.md` "Known gaps" waits for before the queue and command pool work (async compute) is
designed: once a compute pass exists in the frame, overlapping it with graphics has a concrete test.

## Scope

**In**

- Frame graph: `FgAccessFlags::StorageRead` and `StorageWrite` mapping to `RhiTextureState::General`; debug and UI name tables updated.
- `AAPass` becomes a compute FXAA: samples `sceneColor`, writes `sceneColorAA` through `imageStore`, one workgroup per 8x8 pixels.
- `shaders/fxaa.comp` compiled by the existing glslc tool (`.comp` added to the glob).
- Per-frame-slot descriptor sets in the pass, updated at execute time because transient textures change per frame (same pattern as
  `LightingPass`).
- `sceneColorAA` format becomes `R16G16B16A16_SFLOAT` (storage images cannot be sRGB) and holds only the AA result. It is blitted to
  the backbuffer straight after the pass; debug lines, gizmos and UI draw on the backbuffer after that, in the swapchain format, so the
  filter never touches them and the frame graph preview of `sceneColorAA` shows the pure FXAA output.
- A runtime toggle in the existing render snapshot flags so the effect can be compared: `RenderSnapshot::antiAliasing` bool, default on,
  exposed where `showBufferOverlay` is.

**Out**

- Buffer resources in the frame graph (`FgBufferHandle`, buffer barriers between passes). Trigger: a compute pass that produces a buffer
  another pass consumes, for example light culling.
- Async compute, second queue, command pools. Own plan; this pass is its test case.
- Temporal AA, MSAA. FXAA is chosen because it needs one input and one output and has a well-known reference implementation.

## Decisions

- **FXAA, not a blur.** A box filter would prove the plumbing but leave the frame worse. FXAA is the smallest filter that is an improvement,
  and it is what the pass has been named all along. Reference: the standard FXAA 3.11 "console" quality luma-based edge blend, written
  plainly in GLSL, about 80 lines. No subpixel tuning beyond the reference defaults.
- **Compute, not a fullscreen fragment pass.** Same shader math would work as a fragment pass writing a colour attachment, but the point of
  this plan is the first compute node; a fullscreen triangle would prove nothing new.
- **Descriptor sets per frame slot, written at execute.** Transient textures come from the resource pool and may differ per frame, so the
  set is updated inside the execute lambda with `updateDescriptorSet`. Slot-indexed so the update never touches a set an in-flight frame
  still binds. Identical to `LightingPass`.
- **Linear float intermediate.** `R16G16B16A16_SFLOAT` for `sceneColorAA`, checked with `supportsTextureFormat(format, Storage |
  ColorAttachment | Sampled | TransferSrc)` at init; fall back to the blit pass with a log line if unsupported. Costs 2x bandwidth on one
  1080p-class texture; acceptable. Rejected: `R8G8B8A8_UNORM` with manual sRGB encode in the shader, because the overlays drawn afterwards
  would land linear in an encoded target.

## Steps

1. **Frame graph access flags.** `src/renderer/framegraphresource.h`: add `StorageRead = 1 << 6`, `StorageWrite = 1 << 7`.
   `framegraph.cpp accessToLayout`: both map to `RhiTextureState::General`. `framegraphdebug.h`, `ui/framegraphwindow.cpp`,
   `ui/framegraphnodeview.cpp`, `framegraphpreviews.cpp`: name and colour entries for the two flags. Compile and headless run unchanged.

2. **Shader.** `shaders/fxaa.comp`:

   ```glsl
   #version 450
   layout(local_size_x = 8, local_size_y = 8) in;
   layout(set = 0, binding = 0) uniform sampler2D sceneColor;
   layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D sceneColorAA;
   layout(push_constant) uniform Push { vec2 inverseSize; uint enabled; } push;
   ```

   Luma from the sampled neighbourhood, edge detection with the reference thresholds, blend along the edge direction, `imageStore`.
   When `enabled == 0` the shader copies the centre texel so the toggle costs no pipeline switch. `build.cpp`: add
   `glob({.include = "shaders/*.comp"})` to the `shaders` tool.

3. **Pass.** `src/renderer/passes/aapass.h/.cpp` becomes a class with the same shape as `LightingPass`:

   ```cpp
   class AAPass {
   public:
       auto init(RhiDevice* device, uint32_t frameCount) -> bool;
       auto destroy(RhiDevice* device) -> void;
       auto addPass(FrameGraph& fg, FgTextureHandle input, RhiExtent2D extent, uint32_t frameSlot, RhiSampler* sampler, bool enabled) -> const AAPassData&;
   private:
       RhiShaderModule* shader = nullptr;
       RhiDescriptorSetLayout* setLayout = nullptr;
       RhiPipeline* pipeline = nullptr;
       RhiDescriptorPool* pool = nullptr;
       std::vector<RhiDescriptorSet*> sets; // one per frame slot
       RhiFormat outputFormat = RhiFormat::R16G16B16A16_SFLOAT;
       bool supported = false;
   };
   ```

   Setup: `builder.read(input, FgAccessFlags::ShaderRead)`, `builder.write(createTexture("sceneColorAA", {extent, outputFormat, Storage |
   ColorAttachment | Sampled | TransferSrc}), FgAccessFlags::StorageWrite)`. Execute: `updateDescriptorSet(sets[slot], {sampled input,
   storage output})`, `bindPipeline`, `bindDescriptorSet`, `pushConstants`, `dispatch((w + 7) / 8, (h + 7) / 8, 1)`. When `supported` is
   false, fall back to the current blit body so the frame still renders.

4. **Renderer wiring.** `renderer.cpp`: `aaPass.init(device, imgCount)` next to the other passes, `destroy` in `Renderer::destroy`, and the
   `addAAPass` call becomes `aaPass.addPass(frameGraph, lightData.sceneColor, ext, imageIdx, textureSampler, snapshot.antiAliasing)`.
   The blit to the backbuffer moves up to directly after AA; debug, gizmo and UI passes take `colorHandle` (the backbuffer) as their
   target and keep the swapchain format. First landed with the overlays drawing into `sceneColorAA`; Henrik spotted the composited UI in
   the `sceneColorAA` preview, which is why the split exists.

5. **Shader search path** (found while landing). The engine loaded `shaders/*.spv` relative to the working directory, which happened to
   contain stale, git-ignored copies from an earlier build; the new `fxaa.comp.spv` only existed under `_out/`. `shaderloader` gained
   `setShaderSearchPath`, and `main.cpp` passes `SDL_GetBasePath()` so shaders resolve next to the executable, where the build writes them.
   The stale copies in `shaders/` can be deleted.

6. **Toggle.** `RenderSnapshot::antiAliasing = true`; the editor's view menu gets a checkbox next to the buffer overlay toggle; main copies
   it into the snapshot like the other flags. Observation `OBS_EVENT("Render", "AntiAliasing", "aa").field("enabled", ...)` emitted when it
   changes.

## Verification

- `./_out/ngen-build -p linux-vulkan -c debug` builds; `fxaa.comp.spv` appears in the out dir.
- Headless three_cubes run with `--validation`: zero validation messages; `PassExecuted` for `AAPass` once per frame; `FrameGraphCompiled`
  pass count unchanged from before (the pass is replaced, not added). Verified: 7784 `AAPass` events, pass count 9 before and after,
  `sceneColorAA` allocated as `R16G16B16A16_SFLOAT`.
- Frame graph preview window shows `sceneColorAA` with visibly softened cube edges compared with `sceneColor` and no overlays in it;
  the two look identical with the toggle off. Henrik confirms visually; the machine cannot screenshot the window.
- Debug lines, gizmos and ImGui render on the backbuffer unchanged from before this plan: visual check.
- `ngen-example-*` sweep unchanged (no RHI change in this plan).

## Deferred / follow-ups

- **Buffer resources in the frame graph** with `RhiBufferState` barriers between passes. Trigger: light culling or GPU-driven visibility,
  which produce buffers consumed by graphics passes.
- **Async compute** (`src/rhi/README.md` group 1): run this pass on a compute queue overlapping the shadow pass of the next frame. Trigger:
  a profile showing the pass on the critical path; plan `docs/plan_rhi_queues.md`.
- **Tone mapping** in the same compute pass once the lighting output moves to HDR; the float intermediate is already there.
