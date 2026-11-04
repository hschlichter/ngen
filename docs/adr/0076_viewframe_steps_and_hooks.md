# 76. The frame loop as `ViewFrame` steps and `ViewHooks`

Date: 2026-11-01

## Status

Accepted

## Context

ngen-view's frame loop was about 880 lines in `src/apps/view.cpp`. It mixed the engine's steps with the tools' and the UI's work: window lists,
`LodWindowSync`, `StreamingWindowSync`, gizmos and labels. Per-frame tool work had no defined place, so it went wherever it was convenient, and the
engine's own order of work could not be read from the loop. The layering ([ADR 75](0075_engine_layering.md)) needed the engine's frame to run without
tools or UI.

## Decision

- `ViewFrame`, in the `view` library, holds the frame's engine state and has one method per step: `updateScene`, `advanceTime`, `resetCameras`,
  `setViewport`, `sendSceneInputs`, `updateResidency`, `moveCamera`, `cull`, `fitCascades`, `readBack`, `buildSnapshot`.
- `ViewHooks` is an interface with no-op virtual hooks at fixed points: `afterScene`, `cameras`, `event`, `afterCamera`, `debugDraw`,
  `wantReadbacks`, `takeReadbacks`, `drawUi`, `fillSnapshot`.
- `ToolsHooks` implements the tools' part; `UiHooks` wraps it, so the loop holds one hook object.
- `FrameCameras` names which camera each use takes; the debug camera takes them over once a frame, after the commands.
- The status bar's gathering moved into the UI (`StatusBarGatherer`).

The plan first proposed one tools hook called at fixed points; Henrik chose the steps in the view library, with the hooks an interface that both the
tools and the UI implement.

## Consequences

- Easier: without hooks, the frame is the engine's alone; the loop in `view.cpp` went from about 880 lines to 47.
- Easier: where per-frame tool work happens is fixed and named.
- Harder: three timing differences came with it — a UI switch to the debug camera shows from the next frame, a gizmo drag takes mouse motion before
  mouse look, and the first frame's time is zero.
- Harder: the `Camera` and `Cull` profiler zones replaced `CameraAndCull`, so older captures don't line up.

## Verification

- Sources: [plan_lean_pass.md](../plan_lean_pass.md) (step 5), `src/view/README.md`, `src/tools/README.md`,
  [plan_status_bar.md](../plan_status_bar.md) (decision 5).
- Code: `src/view/viewframe.h:84-126` (the steps); `src/view/viewhooks.h:15-30` (the hooks); `src/tools/toolshooks.h:18` (`ToolsHooks`);
  `src/ui/uihooks.h:18` (`UiHooks`); `src/ui/statusbar.h:37` (`StatusBarGatherer`).
- Differs from plan: plan_status_bar decision 5 had the view assemble `StatusBarData`; the UI gathers it now.
