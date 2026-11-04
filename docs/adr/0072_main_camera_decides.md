# 72. The main camera decides; the debug camera observes

Date: 2026-10-31

## Status

Accepted

## Context

Residency, LOD and culling are all decided from the camera, so their effects behind or far from the player can't be seen from the player's eye.
Working on streaming needed a way to watch residency from outside without changing it.

The plan first decided (decision 1 A) to cull from the camera rendered from. That was reversed the same day: the main camera drives LOD and
streaming, culling and shadows by default, each switchable on its own.

## Decision

- A second, debug camera to render from. The main camera stays the player: every verb that places the camera (`camera`, `fly`, `--camera`, scene
  cameras, bookmarks) moves it.
- LOD, streaming and render LOD share one switch, as one answer to "where is the viewer"; culling and shadows have one switch each. All three are on
  the main camera by default (`debug-camera lod|cull|shadows main|debug`).
- The main camera's frustum, a body at its eye and its trail are drawn in the debug view.
- C hands the controls to the main camera while the view stays on the debug camera.
- One viewport.

Rejected: picture-in-picture (deferred; the renderer draws one view per frame).

## Consequences

- Easier: residency, LOD and culling can be inspected from outside without changing them.
- Easier: the LOD and Streaming inspectors and the Streaming map measure from the same camera as the engine (`lodCamera`).
- Harder: two cameras to keep straight in every tool; picking, gizmos and labels go through the camera rendered from.

## Verification

- Sources: [plan_debug_camera.md](../plan_debug_camera.md) (Results, "Changed 2026-10-31").
- Code: `src/tools/debugcamera.cpp:30` (`lodCamera`); `src/view/README.md:224-237`; the switches are configuration values
  `tools.debugcamera.lod`, `.cull`, `.shadows`.
- Differs from plan: the plan's Results place the state in `src/ui/camerawindow.h` and `src/view/debugcamera.*`; it now lives in
  `src/tools/debugcamera.*` as `DebugCameraState` in `ToolsState`.
