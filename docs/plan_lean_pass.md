# Lean pass: the engine apart from its tools, a lean frame loop and lean instrumentation

**Status. Landed.** Step 0 done 2026-10-31, steps 1 to 5 2026-11-01, step 7 2026-11-04, step 6 2026-11-05; step 8 dropped 2026-11-05. Decisions locked 2026-10-31 (Henrik): the layering and the order below; settings move into the configuration system
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
- The layering below, as the code's structure: each layer a library that links only the layers below it.
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
8. Dropped (Henrik, 2026-11-05): the layering is the code's structure; an executable only to check it adds confusion and complexity.

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
- `rg` finds no include of a `src/ui` or `src/tools` header from `src/view`, `src/world` or `src/renderer`, and no ImGui include in the
  renderer's core; nothing names the Engine Map or Scene Flow outside `docs/` plans and reviews.
- The render thread's time at the village with the AABB overlay off drops by the visibility readback (about 15 ms a frame measured
  2026-10-31: `ParseReadback` and `CullResult`).
- The architecture decisions recorded: [ADR 75](adr/0075_engine_layering.md), [ADR 76](adr/0076_viewframe_steps_and_hooks.md),
  [ADR 78](adr/0078_render_extensions.md).
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

- **Step 3** (2026-11-01): two libraries in `build.cpp`.
  - `view`: `viewscene`, `placement`, `viewpicking`, `ground`, `camera`; links `renderer`, `world`, `assetclient`, `packer`, `config`.
  - `tools`: `src/tools/*`; links `view`, `renderer`, `config`. `ui` links `tools`, and no longer adds `src/view` or `src/tools` to its own
    include paths (they come with the libraries).
  - `src/camera.*` and `src/debugdraw.*`, compiled into the program and called by the renderer, the view runtime and the tools, went to
    their owners instead of a shared library (a `common` library was tried and dropped, Henrik: ownership over buckets):
    - the projection convention (`perspectiveProjection`, `projectionDepthAt`, `projectionNear`, was `cameraProjection`, …) is the
      renderer's: `src/renderer/projection.*`; the field of view stays the view's (`cameraFovDegrees`);
    - the camera (pose, flying, orbit, dolly, the follow height) is the view runtime's: `src/view/camera.*`; it takes a `CameraMove`
      (forward, right, up, fast) the application maps its keys onto, instead of reading SDL's keyboard;
    - the debug lines are the renderer's input: `src/renderer/debugdraw.*`;
    - the corner axis gizmo no longer holds the main thread's `Camera*` (a race, and the renderer's only need for `Camera`):
      `gizmoHitTest` returns the axis clicked and `view.cpp` turns the camera rendered from (before, always the main camera).
    - The renderer's public include paths carry `src/scene`, `src` and glm, which its headers need.
  - `viewcommands.cpp` and `viewdumps.cpp` stay in the program until step 4 splits them. `src/tools/README.md` (new) has the layer's rules.
  - Checks: every release target builds; the 8 reference images byte-identical; the debug camera's runs give `streaming` and `culling`
    records identical to before; following holds 1.8 m on all 30 steps; the village run with the tool windows open gives the same
    `lodtool` (11,954 rows) and `streamingtool` (396 roots) records; the debug build runs with every tool window open.

- **Step 4** (2026-11-01): `viewcommands.cpp` (2,755 lines) split by domain into the `viewcommands` library, and every command registered
  by the layer that owns what it acts on.
  - Engine (`src/view`): `cameracommands`, `scenecommands`, `lodcommands`, `configcommands`, `displaycommands` (texture inspection, dumps,
    screenshots, captures, RenderDoc), `profilecommands`, and `viewcommands.cpp` (`registerViewCommands`, `introspect.record`). `zones` is the
    profiler's, so it went there, not with the tools.
  - Tools (`src/tools/toolcommands.*`): `view.camera.debug`, `view.lod.inspect`, `view.streaming.inspect`; `debug-camera`, `lod-show`,
    `streaming-show`; the `lodtool` and `streamingtool` records, and a new `debugcamera` record (its pose and trail, from `status`). They set
    `ViewContext::debugCamera`, which the camera methods use for `camera: "debug"`. `lodCamera` went to `debugcamera.h`.
  - UI (`src/ui/uicommands.*`): `view.window`, `window`, and a new `windows` record (each window and whether it is open, which replaces
    `streamingtool`'s `window` field). `view.overlay` is gone: the `overlay` verb sets the configuration values, as the other short forms do,
    and `ViewUI::setOverlay` went with it.
  - `ViewContext` has no `ViewUI&` and no `ToolsState&`. `RenderLodRequest` carries its own answer (a callback) instead of a kind the engine
    dispatched to the tools' inspection.
  - The verb table is `RpcVerbs` (`src/rpc/rpcverbs.*`): each layer adds its verbs beside its methods. `requiredField`/`optionalField` are
    the registry's. The flight (`cameraflight.*`) and "go to asset" (`assetgoto.*`) are the view runtime's, over a camera and the scene
    instead of the commands' context; the flight ends through a callback, so the runtime has no RPC.
  - The GPU scene's capture join (`GpuSceneInstanceRow`, `buildGpuSceneRows`, the JSON writer, the capture list) went from the GPU Scene
    window to the renderer (`src/renderer/gpuscenecapture.*`); `viewdumps.cpp` includes no UI header.
  - Prim paths have one owner, `src/world/primpath.*` (`child`, `join`, `isAtOrUnder`), for the copies in the composer, the World,
    placement, the LOD and Streaming tools, the commands and `view.cpp`. The composer keeps its own variant-aware child and parent paths. The
    framegraph's "Helpers" banner says what the functions under it are for.
  - Checks: release and debug build; the 8 reference images byte-identical; the debug camera's runs give `streaming` and `culling` records
    identical (but for timings and pack keys) to before and to the first runs; following holds 1.8 m on all 30 steps; the village run with
    the tool windows open gives `lodtool` 11,954 rows and `streamingtool` 396 roots; a bad overlay name, an unknown verb, the debug camera
    while off, a flight with no cameras and an unknown window each fail with their message; the debug build runs with every tool window
    open, and its assert that no verb is added twice holds.

- **Step 5** (2026-11-01): the frame loop is the engine's steps in order (Henrik: the steps in the `view` library, the hooks an interface
  the tools and the UI implement).
  - `ViewFrame` (`src/view/viewframe.*`) holds the frame's engine state (the main camera, the cameras the frame uses, the camera modes, the
    flight, go-to, the selection and picking, residency, culling and its readback, the cascades, the debug lines, the readbacks) and one
    method per step: `updateScene`, `advanceTime`, `resetCameras`, `setViewport`, `sendSceneInputs`, `updateResidency`, the controls (`look`,
    `wheel`, `startOrbit`, `pick`, `deselect`, `selectParent`, `frameSelected`, `frameScene`), `moveCamera`, `cull`, `countLodPops`,
    `fitCascades`, `drawCameraModes`, `readBack`, `buildSnapshot`.
  - `ViewHooks` (`src/view/viewhooks.h`): `afterScene`, `cameras`, `event`, `afterCamera`, `debugDraw`, `wantReadbacks`/`takeReadbacks`,
    `drawUi`, `fillSnapshot`, each a no-op unless overridden. `ToolsHooks` (`src/tools/toolshooks.*`) implements the tools' part: the rows,
    the debug camera's placement and the cameras it takes over (`FrameCameras`, replacing the loop's `viewCamera`/`controlledCamera`
    lambdas), the C key, the transform and axis gizmos, the trail, the overlays and layers, the LOD and Streaming windows' requests (the
    loop's two syncs). `UiHooks` (`src/ui/uihooks.*`) wraps it: ImGui's events and frame, F12, the Camera window's list, the Culling
    window's numbers, the windows' readbacks, the status bar (`StatusBarGatherer` moved from `view.cpp` to `src/ui/statusbar.*`), the
    Assets, Edit, texture, RenderDoc, screenshot and Camera window requests.
  - The overlays moved from `ViewUI::drawDebug` to `src/tools/overlays.*` with their configuration values; `ViewTool` went to `tools.h`
    (`ToolsState::activeTool`, set from the menu by the UI). The tools' states know whether their window is open (`windowOpen`, set by the
    UI), so the tools decide alone when their rows are kept.
  - The commands: `runFrameCommands` (the script's clock, its due lines, the endpoint, `--frames`) and `answerResidentWaiters` in
    `viewcommands`; the clock is `ScriptClock` (`scriptHeld` and `scriptFrame` left `ViewResidency`). `ViewDumps` takes its readbacks
    through `wantReadbacks`/`takeReadbacks`. The asset server's change notices are the scene update's now, not the UI section's.
  - `view.cpp`: 1,481 lines to 566; the loop from about 880 lines to 47. The window's input is `pollInput` (SDL's events to the hooks, then
    to `ViewFrame`'s controls), the program's. `ImGuiBackendVulkan::processEvent` takes a const event.
  - Behaviour kept, with three differences in when: a camera taken over by the debug camera is decided once a frame after the commands, so
    a switch made in a window or by C shows the next frame (before, the same frame, from a debug camera not yet placed); a gizmo drag takes
    the mouse's motion before mouse look; the first frame's time is 0. The `Camera` and `Cull` profiler zones replace `CameraAndCull`.
  - Records, methods, verbs and windows unchanged.
  - Checks: release and debug build; the 8 reference images byte-identical; the debug camera's runs give `status`, `streaming` and
    `culling` records identical (but for timings and pack keys) to step 4's; following holds 1.8 m on all 30 steps; the village run with
    the tool windows open gives `lodtool` 11,954 rows and `streamingtool` the same as step 4 (only `pops` differs, and it differs between
    runs of one build: 4 and 22); the error paths fail with their messages; the debug build runs with every tool window open; the overlays
    (grid, origin, bounds, selection, cascades, light, axis gizmo) draw on three_cubes.

- **Step 6** (2026-11-05): the renderer's tools out of its core, split by layer (Henrik: the instrumentation as `rendertools`, one step).
  - The extension point: `RenderExtension` (`src/renderer/renderextension.h`), added before `Renderer::init`, called in order at fixed points of
    `render`: `init` (with the device, pipeline registry, frame graph, deletion queue, uploader, preview sampler and `PreviewTextures`),
    `beginFrame` (first in every frame, recording or not), `afterFence`, `addOverlayPasses`, `beforeExecute`, `afterExecute`, `afterSubmit`,
    `endFrame`, `destroy`. `Renderer::state()` gives the instrumentation what the renderer holds and last did; `setDrawLogEnabled`,
    `waitForFrame` and `commandBuffer(slot)` the rest.
  - Frame data rides in the snapshot (Henrik): `RenderSnapshot::extensions`, one `RenderExtensionData` per extension at its index. The snapshot
    lost the ImGui draw data, the gizmo vertices, `showGizmo` and the mouse, and gained the capture watches (when they change), so a frame debug
    capture and the watches it describes arrive together.
  - `src/rendertools/` (new library, no ImGui): `RenderTools` with the frame graph debug snapshot and previews, the render debug snapshot, the
    frame debugger, GPU counters, the texture inspector and dumps and screenshots (all moved out of `Renderer` and `RenderThread`), and the files
    `framedebug`, `framegraphpreviews`, `gpucounters`, `renderdebug`, `renderdebugjson`, `screenshot`, `gpuscenecapture` and `capturefiles` (the
    capture writers, out of `capture.cpp`). `SceneGpuStats` and its parts stay in the core (`scenegpustats.h`): the `gpuscene` record is the
    engine's. On the main thread: `startFrame`, `request()`, `results()`, `fillSnapshot`, `requestScreenshot`, `requestTextureDump`,
    `setTextureInspect`; `ViewDumps` and `UiHooks` go through it, and `ViewFrame`'s readbacks are only the captures.
  - `src/tools/`: the gizmos (`gizmo.h`, `axis3dgizmo`, `translategizmo`, `rotategizmo`, `scalegizmo`) and `GizmoPass`, drawn by `GizmoRender`
    from the `GizmoFrame` `ToolsHooks::fillSnapshot` makes. The axis gizmo is drawn and hit-tested on the main thread (`Renderer::gizmoHitTest`
    gone), which ends the render thread writing its hover while the main thread read it.
  - `src/ui/`: `ImGuiBackend` (now a `PreviewTextures`), `ImGuiFrameSnapshot` and `EditorUIPass`, drawn by `UiRender` from the `UiFrame`
    `UiHooks::fillSnapshot` makes; it stays out of a screenshot frame unless `--show-ui`.
  - The capture service stays in the core: picking reads through it (ADR 31: one capture service). Without `PreviewTextures` it makes no
    preview textures; the dumps' pixels are kept either way.
  - Behaviour kept, with these differences in when: a screenshot is of the frame whose snapshot asked for it (before, of whichever frame the render
    thread recorded next); a pick's watch reaches the render thread with its snapshot, at most one frame later than before (9 frames from pick to
    selection at three_cubes); counters switched on start a frame later. The extensions' copies after the frame are recorded before the core's
    static captures instead of after them. The `GizmoUpdate` zone moved into `GizmoRender`; `Screenshot` and `TextureDump` became
    `ReadbackFiles`.
  - Records, methods, verbs and windows unchanged.
  - Checks: every release target and the debug build build; no ImGui and no tools, UI or rendertools header in `src/renderer`, `src/world` or
    the view runtime; the 8 reference images byte-identical (the axis gizmo now drawn from the main thread); the debug camera's village runs give
    `status`, `streaming` and `culling` records identical to step 7's, and the windows run the same tool records (`lodtool` 11,954 rows);
    `inspect`, `dump-texture`, `capture`, `dump-frame` (13 passes with their commands and descriptor sets), `dump-gpuscene`, `screenshot` and
    the `render`, `memory`, `counters` and `pipelines` records all answer; picking selects `/World/Cube_1`; the debug build runs with every
    window open and `--show-ui`.

- **Step 7** (2026-11-04): what the readback cost was not what the plan assumed. Measured at `/cameras/village`, the overlay off, over 60
  frames: the render thread's `CullResult` 13.9 ms a frame, `ParseReadback` 1.5 ms. The visibility bits were only `ParseReadback` (unpacking
  2.4 million words into two byte arrays); `CullResult` was `Renderer::cullResult` summing every draw slot's triangles (`sceneTriangles`,
  for the status bar and the `culling` record), 2.4 million 128-byte records walked every frame.
  - The visibility bits are read back only for a frame whose snapshot asks (`RenderSnapshot::readVisibility`, set by the tools while
    `tools.overlay.culled` is on); `DrawLists` copies and parses them only then, and `CullResult::viewBits` is empty otherwise.
    `cameraVisible` is gone (nothing read it). The overlay's byte list (`CullOverlay`, 0.28 ms) is built only while the overlay is on.
  - `sceneTriangles` is kept by the renderer as slots change (the bucket counts' way), the same number (175,419,138 at the village).
  - After: `CullResult` 0.00 ms, `ParseReadback` 0.00 ms; the render thread's own work from about 17 ms a frame to about 1 ms. The frame
    went only from 35.2 to 33.5 ms: the village is GPU-bound (culling and the geometry pass, about 33 ms), and the render thread now waits
    in `Present` instead of working. With the overlay on: the bits come back (`ParseReadback` 0.8 ms) and the overlay draws its 2.4 million
    boxes as before.

## Open questions

None left: the renderer split (step 6) was done in its place in the order.

## Deferred / follow-ups

- **The time-based streaming measurement** (late since, lead per level) moved out of the scene thread's residency into the instrumentation
  layer. Trigger: the scene thread's code read again for the next streaming change.
- **The per-draw debug fields** (`streamRoot`, `lodDepth`, two words of the 128-byte instance record used only by debug views). Kept: they
  cost no space the record didn't have. Trigger: the record needing the room.
