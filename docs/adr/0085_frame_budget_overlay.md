# 85. A frame budget overlay from the profiler's numbers; ImGui textures uploaded under a lock

Date: 2026-11-11

## Status

Accepted

## Context

The profiler times every frame graph pass on the GPU ([ADR 82](0082_detailed_profiling.md)), and the Performance window and the `zones` record
show them. Both are for engine developers: a timeline of every thread, named for the code. Content and art developers had no simple answer to
"what does what I made cost, and is the frame within budget?".

Building the overlay exposed an existing race in the ImGui backend. ImGui's font atlas gains glyphs on the main thread as new text is drawn,
and the render thread uploaded the atlas's changes and then marked them done; glyphs added while it uploaded were marked done with them and
never drawn ("hadows", "Geometr").

## Decision

Locked with Henrik on 2026-11-11:

- **A viewport overlay, not a window**: translucent, no title bar, draggable, toggled from the View menu, with Ctrl+B and by
  `ui.overlay.budget`.
- **Bars against a frame budget** (`profile.budget.frameMs`, default 16.7 ms). The whole frame and the GPU frame are green under 80% of the
  budget, yellow up to 5% over it, red past that: a frame locked to the display's rate sits right on a budget of that rate.
- **By group in plain words** (Shadows, Geometry, Lighting, Post-processing, …), in the order the frame records them, each group's passes
  beneath it; one totals line of the frame's draws and triangles.
- **The profiler's numbers, not new ones**: `profile::gpuPassTimes` gives each pass's milliseconds per frame over the `zones` record's
  window (60 frames), so the overlay and the record agree. No new record: the bars are the `zones` record's GPU lane.
- **ImGui's texture changes are uploaded under a lock**: `ImGuiBackendVulkan` holds a lock from `beginFrame` to `endFrame`, while ImGui may
  add glyphs, and the render thread takes it to upload a frame's texture changes itself, before drawing, only for a frame that had some.
  ImGui's own draw call gets no texture list.

Rejected: a dockable window (the overlay is seen while flying the scene); a timeline (the Performance window's job); per-pass draws and
triangles (deferred until content developers ask); ordering passes by start time (the window starts partway through a frame) or by latest
start (zero-length passes tie with their neighbours, and the bars jumped); uploading the atlas on the main thread (the render thread owns the
device's command buffers).

## Consequences

- Easier: content developers see the cost of what they make against a budget while flying the scene, in words that are theirs.
- Easier: any ImGui window opened mid-run draws every glyph.
- Harder: the render thread can wait for the main thread's ImGui frame when a frame added glyphs; at steady state there are none
  (`ImGuiTextureUpload` absent).
- Harder: CPU work isn't split by group; the header's whole frame shows when the CPU is the limit, and the Performance window shows where.

## Verification

- Sources: [plan_pass_budget_overlay.md](../plan_pass_budget_overlay.md), `src/profile/README.md` ("At runtime"), `src/renderer/README.md`
  (the ImGui texture lock).
- Code:
  - `src/profile/profile.h:245` (`GpuPassTime`, `GpuPassTimes`), `:255` (`gpuPassTimes`); `src/profile/profile.cpp:959`;
    `src/profile/profileconfig.cpp:21` (`frameBudgetMs`).
  - `src/ui/budgetoverlay.h:11` (`drawBudgetOverlay`); `src/ui/budgetoverlay.cpp:64` (the 5% tolerance), `:192` (`ui.overlay.budget`);
    `src/ui/uihooks.cpp:151`.
  - `src/imguibackendvulkan.h:35` (`textureMutex`); `src/imguibackendvulkan.cpp:70` (locked in `beginFrame`), `:80` (unlocked in
    `endFrame`), `:88` (the upload); `src/ui/imguibackend.h:28` (`texturesChanged`).
- Differs from plan: under the corner axis gizmo rather than in the corner; draggable rather than fixed; passes in recording order; the label
  column fits the longest label; over budget is more than 5% over; counts as K and M; the ImGui race fixed here.
