# Render Debug window

**Status. Landed.**

## Current state

Render inspection is spread out: the frame graph window shows passes, resources and GPU times; the Performance window shows timelines; the
View menu holds the buffer view modes and overlays; `notes.md` holds numbers that had to be computed from obs dumps. Nothing shows what the
renderer holds on the GPU (meshes, triangles, textures, materials) or what a frame costs in draw calls.

The render thread already ships a `FrameGraphDebugSnapshot` to the editor each frame when the frame graph window is open
(`RenderThread::latestFrameGraphDebug`). The same route can carry a render debug snapshot.

## Scope

**In**

- `RhiCommandStats` on `RhiCommandBuffer`: draws, dispatches, barriers, pipeline binds, descriptor set binds, primitives (indices / 3 for
  triangle lists, vertices / 2 for lines). Reset at `begin()`, read with `cmd->stats()`. Counted in the Vulkan backend's overrides.
- Frame graph records the stats delta around each pass into `FgPassDebug` (draws, dispatches, barriers, primitives).
- `RenderDebugSnapshot` built on the render thread when the window is open, delivered like the frame graph snapshot:
  - device: name, driver, `RhiDeviceLimits`, calibrated timestamps, validation on;
  - swapchain: extent, colour format, image count, frames in flight, current slot;
  - scene: instance count, unique meshes with triangles, vertices, index and vertex buffer bytes; textures with size, format, mips, bytes;
    material count, how many fall back to the checkerboard; light count and the active sun (direction, radiance, shadow colour);
  - frame: per-pass counters from the graph, totals, transient pool contents (texture, size, format) and pool allocations this frame;
- `src/ui/renderdebugwindow.*` with tabs Scene, Frame, View, Device. View tab hosts the buffer view radio group, the overlay toggles and
  the FXAA toggle; the menu entries stay, both write the same flags.
- Menu item Windows > Render Debug.
- Draw list (added after landing): passes call `FrameGraphContext::beginDraw/endDraw` with mesh, material, prim, index range while the
  window is open; the Frame tab lists draws per pass with the USD prim path, click selects the prim. "Time draws" wraps a page of up to 64
  consecutive draws of one pass in GPU zones; the renderer joins the zone times with the slot's draw log after the fence.

**Out**

- Wireframe (needs a polygon mode in `RhiRasterState` and a second geometry pipeline), shadow map resolution and light distance sliders,
  culling counters, texture thumbnails, GPU memory from the allocator. Each listed under Deferred with its trigger. Per-draw timing for
  every draw at once: the zone budget is 128 per command buffer, so timing is paged.

## Decisions

- **Counters live in the RHI command buffer, not in passes.** Every draw goes through the RHI anyway; counting there is one increment
  per call and covers passes that have not been written yet. The RHI already exposes GPU zones and labels on the same object.
- **Snapshot, not shared state.** The scene tables are read from renderer state that belongs to the render thread; copying them into a
  snapshot once per frame while the window is open is the same pattern the frame graph debugger uses, and it is a few hundred bytes for
  the mesh and texture tables on Sponza.
- **Device name through the RHI.** `RhiDeviceLimits` gains `deviceName` and `driverName` strings filled at init; tiny, and the Performance
  and Render Debug windows both want it.

## Steps

1. `RhiCommandStats` in `rhitypes.h`; `stats()` on `RhiCommandBuffer`; Vulkan counts in `draw`, `drawIndexed`, `dispatch`, `pipelineBarrier`,
   `bindPipeline`, `bindDescriptorSet`, `blitTexture`, copies; reset in `begin()`. Device name and driver in limits.
2. `FgPassDebug` gains `draws`, `dispatches`, `barriers`, `primitives`; `FrameGraph::execute` snapshots `cmd->stats()` before and after each
   pass and stores the delta on the pass node; `buildDebugSnapshot` copies it.
3. `RenderDebugSnapshot` in `src/renderer/renderdebug.h`; `Renderer::buildRenderDebugSnapshot()`; `RenderThread` gains a wanted flag and a
   slot, same as the frame graph snapshot; `main.cpp` passes it to `EditorUI::draw`.
4. `renderdebugwindow.*`: four tabs, tables with sortable columns for meshes and textures (ImGui table sorting), totals row.
5. Menu entry, editor flags, `PROFILE_ZONE("RenderDebugWindow")` around the draw.

## Verification

- Headless three_cubes: `FrameGraphCompiled` unchanged; a new `RenderStats` observation every 60 frames with draws, primitives, meshes,
  textures matches the scene (3 cubes: 3 or 4 meshes, 36 triangles per cube).
- Sponza: Scene tab shows 115 meshes and 3.75 M triangles, 25 textures at 4096x4096 with 1 mip; Frame tab shows GeometryPass draws equal to
  the instance count and ShadowPass draws equal to prim-first instances. Henrik, visual.
- Validation clean; example sweep unchanged (the RHI addition is additive).
- `ngen-view --render-debug` keeps the snapshot and draw log on without the window; `RenderStats.logged_draws` equals the geometry plus
  shadow draw count (8 on three_cubes). This is how the missing `setDrawLogEnabled` call was caught.

## Deferred / follow-ups

- **Wireframe toggle.** Trigger: first time a mesh looks wrong and the normals view is not enough. `RhiPolygonMode` in `RhiRasterState`.
- **Shadow resolution and light distance sliders.** Trigger: the shadow pass work in `notes.md`.
- **Culling counters** (drawn versus culled, per pass). Trigger: frustum culling lands.
- **Texture thumbnails** via the frame graph preview registration. Trigger: first texture bug hunted by eye.
- **GPU memory totals** from the backend allocator. Trigger: an out-of-memory on a real scene.
