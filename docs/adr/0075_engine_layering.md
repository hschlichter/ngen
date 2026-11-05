# 75. Engine layering, the tools apart from the UI

Date: 2026-10-31

## Status

Accepted

## Context

Dependencies ran both ways. The tools' data types were defined in UI window headers and built in `src/view`; `viewcommands.cpp` was a 2,922-line
catch-all; the renderer linked ImGui and held the editor gizmos, one of them holding the main thread's `Camera*` from the render thread. Neither the
UI nor the tools could be cut from the engine, and nothing showed what the engine is without them.

## Decision

Locked by Henrik on 2026-10-31:

- Five layers, nothing including upward: Foundation (`profile`, `trace`, `config`, `rpccore`) → Engine (`world`, `renderer`, the view runtime) →
  Instrumentation (records, methods and verbs, by domain) → Tools → UI.
- Built as libraries, the layering being the code's structure: `view`, `viewcommands` (split by domain), `rendertools`, `tools` (`src/tools/`) and
  `ui`, each linking only the layers below it.
- The tools' state is plain structs the application owns. Windows read them and write requests; the tools' hooks apply the requests.
- `src/tools` never includes `src/ui`.
- No "common" bucket: the camera, the projection and debug drawing moved to their owners.
- Commands are registered by the layer that owns them, with verbs per layer.

Rejected: a `common` library (tried and dropped: ownership over buckets), and keeping the tools' data in the windows.

## Consequences

- Easier: the engine runs without tools or UI hooks ([ADR 76](0076_viewframe_steps_and_hooks.md)), the tools build without the UI, and the
  renderer includes neither ImGui nor a tool ([ADR 78](0078_render_extensions.md)).
- Easier: `src/apps/view.cpp` went from 1,481 lines to 590.
- Harder: each library's README and `AGENTS.md` carry the rule; a change that reaches upward is caught by its library's links and in review.

## Verification

- Sources: [plan_lean_pass.md](../plan_lean_pass.md), `src/tools/README.md`, `src/rendertools/README.md`, `AGENTS.md` ("Folder structure").
- Code: `build.cpp:183` (`renderer`), `:212` (`rendertools`), `:229` (`view`), `:259` (`viewcommands`), `:291` (`tools`), `:323` (`ui`); no file in
  `src/view`, `src/renderer`, `src/world` or `src/tools` includes a UI header, and nothing in `src/renderer` includes ImGui or a tool.
- Built in steps 0 to 7 of the plan, 2026-10-31 to 2026-11-05. Step 7 found the per-frame readback cost the plan put on the visibility bits was
  mostly `Renderer::cullResult` summing every slot's triangles each frame; the renderer now keeps `sceneTriangles` as slots change.
