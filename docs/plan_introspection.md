# Debugging and introspection (umbrella)

**Status. Landed.** Stages 1–6 and 8 landed in one pass, in the order below. Stage 7 (shader introspection) is deferred; see Deferred / follow-ups.

Umbrella plan: it sets the principles and the order of the stages. Each stage gets its own `docs/plan_*.md`. It follows up the GPU-driven work
(`docs/plan_gpu_driven.md`), which removed per-draw timing and moved culling out of the CPU's sight.

The engine exists to learn from. The goal here is visibility into as much of it as possible: every resource, every pass, every barrier, every draw
command, every byte of GPU memory, every culling decision. It should be possible to see what happened in a frame, why it happened, and what it cost.

## Current state

What exists today:

- **Render Debug window:**
  - Scene tab: meshes, textures, and the texture inspector (mip levels, level dumps).
  - Frame tab: per-pass stats and GPU time, the draw log (read back and lagged), transient pool textures.
  - View tab: G-buffer views (albedo, normals, depth, shadow factor/map/UV, world position, mip level, cascades), overlays, live sampler settings.
  - Device tab: limits and features.
- **Frame Graph window:** a list view (passes, accesses, resources, lifetimes) and a node view with thumbnails of colour textures at their last use.
- **Performance window:** CPU zones per thread and GPU zones per pass on one timeline, 1024 frames of history, zone statistics, Chrome trace export.
- **Culling window:** enable, freeze, the red/green AABB overlay, counts per view (read back).
- **Camera window.**
- **Headless:**
  - the observation bus (JSONL)
  - `--dump-render-debug`, `--dump-profile`, `--screenshot`, `dump-texture`
  - session scripts driving the camera, views, culling, sampler, shadows and prim transforms

What is missing or got worse:

- **Per-draw GPU timing:** gone since indirect draws.
- **Buffer contents:** no view of any GPU buffer (instance records, draw commands, counts, material table, visibility, the geometry pool).
- **Frame-graph textures:** only the last use of a colour texture has a preview. Depth, float formats and intermediate states have none.
- **Culling:** visible only as counts and a camera-only overlay. There's no per-view or per-instance view of decisions, and nothing showing the compaction.
- **Hardware counters:** no pipeline statistics (vertex, fragment or compute invocations, clipping, primitives).
- **Memory:** no view of GPU memory (what is allocated, by whom, how much, which memory type).
- **Barriers:** the graph issues them, but nobody can see which barriers, between which states, with which derived stage and access masks.
- **Pipelines and shaders:** no view of pipeline state or descriptor contents, and no shader reflection.
- **Object names:** RHI objects have no debug names, so RenderDoc and validation messages show raw handles.
- **External tools:** no RenderDoc integration.

## Principles

Locked with the decisions below.

1. **Everything visible is also dumpable.** Every inspector has a UI view and a headless JSON dump or observation event, driven by the same data, with a
   session verb to trigger it. What the editor can show, an agent can read.
2. **One capture mechanism, many views.** The inspectors share one way to capture a frame-graph resource (texture or buffer) at a chosen point in the
   frame, and one way to read it back after the fence. Views add interpretation (a typed buffer schema, a colour map), not new readback paths.
3. **Explain, don't only show.** Where the engine derives something (barrier masks from states, a cascade from depth, a draw command from an instance),
   the inspector shows the inputs and the derived result side by side.
4. **Free when off, allowed to cost when on.** Nothing is recorded, copied or read back unless an inspector or dump asks for it. When one does, it may
   stall, copy whole buffers or keep history. Correctness first, cost second.
5. **Names everywhere.** Every GPU object has a debug name, used by the engine's own views, validation messages and external tools.

## Decisions

All five recommendations were confirmed and are locked: dump parity required, one generic capture service, both our own frame debugger and
RenderDoc, tooling compiled out of `gamerelease` only, separate windows with a shared selection. Scene and USD introspection waits (Deferred).

1. **Dump parity (principle 1): required for every stage, or UI-first with dumps where cheap?** I lean required. It's how every stage so far was
   verified, and it keeps agents able to check the tools themselves. Cost: each view needs a JSON writer and a verb, maybe 20% more work per view.
2. **Capture mechanism (principle 2).**
   - (a) A generic frame-graph capture service. "Capture resource R after pass P on frame N" inserts a copy into a readback buffer at that point
     (using the graph's barrier tracking) and hands the bytes to an interpreter after the fence. Textures and buffers both.
   - (b) Per-feature readbacks, as culling does today.

   I lean (a). Nearly every stage needs "show me this resource at that moment", and one mechanism keeps the barriers right in one place. The existing
   culling readback and frame-graph previews would move onto it. Cost: the graph needs capture points between passes, and the readback needs pooling.
3. **Our own frame debugger, RenderDoc, or both?**
   - (a) RenderDoc only: in-app capture through its API, plus debug names. It gives deep GPU state (every draw's inputs, shader debugging, texture
     history), but no engine semantics.
   - (b) Our own only: engine semantics (prim paths on draws, culling reasons, graph structure, GPU scene tables), but never RenderDoc's depth.
   - (c) Both.

   I lean (c). RenderDoc integration is small (load the library, trigger a capture from a menu or verb, name objects) and teaches the GPU side. Our
   own debugger teaches the engine side. They answer different questions.
4. **Where debug tooling lives in builds.** I lean compiled into `debug` and `release`, and compiled out of `gamerelease` behind one define. The cost
   is kept at zero by principle 4 rather than by `#ifdef` everywhere.
5. **Window structure.**
   - (a) Keep the existing windows and add new ones (Frame Debugger, Memory, GPU Scene), cross-linked: clicking a resource, pass, prim or draw anywhere
     opens it in the inspector that owns it.
   - (b) Merge everything into one "GPU Inspector" window with tabs.

   I lean (a), with a shared selection model: one current pass/resource/draw/instance selection that every window reads. That selection is what makes
   the cross-linking work.

## Stages

Each stage is its own plan, verified like the GPU-driven stages (headless evidence and dumps; a UI check where only the UI shows it).

| # | Plan (to write) | Delivers | Depends on |
|---|---|---|---|
| 1 | `plan_debug_names_and_memory.md` | RHI debug names on every object (`vkSetDebugUtilsObjectNameEXT`); an allocation registry (name, owner, size, memory type, lifetime); a Memory window and dump | none |
| 2 | `plan_frame_capture.md` | The capture service (decision 2): capture any graph texture or buffer after any pass, read back after the fence; typed buffer views with schemas (instance records, draw commands, counts, material table, mesh table, visibility); texture views for every format (depth, float, integer) with range and channel controls. Culling readback and frame-graph previews move onto it | 1 |
| 3 | `plan_frame_debugger.md` | A frame debugger over one captured frame: step pass by pass and see every attachment and storage resource before and after; the barriers each pass got (old and new state, derived stages, access and layout); descriptor sets bound per pass with their contents; push constants; the pass's draw commands with prim paths | 2 |
| 4 | `plan_gpu_scene_inspector.md` | The GPU scene tables as browsable data: every instance record, mesh entry, material and texture slot, decoded and linked to its prim. Culling per view: per-instance visibility per view, why an instance was culled (which plane), cascade frusta drawn in the viewport, instances coloured by the views that see them. The compaction made visible: per-workgroup counts, offsets, final command order | 2 |
| 5 | `plan_gpu_counters.md` | Pipeline statistics queries in the RHI (vertex and fragment invocations, clipping, primitives, compute invocations) per pass; GPU zones per indirect call (per bucket and view), replacing the lost per-draw timing at bucket granularity; history graphs of every counter | 1 |
| 6 | `plan_debug_views.md` | More viewport views: wireframe, overdraw heatmap, triangle density or size (the small-triangle view from `notes.md`), instance and mesh ID colours, UV and tangent checks, with a legend and value readout under the cursor | 2 for readout |
| 7 | `plan_shader_introspection.md` (deferred) | SPIR-V reflection per pipeline (bindings, push constants, inputs and outputs, local sizes), pipeline state tables, and shader hot reload with error display | none |
| 8 | `plan_renderdoc.md` | RenderDoc in-app API: capture a frame from a menu, key or session verb; open the capture; debug names make it readable (decision 3) | 1 |

Proposed order: 1, 8, 2, 3, 4, 5, 6, 7. Stage 7 was deferred after stage 6.

- Stage 1 comes first because names and a memory registry are small, feed every later stage, and make validation output readable.
- RenderDoc (stage 8) moves up because it is cheap and immediately useful once names exist.
- Stage 2 is the backbone for 3, 4 and 6.
- Shader introspection is independent and can move anywhere.

## Cross-cutting design

These are proposed details; each stage plan refines them.

- **Selection model.** A small `InspectorSelection` on the main thread: current pass, resource, draw, instance, prim. The windows read and write it; the
  render thread gets capture requests derived from it.
- **Capture requests** travel like texture inspect requests do today (main to render thread, latest-only), carrying frame, pass, resource and view.
  Results come back with the debug snapshots.
- **Schemas for buffers.** Each GPU struct that crosses to the GPU (`GpuInstanceRecord`, `GpuMeshEntry`, `GpuMaterial`,
  `RhiDrawIndexedIndirectCommand`, `CullParams`, the counter layout) gets a field schema next to its definition: name, offset, type. The typed buffer
  view and the JSON dump both read it, so a layout change updates the tools.
- **Dumps.** Each stage adds `--dump-<thing>=PATH` or a session verb (`capture`, `dump-buffer`, `dump-memory`, ...) that writes the same data the view
  shows.
- **Documentation.** `src/renderer/README.md` and `src/rhi/README.md` get a "Debugging and introspection" section as the stages land.

## Verification (end state)

- Every inspector listed in Stages has a dump, and a script can produce every dump headless.
- A captured Sponza frame answers, from dumps alone:
  - which barriers each pass received
  - what each draw command drew (prim, mesh, material, index range)
  - which instances each cascade culled and why
  - how much GPU memory each table uses
  - how many fragment-shader invocations the geometry pass ran
- RenderDoc opens an in-app capture with every object named.
- Every tool is free when off: with no inspector open and no dump requested, GPU and CPU frame time are within noise of today's.

## Deferred / follow-ups

- **Shader introspection (stage 7).** SPIR-V reflection, pipeline state tables and hot reload. Deferred until the asset pack system exists: shaders
  are build outputs today (`glslc` in the build's `shaders` tool), and hot reload and reflection belong with how packed assets are cooked, loaded and
  reloaded. Trigger: the asset pack system lands (`notes.md`).

- **Tracy integration.** The profiler macros already allow it. Trigger: the built-in Performance window stops being enough, for example lock
  contention or memory tracking on the CPU.
- **Scene and USD introspection** (composed attribute provenance, layer opinions per attribute). A separate plan when the scene side is the thing to
  learn.
- **GPU crash diagnostics** (breadcrumbs, `VK_EXT_device_fault`). Trigger: the first device loss that is hard to reproduce.

## Results

Each stage plan records its own results. Deviations from this umbrella:

- Stage 2: the culling readback and the frame-graph previews stay on their own paths instead of moving onto the capture service
  ([plan_frame_capture.md](plan_frame_capture.md), Decisions).
- Stage 3: before/after images in the Frame Debugger come from live captures on later frames, labelled with their frame; `dump-frame` captures every
  written resource on the captured frame itself.
- Stage 5: pipeline statistics are per pass; per indirect call there are GPU time zones only (queries don't nest).
- Stage 6: wireframe through a geometry shader instead of line fill mode; tangent checks wait for tangents in the vertex format.
- Stage 8: `renderdoc_app.h` is a single vendored file, not a submodule.
- Decision 4 is only partly done: `NGEN_INTROSPECTION` (defined in `debug` and `release`) guards the RenderDoc integration only. The capture service,
  command log, counters, debug views and windows compile into `gamerelease` too. They cost nothing while unused, but they are not compiled out.
- The shared selection model is not a separate `InspectorSelection`: windows cross-link through callbacks (a GPU Scene row selects the prim, the Frame
  Debugger and Frame Graph windows open a resource in the Capture window).
- End-state criterion "every tool is free when off" was not measured. What stays on when every tool is off: the barrier records per pass, the
  `cullPlanes` store per instance, and the per-region GPU zones (a few timestamp pairs).
