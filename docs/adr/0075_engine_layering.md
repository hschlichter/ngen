# 75. Engine layering, the tools apart from the UI

Date: 2026-10-31

## Status

Accepted

Partly built: steps 0 to 5 and 7 of the lean pass are done; the build does not enforce the layers yet (see Verification).

## Context

Dependencies ran both ways. The tools' data types were defined in UI window headers and built in `src/view`; `viewcommands.cpp` was a 2,922-line
catch-all; the renderer linked ImGui and held the editor gizmos, one of them holding the main thread's `Camera*` from the render thread. Neither the
UI nor the tools could be cut from the engine, and nothing showed what the engine is without them.

## Decision

Locked by Henrik on 2026-10-31:

- Five layers, nothing including upward: Foundation (`profile`, `trace`, `config`, `rpccore`) → Engine (`world`, `renderer`, the view runtime) →
  Instrumentation (records, methods and verbs, by domain) → Tools → UI.
- Built as libraries: `view`, `viewcommands` (split by domain), `tools` (`src/tools/`) and `ui`.
- The tools' state is plain structs the application owns. Windows read them and write requests; the tools' hooks apply the requests.
- `src/tools` never includes `src/ui`.
- No "common" bucket: the camera, the projection and debug drawing moved to their owners.
- Commands are registered by the layer that owns them, with verbs per layer.
- An `ngen-view-core` target (engine plus instrumentation, without tools and UI) is to prove the layering in the build.

Rejected: a `common` library (tried and dropped: ownership over buckets), and keeping the tools' data in the windows.

## Consequences

- Easier: the engine runs without tools or UI hooks ([ADR 76](0076_viewframe_steps_and_hooks.md)), and the tools build without the UI.
- Easier: `src/apps/view.cpp` went from 1,481 lines to 566.
- Harder: until `ngen-view-core` exists, the layering is kept by convention and can erode.
- Harder: the renderer is still not free of ImGui or the gizmos.

## Verification

- Sources: [plan_lean_pass.md](../plan_lean_pass.md), `src/tools/README.md`, `AGENTS.md` ("Folder structure").
- Code: `build.cpp:213` (`view`), `:242` (`viewcommands`), `:273` (`tools`), `:305` (`ui`); no file in `src/view`, `src/renderer`, `src/world` or
  `src/tools` includes a UI window header.
- Differs from plan (steps not done):
  - Step 6: the renderer still includes `imguibackend.h` (`renderer.cpp`, `rendersnapshot.h`, `capture.cpp`, `framegraphpreviews.cpp`,
    `passes/editoruipass.cpp`), and the gizmos still live in `src/renderer/` (included from `src/tools/toolshooks.h:3-6`).
  - Step 8: there is no `ngen-view-core` target.
- Step 7 (2026-11-04): the per-instance visibility bits are read back only for a frame whose snapshot asks (`RenderSnapshot::readVisibility`, set
  by `ToolsHooks::fillSnapshot` while the culled overlay is on); `cameraVisible` is gone. Most of the 15 ms the plan put on that readback was
  `Renderer::cullResult` summing every slot's triangles each frame; the renderer now keeps `sceneTriangles` as slots change.
