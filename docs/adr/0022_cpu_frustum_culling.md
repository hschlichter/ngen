# 22. CPU frustum culling on the main thread

Date: 2026-09-20

## Status

Superseded by [ADR 29](0029_gpu_culling_ordered_compaction.md)

## Context

Sponza drew all 407 of its instances every frame, whether in view or not. The main thread already owned the camera, every instance's bounding box
and the debug overlay, and 407 box tests cost microseconds. There was no culling of any kind, and no way to see what a culling step would remove.

## Decision

The main thread culls per instance while it builds the render snapshot:

- A six-plane frustum (Gribb-Hartmann extraction) and a positive-vertex test against each instance's world bounding box.
- The result travels in the snapshot as a `visible` byte per instance; the renderer skips invisible instances in its draw loop.
- Freezing the frustum captures a separate cull matrix, so the camera can fly out and see what was culled, drawn as a debug overlay.
- Shadow cascades later got masks of their own (`shadowVisible`), tested against each cascade's box.

Culling inside `Renderer::render` was rejected: it would have needed the boxes copied to the render thread and a result path back to the editor.

## Consequences

- Easier: 234 of Sponza's 407 instances culled from the camera view with byte-identical images.
- Easier: the frozen-frustum overlay made culling visible; it survives in the GPU culling.
- Harder: the gain was small, because Sponza's meshes are building-sized and rarely fully out of view.
- Harder: the per-instance CPU draw loop stayed, and with it draw cost that scaled with instance count. GPU-driven rendering (ADR 26) and GPU
  culling (ADR 29) removed both: `cullInstances`, `cullShadowCascades` and the snapshot's `visible`/`shadowVisible` were deleted.

## Verification

- Sources: [plan_frustum_culling.md](../plan_frustum_culling.md), [plan_shadow_cascades.md](../plan_shadow_cascades.md),
  [plan_gpu_culling.md](../plan_gpu_culling.md).
- Code: `rg "cullInstances|cullShadowCascades|shadowVisible" src` finds nothing. `src/renderer/culling.h:15` keeps `CullState` — the toggles and the
  frozen matrix — with the comment "The culling itself runs on the GPU (InstanceCullPass)".
