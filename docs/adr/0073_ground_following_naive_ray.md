# 73. Ground following with a naive ray; collision waits for physics

Date: 2026-10-31

## Status

Accepted

## Context

Walking at eye height through the test world was wanted before any physics or collision exists. The roadmap has a physics system; the project's
stance is not to build stopgaps for what a roadmap system will solve, but to use the structures that already exist and say what is missing.

## Decision

- The ground is every drawn mesh the main thread holds (kept from `SceneChanges` as they pass to the renderer).
- The eye is kept 1.8 metres (by default) above the highest hit below it, set each frame with no smoothing.
- The hit is found by brute force: a ray down from the height above the eye, through the draws whose bounds hold the camera, every triangle of
  those tested (Möller–Trumbore). There is no acceleration structure; chunks of draws are skipped by the bounds the shadow cascades already keep.
- Its known limits are shown, not filtered: grass and roofs lift the camera, and every render-LOD level counts as ground.
- Orbiting pivots on the depth-buffer point under the cursor while Left Alt is held.

Rejected: authored collision, or a filtered ground — both deferred to physics.

## Consequences

- Easier: works on any scene with no authoring.
- Easier: the `status` record shows the hit, its prim and what was tested, so the cost is visible.
- Harder: wrong on ground cover and overlapping LOD levels.
- Harder: the cost grows with the number of triangles under the camera.

## Verification

- Sources: [plan_camera_modes.md](../plan_camera_modes.md).
- Code: `src/view/ground.cpp:63` (`groundBelow`); `src/view/README.md:238-252` (camera modes, "Ground from collision geometry waits for physics").
