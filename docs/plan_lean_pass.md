# Lean pass: the engine apart from its tools, a lean frame loop and lean instrumentation

**Status. In progress.** Step 0 done 2026-10-31, step 1 and 2 2026-11-01. Decisions locked 2026-10-31 (Henrik): the layering and the order below; settings move into the configuration system
([plan_configuration.md](plan_configuration.md)); the Engine Map and Scene Flow windows removed with their code. Pushback welcome on the
open choices.

## Current state

The UI and tools can't be cut from the engine: the dependencies run both ways.
- **Settings live in UI structs**: `ViewUI`, `CameraWindowState`, `LodWindowState` hold render settings, views, culling, the debug camera's
  switches and the camera modes; the frame loop and RPC read them there ([plan_configuration.md](plan_configuration.md), Current state).
- **Engine-side code includes UI headers**: `src/view/lodtool.h` → `lodwindow.h`, `streamingtool.h` → `streamingwindow.h` (the tools'
  data types, `LodWindowRow`, `LodInspection`, `StreamingWindowRow`, `StreamingInspection`, are defined in the windows and built in
  `src/view`); `debugcamera.h` and `ground.cpp` → `camerawindow.h`; `viewcommands.h` → `enginemap.h`; `viewdumps.cpp` → `gpuscenewindow.h`.
  `src/view` isn't a library: its files, tools among them, are compiled into `ngen-view`.
- **The renderer carries tools**: the ImGui backend, `EditorUIPass` and `ImGuiFrameSnapshot` in `RenderSnapshot`; the editor gizmos (the
  axis gizmo holds the main thread's `Camera*` and reads it from the render thread); capture, frame debug, render debug, GPU counters and the
  frame graph's previews. The `renderer` library links ImGui.
- **The frame loop mixes engine and tool steps**: `src/apps/view.cpp`'s loop is about 950 lines (from line 552), a third of it tool syncs
  (the window lists, `LodWindowSync`, `StreamingWindowSync`, labels, gizmos).
- **`viewcommands.cpp` is a catch-all**: 2,922 lines, 52 RPC methods, 21 records, 51 verbs, and logic that isn't RPC (the flight,
  `lodCamera`, `engineMapStats`, asset go-to).
- **The Engine Map and Scene Flow windows** (934 lines in `src/ui`: `enginemap.*`, `enginemapwindow.*`, `sceneflowwindow.*`) aren't
  what they should be (Henrik, 2026-10-31). Around them: the `map` record and `engineMapStats` in `viewcommands.cpp`, seven
  `drawEngineMapLink` calls in other windows, the menu items and the frame loop's syncs for both, two `src/view/README.md` sections, and
  the `AGENTS.md` rule that every new part of the engine gets a node in the map.
- **Instrumentation runs unconditionally**: the visibility word of each of the 2.4 million draws at the village read back, unpacked and
  copied every frame for the AABB overlay alone (`cameraVisible`, one of the two arrays, has no reader); the overlay's list built with the
  overlay off; the scripts' residency bookkeeping inside the loop.

## Scope

**In**
- The layering below, enforced by the build: a target without the tools and UI layers.
- Moving code to its layer; splitting `view.cpp`'s loop and `viewcommands.cpp`; the renderer's tools moved out of its core.
- Removing what nothing needs, and making instrumentation cost nothing while unused.

**Out**
- Changing what anything does: every step keeps behaviour, records and images.
- GPU culling's cost (about 18 ms at the village): the core renderer's work, for the roadmap's mesh-shader step.
- The UI's size: it may be large, as long as it is a layer that can be cut.

## The layers

| Layer | Contents | May include |
|---|---|---|
| Foundation | `profile`, `trace`, `config` ([plan_configuration.md](plan_configuration.md)), `rpccore` | the standard library |
| Engine | `world`, `renderer` (rendering only), the view runtime (`ViewScene`, placement, camera, camera modes) | Foundation, each other |
| Instrumentation | records, RPC methods and verbs per domain; the renderer's capture, frame debug, counters | Engine |
| Tools | LOD and Streaming tool logic and data, the debug camera, overlays, gizmos | Engine, Instrumentation |
| UI | `src/ui` windows, the ImGui backend, `EditorUIPass` | everything above |

Nothing includes from a layer below it in the table.

## Steps

One step, one commit; each verified before the next.

0. **The Engine Map and Scene Flow removed**, first, so later steps don't move code that goes:
   - `src/ui/enginemap.*`, `enginemapwindow.*`, `sceneflowwindow.*`; their `ViewUI` members, menu items and `window enginemap|sceneflow`;
     the `drawEngineMapLink` calls in the Assets, Camera, LOD and Streaming windows; the Scene Flow and Engine Map syncs in `view.cpp`.
   - The `map` record, `mapRecord` and `engineMapStats` (`viewcommands.*`); `viewcommands.h`'s `enginemap.h` include goes with them.
   - `src/view/README.md`'s "Where everything sits: the Engine Map" and "How a scene reaches the screen: the Scene Flow window", and the
     references to both elsewhere in the READMEs and the `run-headless` skill.
   - The upload budget and geometry pool limit, whose only controls were Scene Flow's, stay reachable by the `gpuscene` verb until
     [plan_configuration.md](plan_configuration.md) makes them configuration values.
   - `AGENTS.md`'s "Runtime access and visibility": the Engine Map sentence replaced by "A change says where in the engine's flow it sits in
     its library's README." (done 2026-10-31).
1. **Settings into configuration** ([plan_configuration.md](plan_configuration.md) builds the system and moves the render settings): then
   culling, purposes, the debug camera's switches, the camera modes, the LOD Level view's colour mode and the LOD and Streaming layers.
   `ViewUI` keeps UI state only.
2. **The tools' data out of the UI**: the rows, inspections, shapes and totals, `DebugCameraState`, `CameraModesState`'s results move to the
   tools layer; the windows include them.
3. **Libraries**: `src/view` becomes the `view` library (engine: `viewscene`, `placement`, `viewpicking`, camera modes' ground test);
   `src/tools/` (new) the `tools` library (`lodtool`, `streamingtool`, `debugcamera`, the overlays); `ui` links `tools`, `tools` links
   `view`.
4. **`viewcommands.cpp` by domain**: camera, scene and LOD, display, introspection, the tools' records (`lodtool`, `streamingtool`, `zones`),
   the verb table; the flight, `lodCamera` and asset go-to move to the view runtime.
5. **The frame loop**: the loop is the engine's steps in order (scene update, commands, camera, culling and cascades, snapshot, submit),
   with one tools hook called at fixed points (after the scene update, after the camera, at debug drawing, at UI drawing); the scripts'
   residency bookkeeping moves to the commands. `view.cpp` keeps arguments, wiring and the loop.
6. **The renderer's tools out of its core** (`renderertools`): the ImGui backend and `EditorUIPass`, the gizmo passes, capture, frame debug,
   render debug, counters, previews. The core keeps an extension point (passes added after the frame, capture hooks). The axis gizmo reads
   the snapshot's view matrix instead of a `Camera*`.
7. **Instrumentation paid for only when used**: the per-draw visibility read back while the AABB overlay is on only; `cameraVisible`
   removed; the overlay's list built only while it is shown.
8. **The proof target**: `ngen-view-core` in `build.cpp`, the engine and instrumentation layers without tools and UI, built with the rest so
   the layering can't erode.

## Visibility

- **Records, RPC and verbs**: the `map` record and `window enginemap|sceneflow` removed (step 0); the rest unchanged in what they answer,
  except where [plan_configuration.md](plan_configuration.md) replaces the bespoke setters with `config.*`; they move between files only.
- **Windows**: the Engine Map and Scene Flow removed (step 0). What the Scene Flow window showed stays in records: `scenethread`,
  `gpuscene`, `assets`. The upload budget and geometry pool limit lose their window controls until the Configuration window
  ([plan_configuration.md](plan_configuration.md)), keeping the `gpuscene` verb.

## Verification

After every step:
- The 8 reference images (three_cubes and Sponza: lit, albedo, normals, prepass) byte-identical, with validation on.
- At `/cameras/village`, the `status`, `streaming`, `culling` and `lodtool` records equal the step before's for the same script.
- No warnings or errors in the trace.

At the end:
- `ngen-view-core` builds and runs the baseline script headless, its images byte-identical to `ngen-view`'s.
- `rg` finds no include of a `src/ui` or `src/tools` header from `src/view`, `src/world` or `src/renderer`, and no ImGui include in the
  renderer's core; nothing names the Engine Map or Scene Flow outside `docs/` plans and reviews.
- The render thread's time at the village with the AABB overlay off drops by the visibility readback (about 15 ms a frame measured
  2026-10-31: `ParseReadback` and `CullResult`).
- `src/apps/view.cpp`'s loop and each split `viewcommands` file listed with their line counts beside today's.

## Results

- **Step 0** (2026-10-31): the six window files deleted (934 lines) and their wiring: `ViewUI`'s members and accessors, the menu items, the
  `window` names, the four "Map:" links, the frame loop's two syncs (about 80 lines of `view.cpp`), the `map` record with `mapRecord` and
  `engineMapStats` (97 lines of `viewcommands.cpp`); `viewcommands.h` no longer includes a UI header for it. `src/view/README.md` keeps the
  scene's path to the screen as "How a scene reaches the screen", its numbers in the `assets`, `scenethread` and `gpuscene` records. In all
  1,195 lines removed, 22 added. The 8 reference images byte-identical with validation on; the village run with the LOD, Streaming, Camera
  and Assets windows open clean; `window sceneflow` and `window enginemap` answer "unknown window".

- **Step 1** (2026-11-01; the render settings went with [plan_configuration.md](plan_configuration.md)): 30 more values, 49 in all.
  - Where they are declared: culling in the renderer (`render.culling.enabled`, `.frozen`), purposes in the scene (`scene.purposes.guide`,
    `.proxy`), following in the camera modes (`view.camera.follow`, `.followHeight`), and the tools' in their files: the debug camera
    (`tools.debugcamera.on`, `.controlMain`, `.lod`, `.cull`, `.shadows`, `.trail`), the LOD layers (`tools.lod.labels`, `.gizmo`,
    `.gizmoAll`, `.popFlash`, `.colorByDepth`), the Streaming layers (`tools.streaming.bounds`, `.rings`, `.ringsAll`, `.labels`), and the
    viewport overlays in `viewui.cpp` where `drawDebug` draws them (`tools.overlay.grid`, `.origin`, `.gizmos`, `.aabbs`, `.selectedAabb`,
    `.lightGizmos`, `.culled`, `.culledView`, `.cascadeFrusta`; they move with that code in step 3).
  - Windows show a value another library declares by name (`configCheckbox`, `configBool`, `configMenuItem` in `src/ui/configwindow.h`),
    so the Camera, LOD and Streaming windows and the menu include no tool header.
  - Removed: the settings-only methods `view.cull`, `view.camera.follow`, `view.lod.visualize`, `view.streaming.visualize`; their verbs
    (`cull`, `camera-follow`, `lod-show`, `streaming-show`) set configuration. `view.camera.debug` (place, clear the trail) and
    `view.purposes` (answers once drawn) stay, their switches setting the values. The `status`, `lodtool` and `streamingtool` records
    no longer copy switches the `config` record has.
  - `ViewUI` holds no setting now: its members are windows open, filters, selections, requests and statistics.
  - Checks: the 8 reference images byte-identical; the debug camera's three runs at the village (off, defaults, every switch on the debug
    camera) give `streaming` and `culling` records identical to before the step; every verb above shows in the `config` record; the debug
    build runs with the Configuration, Camera, LOD, Streaming, Culling and Render Debug windows open. It caught the LOD window's Bias and
    Hysteresis inputs asserting in ImGui (since the LOD tool), fixed.

- **Step 2** (2026-11-01): the tools layer is `src/tools/` (Henrik). `lodtool.*`, `streamingtool.*` and `debugcamera.*` moved there from
  `src/view` (step 3's file moves done here, so nothing moves twice), with their data out of `src/ui`:
  - `src/tools/lodtool.h`: `LodShape`, `LodRow` (was `LodWindowRow`), `LodInspection`, `LodToolState` (the rows, inspection, filters,
    selection, render-LOD counts and the window's requests), `lodLevelColor`, `lodRowShown`.
  - `src/tools/streamingtool.h`: `StreamingLevel`, `StreamingRow`, `StreamingInspection`, `StreamingTotals`, `StreamingToolState` (rows,
    totals, inspection, filters, selection, history, cameras, flight, requests), `streamingStateColor`, `streamingStateName`,
    `streamingRowShown`.
  - `src/tools/debugcamera.h`: `DebugCameraState`. `src/view/ground.h`: `CameraModesState` (the camera modes are the view runtime's).
  - `src/tools/tools.h`: `ToolsState`, the three together, owned by `view.cpp` and bound into `ViewUI` (`bindTools`) and `ViewContext`.
    `ViewUI` no longer owns any tool's state; the windows keep only theirs (`LodWindowState`: scroll, "Set shown", edit buffers;
    `StreamingWindowState`: scroll, the map's pan and zoom, edit buffers; `CameraWindowState`: bookmarks and jump).
  - Nothing in `src/tools` or `src/view` includes a UI header except `viewcommands.cpp` (`viewui.h`) and `viewdumps.cpp`
    (`gpuscenewindow.h`), for step 4. The tools' code still compiles into `ngen-view` until step 3 makes it a library; the UI library
    includes `src/tools` and `src/view`.
  - Checks: the 8 reference images byte-identical; the three debug camera runs' `streaming` and `culling` records identical to before;
    with the LOD, Streaming and Camera windows open, labels, bounds and following on, the `lodtool` record has its 11,954 rows and the
    `streamingtool` its 396 roots, as before; validation clean.

## Open questions

- **The renderer split (step 6) now or later**: steps 1 to 5 already make the view cuttable; step 6 is the largest single move.

## Deferred / follow-ups

- **The time-based streaming measurement** (late since, lead per level) moved out of the scene thread's residency into the instrumentation
  layer. Trigger: the scene thread's code read again for the next streaming change.
- **The per-draw debug fields** (`streamRoot`, `lodDepth`, two words of the 128-byte instance record used only by debug views). Kept: they
  cost no space the record didn't have. Trigger: the record needing the room.
