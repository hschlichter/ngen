# Frame budget overlay: the frame graph's passes as bars

**Status. Landed.**

## Current state

The profiler already times every frame graph pass on the GPU: a pass is a top-level GPU zone whose site carries the pass's group (Shadow,
Geometry, Lighting, Post, Culling, Ui, …), registered by `FrameGraph::addPass` ([plan_profile_detail.md](plan_profile_detail.md)). The
`zones` record gives each pass's milliseconds per frame over the last 60 frames, and the Performance window draws the passes on its GPU lane.
Both are aimed at engine developers: a timeline of every thread, named for the code. Content and art developers have no simple answer to
"what does what I made cost, and is the frame within budget?".

## Goals

1. A small overlay in the viewport that shows, while flying the scene, how the frame's GPU time splits over the kinds of work (shadows,
   geometry, lighting, post-processing, UI), measured against a frame budget, so the cost of content is seen as it changes.
2. Plain words and a budget, not a timeline: the Performance window stays the engine developers' tool.

## Decisions

Locked with Henrik on 2026-11-11.

1. **A viewport overlay**, not a window: a translucent panel in the viewport's top-right corner, no title bar (draggable: see Built).
   Toggled from the View menu, with Ctrl+B, and by a config value, like the other overlays.
2. **Bars against a frame budget.** A bar's length is milliseconds against the budget (`profile.budget.frameMs`, default 16.7 ms: 60 frames a
   second). The header shows the GPU frame and the whole frame (the main thread's interval) against the budget, green under 80%, yellow up to
   it, red over (more than 5% over: see Built).
3. **By group, passes beneath.** One bar per group in plain words (Shadows, Geometry, Lighting, Post-processing, …), in the order the frame
   runs them, coloured by the group's colour; each group's frame graph passes listed under it in smaller rows.
4. **One totals line** for the content numbers: the frame's draws and triangles (`SceneGpuStats::frameCommands`, already published every
   frame). Per-pass draws and triangles are deferred.

## Scope

**In**

- `src/profile`: the passes' times per frame over the last N frames, the same numbers as the `zones` record's GPU lane, as a function the overlay
  and any other reader use; the frame budget config value.
- `src/ui`: the overlay, its toggle (View menu, Ctrl+B, config value).

**Out**

- Per-pass draws and triangles (Deferred).
- CPU work per group (the scene, streaming, the UI on the main thread): the header's frame time shows when the CPU is the limit; the
  Performance window shows where.
- Changes to the Performance window.

## Steps

1. **Pass times.** `src/profile/profile.h`, beside `gpuZonesIn`:

   ```cpp
   // The GPU's top-level zones (the frame graph's passes) over the last `frames` main-thread frames: each site's milliseconds per frame,
   // ordered by when it first ran, with the frames' average interval and GPU frame time. The zones record's GPU lane over the same frames.
   struct GpuPassTime {
       uint32_t siteId = 0;
       double msPerFrame = 0.0;
   };
   struct GpuPassTimes {
       uint32_t frames = 0;
       double frameMs = 0.0; // the main thread's frame interval
       double gpuFrameMs = 0.0;
       std::vector<GpuPassTime> passes;
   };
   auto gpuPassTimes(uint32_t frames, GpuPassTimes& out) -> void;
   ```

   The window is the one the `zones` record uses (`frameAt` over the last `frames` intervals), so the overlay and the record agree.

2. **Budget.** `profileconfig::frameBudgetMs`, `profile.budget.frameMs` (`config::Float`, default 16.667, 1–1000): "The frame time content is
   measured against; 16.7 ms is 60 frames a second."

3. **Overlay.** `src/ui/budgetoverlay.h/.cpp`:

   ```cpp
   // The frame budget overlay: the GPU's passes by group as bars against the frame budget, for content developers. Pinned in the viewport's
   // top-right corner; reads the profiler and the last GPU scene stats.
   auto drawBudgetOverlay(const SceneGpuStats& gpu) -> void;
   ```

   - Once a frame: `profile::gpuPassTimes(60, …)`; passes summed by `profile::groupOf(siteId)` into groups in first-run order.
   - Header: `GPU 11.2 ms · frame 16.9 ms of 16.7`; a bar for the GPU frame in the budget colour.
   - A row per group: its plain name, a bar in `profile::groupColor`, milliseconds; its passes beneath, indented, smaller bars.
   - Footer: draws and triangles.
   - An ImGui window with `NoDecoration | NoInputs | NoNav | NoFocusOnAppearing | NoSavedSettings | AlwaysAutoResize`, background alpha 0.6,
     positioned under the menu bar at the right edge.
   - Group names: Shadow → Shadows, Geometry → Geometry, Lighting → Lighting, Post → Post-processing, Culling → Culling, Ui → UI,
     Tools → Editor tools, Upload → Uploads, Render → Other rendering; the rest by `groupName`.

4. **Toggle.** `uiconfig::budgetOverlay`, `ui.overlay.budget` (`config::Bool`, default off) in `src/ui`; a View menu item "Frame Budget"
   with Ctrl+B through `configMenuItem`; `ViewUI::toggleShortcuts` flips it.

5. **Docs.** `src/profile/README.md` (`gpuPassTimes`, the budget value), `src/ui` window list in the README that lists the windows.

## Built

Where the code differs from the steps above:

- **The overlay sits under the corner axis gizmo**, not in the corner itself, which the gizmo takes: `Axis3DGizmo::gizmoSize` and `margin`
  are public now so the overlay places itself by them.
- **The passes are in recording order** (the frame graph's), not ordered by time: the newest GPU frame gives the order, and a pass that ran
  only in an older frame of the window goes right after the pass recorded before it there. Ordering by start time first put
  post-processing first (the window starts partway through a frame); ordering by each pass's latest start then made the bars jump in the
  window (Henrik, 2026-11-11): zero-length passes (PresentPass, GizmoPass) tie with their neighbours, and an unordered map broke the ties.
  Checked: three consecutive frames while the depth prepass was switched off give identical label columns; while the window mixes frames with
  and without a pass, the passes interleave as recorded, for at most 60 frames.
- **`gpuPassTimes` walks the GPU frames from the newest back to the window's start**, not through `gpuZonesIn`, which scans the whole
  history (about 2,400 frames at 10 seconds): the overlay cost 0.13–0.23 ms a frame that way, 0.02–0.03 ms now.
- **The label column fits the longest label**: pass names such as `InstanceCullLateScatter` ran into the bars.
- **A pre-existing race in the ImGui backend, fixed here** (decided with Henrik on 2026-11-11): the overlay's labels lost letters ("hadows",
  "Geometr"), the same ones every run. ImGui's font atlas gains glyphs on the main thread as new text is drawn, and the render thread uploaded
  the atlas's changes and then marked them done; glyphs added while it uploaded were marked done with them and never drawn. Any window opened
  mid-run could show it. `ImGuiBackendVulkan` now holds a lock from `beginFrame` to `endFrame` (while ImGui may add glyphs), and the render
  thread takes it to upload a frame's texture changes itself, before drawing, only for a frame that had some
  (`ImGuiFrameSnapshot::texturesChanged`); ImGui's own draw call gets no texture list.

- **Over budget means more than 5% over**: locked to 60 Hz, the whole frame measured 16.6 and 16.7 ms in turn against the 16.7 ms budget, and
  its bar blinked between yellow and red (Henrik, 2026-11-11). Within 5% over is yellow, and the red cap starts past it.
- **Draggable** (Henrik, 2026-11-11): it starts under the axis gizmo and is dragged anywhere on it; ImGui keeps where it was put in
  `imgui.ini`, as for the windows. Over the overlay, clicks go to it, not the scene.
- **Counts with K and M** ("110.3K draws, 5.8M triangles"), not words.

## Visibility

- **Records**: none new. The bars are the `zones` record's GPU lane (`perFrameMs` of the depth-0 GPU zones, each with its `group`) over the
  same 60 frames; the totals line is `gpuscene`'s `frameCommands`; the budget is in `config`.
- **RPC methods and verbs**: `config ui.overlay.budget on` shows it, `config profile.budget.frameMs 33.3` sets a 30 fps budget.
- **Windows**: the overlay itself; the Configuration window lists both values.

## Verification

Results (2026-11-11, this machine) in **bold**.

- Headless, release, Sponza, `--show-ui`, a script that turns the overlay on, records `zones` and `gpuscene` at frame 600 and takes a
  screenshot: the screenshot shows the overlay in the top-right corner with a bar per group and the passes under it. **Shown under the axis
  gizmo: Culling, Shadows, Geometry, Lighting, Post-processing, Editor tools, UI in frame order, each with its passes, every letter drawn.**
- Each pass's milliseconds in the overlay equals the `zones` record's `perFrameMs` for that GPU site at the same frame (±0.05 ms: the record
  and the overlay read one frame apart). Checked by `gpuPassTimes` and the record over the same frames. **Equal: GeometryPass 1.2 (record
  1.174–1.185), ShadowPass 1.1 (1.074–1.100), AAPass 0.42–0.43 (0.414–0.430), LightingPass 0.25 (0.252), HzbBuild 0.29 (0.286–0.290);
  the footer's 619 draws and 5.8 million triangles are `gpuscene`'s `frameCommands` (619, 5,818,853).**
- `config profile.budget.frameMs 8`: the header turns red and the bars lengthen (screenshot). **Run at 3 ms: header "Frame budget 3.0 ms (333
  fps)", the whole frame and GPU bars red and capped, the group bars four to six times longer.**
- Off by default: no overlay in a default screenshot; `BudgetOverlay` zone absent from the `zones` record. **Turned off at frame 730: no
  `BudgetOverlay` zone in the record at frame 830.**
- Cost: the overlay's zone under 0.05 ms a frame. **Release: 0.024 ms a frame (median 0.020 ms); debug 0.089 ms median.
  `ImGuiTextureUpload` absent at steady state (no new glyphs).**
- Trace: no warnings or errors. **Release Sponza and debug three_cubes (`--fail-on-validation`, the overlay turned off and on): no warnings,
  errors or validation messages.**

## Deferred / follow-ups

- **Per-pass draws and triangles.** Trigger: content developers ask which pass their meshes cost in, not only how long it takes.
- **CPU groups** (scene, streaming, UI on the main thread) as bars. Trigger: content that costs mostly CPU (many instances, streaming).
- **The budget line in the Performance window's frame plot.**
