# Frame debugger

**Status. Landed.**

Stage 3 of [plan_introspection.md](plan_introspection.md). It builds on the capture service ([plan_frame_capture.md](plan_frame_capture.md)).

## Current state

The Frame Graph window shows the graph's structure: passes, accesses, resource lifetimes and thumbnails. The Render Debug window shows per-pass stats
and the draw log. None of them shows what a pass actually recorded:
- the barriers the graph issued before it, and what they became in Vulkan (stages, accesses, layouts)
- the commands in it
- the descriptor sets it bound and what those point at
- its push constants

With indirect draws, "what did this pass draw" is also only visible as a count.

## Scope

**In**

- **Barrier records.** The frame graph keeps, per pass, every barrier it issued: resource, kind, old and new access, and old and new state. The records
  are always kept (a handful per pass), and they are in the frame-graph debug snapshot.
- **Derived barrier details (RHI).** `RhiDevice::describeTransition(old, new)` for texture and buffer states returns the backend's stage masks,
  access masks and layouts as text. The Vulkan backend uses the same functions it records barriers with.
- **Command log (RHI).** `RhiCommandBuffer::setCommandLog(bool)` records one text line per command while on:
  - pipeline, set, vertex/index buffer binds with the objects' debug names
  - push constants as floats and hex
  - draws, indirect draws, dispatches, copies, blits, barriers, rendering begin/end with attachments, viewport and scissor
  
  The Vulkan resource structs keep their debug name for this. The graph slices the log per pass.
- **Descriptor contents (RHI).** `RhiDevice::describeDescriptorSet(set)` returns the last written resource per binding and array element, recorded by
  `updateDescriptorSet`.
- **Frame capture:** one frame recorded with the command log on, delivered as a `FrameDebugCapture`:
  - passes in execution order with accesses, barriers with derived details, commands, stats and GPU time
  - every descriptor set any pass bound, with contents
- **Frame Debugger window** (Windows > Introspection):
  - a pass list; for the selected pass, tabs for Overview, Barriers, Commands, Descriptors, Resources and Draws
  - Resources shows every resource the pass writes, before (after the previous pass in execution order) and after, through capture watches; a click
    opens it in the Capture window
  - Draws, for indirect passes, lists the pass's draw commands from a `drawCommands` capture, with prim paths
- **Dump and verb:** `dump-frame DIR` writes `DIR/frame.json` (the whole `FrameDebugCapture`). It also writes a capture of every resource each pass
  writes, after that pass: textures as PNG plus JSON, buffers as JSON.

**Out**

- Replaying or editing a frame, and stepping inside a pass draw by draw. RenderDoc (stage 8) does that.
- Shader source and reflection per pipeline: stage 7.

## Decisions

Made while planning, following the umbrella's recommendations.

- **Barrier records always, command log on demand.** Barriers are a few per pass and cheap to keep, so the Frame Graph window can show them too. The
  command log formats text for every command, so it only runs on the captured frame.
- **Before/after images come from live captures, labelled with their frame.** The frame capture records structure. The images are captured when a pass
  is selected, which is a later frame, and they show that frame's number. Capturing every resource of every pass on each frame capture would read
  back hundreds of MB. `dump-frame` does exactly that, once, for headless use.
- **Names travel with RHI objects.** The Vulkan structs keep the debug name they were given, so the command log and descriptor contents print names
  without a lookup table.

## Steps

1. RHI:
   - `RhiTransitionInfo` and `describeTransition` for texture and buffer states.
   - `RhiCommandRecord` and the command log on `RhiCommandBuffer`.
   - `RhiDescriptorInfo` and `describeDescriptorSet`.
   - Vulkan: names stored in the resource structs; logging in every command; descriptor writes recorded; the barrier helpers made callable by the
     device.
2. Frame graph: `FgBarrierRecord` per pass; command log slice per pass when logging (log length before the pass's barriers, after its execute);
   bound descriptor sets per pass (from the log records).
3. Renderer: `requestFrameDebugCapture()`, which turns on the command log for the next frame and then builds `FrameDebugCapture` with derived
   barrier details and descriptor contents. Delivered through `RenderThread` like the other debug snapshots.
4. UI: `FrameDebuggerWindow`, and capture watches for the selected pass's resources (before and after) and for `drawCommands`.
5. `dump-frame DIR` in `main.cpp`: the frame capture plus capture watches for every written resource after every pass, all on the same frame; the
   files are written when every result has arrived.

## Verification

- `dump-frame` on Sponza writes `frame.json` with every executed pass, in order. `GeometryPass` has:
  - barriers such as `gbuffer.albedo` `Undefined → ColorAttachment` with Vulkan stages (`COLOR_ATTACHMENT_OUTPUT`) and layouts
    (`UNDEFINED → COLOR_ATTACHMENT_OPTIMAL`), and `drawCounts` `StorageWrite → IndirectRead` with `DRAW_INDIRECT`/`INDIRECT_COMMAND_READ`
  - a command log with `bindPipeline geometry.pipeline.cullback.less`, `bindDescriptorSet geometry.set.slotN`, `drawIndexedIndirectCount
    cull.commands …`
  - the descriptor set's contents: binding 2 → `gpuscene.instances`, binding 1 element 0 → `texture.fallback`, element 1 → `material.N.basecolor`
- `ShadowPass` commands show one push constant per cascade (the cascade matrix as floats).
- The dump directory holds a PNG for every texture a pass writes (G-buffer, depth, shadow atlas, scene colour, AA output) and JSON for the culling
  buffers.
- A frame with the command log on renders the same image as one without (screenshot byte-identical), and validation stays clean.

## Results

- `dump-frame` on Sponza writes `frame.json` with all 13 executed passes in order, each with barriers carrying Vulkan stages, accesses and layouts.
- Command logs print object names; `ShadowPass` shows one push constant per cascade.
- Descriptor contents resolve: binding 1 holds the fallback and the material textures, binding 2 `gpuscene.instances`.
- The dump directory holds a capture of every resource each pass writes.
- The `drawCounts`/`drawCommands` transition to indirect read appears in `ShadowPass`, the first reader after `CullReadback`, not in `GeometryPass`
  as the Verification section guessed.
- Screenshots byte-identical with the command log on; validation clean.
