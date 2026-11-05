# 78. Render extensions: what isn't the renderer's core joins its frame

Date: 2026-11-05

## Status

Accepted

## Context

The renderer carried the tools and the UI in its core: it linked ImGui, drew the UI pass and the gizmo passes, owned the corner axis gizmo (its
hover written on the render thread, hit-tested from the main thread), and held the instrumentation in `Renderer` and `RenderThread`: the frame
graph and render debug snapshots, the frame debugger, GPU counters, the texture inspector and dumps, screenshots. Each came with its own setter
or channel on the render thread. The layering ([ADR 75](0075_engine_layering.md)) needed the renderer free of all of them, while the
instrumentation stays in every program the records serve.

## Decision

- `RenderExtension` (`src/renderer/renderextension.h`) is an interface with no-op hooks at fixed points of `Renderer::render`: `init`,
  `beginFrame` (first in every frame, recording or not), `afterFence`, `addOverlayPasses`, `beforeExecute`, `afterExecute`, `afterSubmit`,
  `endFrame`, `destroy`. Extensions are added before `init` and called in the order added.
- An extension's per-frame data rides in the snapshot: `RenderSnapshot::extensions`, one `RenderExtensionData` per extension at its index, made by
  its main-thread side. The capture watches go with the snapshot too.
- Split by layer (Henrik): the instrumentation is `rendertools` (`src/rendertools/`, no ImGui); the gizmos and their pass are the tools'
  (`GizmoRender`, the axis gizmo drawn and hit-tested on the main thread); ImGui's backend and pass are the UI's (`UiRender`).
- The capture service stays in the core, since picking reads through it ([ADR 31](0031_introspection_principles.md): one capture service). Its
  previews, and the other inspection textures, are shown through `PreviewTextures`, which the UI implements.
- `Renderer::state()` gives the instrumentation read access to what the renderer holds and last did.

Rejected: one `renderertools` library holding the ImGui parts too (a program without the UI would lose the records it serves), and a channel per
extension beside the snapshot (each extension would have to keep its data in step with the frame itself).

## Consequences

- Easier: the renderer includes no ImGui and no tool; a new inspector or overlay joins at a hook instead of changing `Renderer`.
- Easier: a frame's requests and data arrive with that frame; a screenshot is of the frame that asked for it, and a frame debug capture describes
  the watches sent with it.
- Harder: the snapshot carries type-erased payloads, read back by the extension that made them.
- Harder: the instrumentation reads the renderer through `state()`, which grows with what it needs.
- A pick reaches the render thread with its snapshot, at most a frame later than before; counters switched on start a frame later.

## Verification

- Sources: [plan_lean_pass.md](../plan_lean_pass.md) (step 6), `src/renderer/README.md` ("Extensions"), `src/rendertools/README.md`.
- Code: `src/renderer/renderextension.h` (`RenderExtension`); `src/renderer/rendersnapshot.h` (`extensions`, `captureWatches`);
  `src/renderer/previewtextures.h`; `src/rendertools/rendertools.h` (`RenderTools`); `src/tools/gizmorender.h` (`GizmoRender`);
  `src/ui/uirender.h` (`UiRender`); `src/apps/view.cpp` (the three added in order).
- Differs from plan: the plan named one `renderertools` library for all of it; the instrumentation alone became `rendertools`.
