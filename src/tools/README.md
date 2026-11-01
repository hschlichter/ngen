# Tools

The `tools` library: what the engine's tools know and do, apart from the UI that shows them. A tool builds its data from the engine (the
view's scene, the renderer's readbacks), draws its viewport layers with `DebugDraw`, and keeps its state in a plain struct the application
owns. The UI's windows (`src/ui`) draw that state and write requests into it; they keep only their own state (scroll, edit buffers, a map's
pan and zoom). Nothing here includes the UI, so the UI can be left out and the tools still build.

## What is here

| Tool | Files | State | Its viewport layers |
|---|---|---|---|
| LOD | `lodtool.*` | `LodToolState`: the rows (every streaming root and render-LOD group), the inspection, the list's filters, the selection, the render-LOD counts, the window's requests | labels, the heuristic gizmo, the pop flash, the LOD Level view's colours (`tools.lod.*`) |
| Streaming | `streamingtool.*` | `StreamingToolState`: the rows, totals, inspection, filters, selection, the strip chart's history, the cameras for the map, the flight, the requests | bounds, load and unload rings, labels (`tools.streaming.*`) |
| Debug camera | `debugcamera.*` | `DebugCameraState`: its pose and the main camera's trail | the main camera's frustum, body and trail; its switches are `tools.debugcamera.*` |

`tools.h` holds the three as `ToolsState`, which `src/apps/view.cpp` owns, fills each frame and binds into `ViewUI` (`bindTools`) and the
commands' `ViewContext`. How each tool works is in `src/view/README.md` ("The LOD tool", "The Streaming tool", "The debug camera").

## Rules

- **Layering**: the library links `view`, `renderer` and `config`; `ui` links it. Nothing here includes a `src/ui` header, and
  nothing in `src/view` or the renderer includes a header from here.
- **State**: a tool's state is data the application fills and the windows read; requests (a level set, a root framed, radii tuned) are
  fields the window sets and the application applies and clears in its frame, never calls into the engine from the window.
- **Settings**: a tool's switches and layers are configuration values (`src/config/README.md`), named `tools.<tool>.*`, declared in its
  `.cpp` and named in its header; windows edit them by name.
