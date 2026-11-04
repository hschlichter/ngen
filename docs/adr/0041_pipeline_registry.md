# 41. A pipeline registry: nothing waits for a shader

Date: 2026-09-29

## Status

Accepted

Amended by [ADR 54](0054_async_scene_and_incremental_gpu_scene.md): no start-up shaders — pipelines requested at start are no longer waited for.

## Context

Shaders became packed assets, streamed from the asset server ([ADR 42](0042_separate_asset_server.md)) instead of SPIR-V files next to the
binary. Passes created their shader modules and pipelines once, in `init()`, so a pass could not exist before its shaders had arrived, and nothing
could be replaced while the view ran. A shader that failed to compile stopped the view.

## Decision

- `PipelineRegistry`, in the renderer, is the only code that creates pipelines. A pass registers a request: the RHI pipeline description with
  `ShaderRef{id, stage}` where the modules would be, and gets back a stable `PipelineHandle`.
- Shader modules are shared per shader id.
- `update()` runs on the render thread before the frame graph is built. It polls `AssetClient` for arrived shaders and creates at most four
  pipelines per frame.
- A pipeline that is pending or failed means the pass using it is left out of the graph. Until the lit path is ready, a frame is
  `ClearBackbuffer`, the overlays that are ready, and the UI.
- A failed shader is reported in the trace and the pipeline's info; the view goes on.
- Requests are `AtStart` (sent at start-up in one request) or `FirstUse` (the debug views, requested when first shown).
- Hot reload was deferred, then landed with [ADR 57](0057_asset_change_notices.md): `reload(ids)` requests the changed shaders again and swaps
  pipelines when they arrive.

## Consequences

- Easier: the first frame is drawn at once; nothing stalls on a shader, at start or on reload.
- Easier: shader hot reload with no special path.
- Harder: early frames are blank until the lit path's pipelines exist.
- Harder: there is no SPIR-V reflection, so a reload that changes bindings is not detected.
- Harder: no pipeline cache on disk, and creation stays on the render thread.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_async_shaders.md](../plan_async_shaders.md), [plan_async_scene.md](../plan_async_scene.md) (decision 10),
  [src/renderer/README.md](../../src/renderer/README.md) ("Pipelines", "Hot reload").
- Code: `src/renderer/pipelineregistry.h:38` (`AtStart`, `FirstUse`), `src/renderer/pipelineregistry.h:89` (`init` takes the `AssetClient`),
  `src/renderer/pipelineregistry.h:113` (`update(frame, budget = 4)`), `src/renderer/renderer.cpp:432` (`ClearBackbuffer`).
- Differs from plan: the plan waited for the start-up pipelines before the first frame (its decision 6); that wait was removed by
  [ADR 54](0054_async_scene_and_incremental_gpu_scene.md), and `Startup` became `AtStart`.
- Rejected alternatives: an RHI call that rebuilds a pipeline in place (the RHI would keep engine state); a push queue from `AssetClient`
  (polling instead); fallback shaders (passes are skipped instead).
