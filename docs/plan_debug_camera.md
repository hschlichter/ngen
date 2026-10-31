# Debug camera: watching the main camera from outside

**Status. In progress.** Built 2026-10-31. Decisions as leaned, pushback welcome. Comes before [plan_streaming_budgets.md](plan_streaming_budgets.md): budgets are
easier to judge when the refused and late roots can be watched around the player from above.

## Current state

ngen-view has one camera (`Camera cam`, `src/apps/view.cpp`), and everything that depends on where the viewer is reads it:

| What | Where it reads the camera |
|---|---|
| Streaming residency and streaming LOD (scene thread) | `scene.setCamera(cam.position, …)` each frame |
| Render-LOD selection (GPU) | `LodSelectPass::Camera`, its eye from `RenderSnapshot::viewMatrix` (`renderer.cpp`) |
| Culling (GPU) | `RenderSnapshot::cullViewProj`, from `cam` through `cullState` (live, or frozen from the Culling window) |
| Shadow cascades | fitted on the main thread from `cam` |
| The LOD and Streaming tools' inspectors, the Streaming map | `cam`'s position and window area |
| Rendering, picking, gizmos, labels | `cam`'s view and projection |

So the streaming and LOD states can only be seen from where they are decided. The frozen culling frustum (`cull freeze`,
the Culling window) is the one existing way to look at a decision from elsewhere, and it stops the decision to do so.

## Scope

**In**
- A main camera (the player) that always exists and decides residency, LOD and, optionally, culling; and a debug camera that, when on,
  is the one rendered from.
- Input that moves either one; the main camera keeps moving by `fly`, scripts and RPC whichever is controlled.
- The main camera drawn in the viewport from the debug camera: its frustum, its body and a trail of where it went.
- RPC, verbs and records for both; the Camera window and the Engine Map.

**Out**
- A second render (picture in picture of the main camera's view): deferred.
- Any change to what residency, LOD or culling decide: they read the main camera exactly as they read `cam` now.
- Several main cameras (split screen), editor viewports.

## Decisions

### 1. What culls

- **A. The camera rendered from** (the debug camera when on): from outside, everything resident is drawn, the tiles behind the player too.
- **B. The main camera**: only what the player would draw is drawn, as a frozen culling frustum that moves.

**A by default, B as a toggle** (`cull from main`). Streaming states behind the player are what the debug camera is for; B answers "what
does the player actually draw" and replaces freezing for that question while moving. A frozen frustum still wins over both, as now.
Shadow cascades follow the camera that culls, so B shows the player's shadows.

### 2. Who the input moves

- **A.** The keys and mouse move the debug camera while it is on; the main camera moves only by `fly`, scripts and RPC.
- **B.** As A, and a shortcut hands the controls to the main camera while the view stays on the debug camera.

**B**: driving the player by hand while watching from outside is the case that matters; it is one flag on which camera `update()` runs on.

### 3. How it is shown

- **A. One viewport**, rendered from the debug camera while it is on, the main camera drawn in it.
- **B. As A, plus a picture in picture** of the main camera's view.

**A**: B renders the scene twice, which the renderer can't yet (one view per frame). Deferred with that as its trigger.

## Steps

1. **Two cameras** (`src/apps/view.cpp`, `src/view/viewcommands.h`). `cam` stays the main camera, so everything that sets it today (`fly`,
   `camera`, `camera-use`, `--camera`, the Camera window's jumps and bookmarks, frame scene/selected) keeps driving the player. A new
   struct beside `CameraFlight`, held by `ViewContext`:

   ```cpp
   // The debug camera: when on, the view renders from it, while the main camera (ViewContext::cam) keeps deciding residency, LOD and
   // culling. The main camera's trail is kept by distance moved, never by time.
   struct DebugCamera {
       bool on = false;
       Camera camera;
       bool controlMain = false; // the keys and mouse move the main camera instead
       bool cullFromMain = false;
       bool trail = true;
       std::vector<glm::vec3> trailPoints; // a point every trailSpacing metres the main camera moved, the last trailCount
       static constexpr float trailSpacing = 2.0f;
       static constexpr size_t trailCount = 500;
   };
   ```

   One helper, `viewCamera()`, returns the camera rendered from (the debug camera when on, else `cam`); the view and projection in the
   `RenderSnapshot`, picking, gizmos, `DebugDraw`'s eye and the LOD and Streaming labels' projection read it. `cam.update(keys, dt)` runs on
   the controlled camera. Turned on, the debug camera starts 15 m behind and 10 m above the main camera, looking at it.
2. **The main camera's eye in the renderer** (`src/renderer/rendersnapshot.h`, `renderer.cpp`): `RenderSnapshot` gains `lodEye` and
   `lodWindowArea`, which `LodSelectPass::Camera` and the LOD readback take instead of deriving them from `viewMatrix`. The main thread sets
   them from `cam`; with the debug camera off they equal what is derived now, so nothing changes.
3. **Culling and shadows** (`view.cpp`): `cullState.update` takes the view camera's view-projection, or the main camera's with
   `cullFromMain`; the cascades are fitted from the same camera.
4. **The tools read the main camera**: the LOD and Streaming inspectors' eye and window area, the Streaming map's camera marker, and the
   residency (`scene.setCamera`, unchanged) all use `cam`. The Streaming map also marks the debug camera when it is on.
5. **Drawing the main camera** (`src/ui/viewui.cpp` `drawDebug`, beside the frozen frustum): while the debug camera is on, the main
   camera's frustum (its four side edges and a far rectangle at the shadow distance, as the frozen frustum is drawn), a small box at its eye
   with its forward axis, and its trail as a line strip. Off while the debug camera is off.
6. **RPC, verbs, records** (`src/view/viewcommands.cpp`):
   - `view.camera.debug {on?, controlMain?, cullFromMain?, trail?}`, returning the state; verbs `debug-camera on|off`,
     `debug-camera control main|debug`, `debug-camera cull view|main`, `debug-camera trail on|off|clear`.
   - `view.camera.set` and `view.camera.get` take `camera: "main" | "debug"`, `main` by default, so scripts and `fly` are unchanged.
   - The `status` record's `camera` keeps the main camera's pose; a `debugCamera` field beside it holds `{on, position, yaw, pitch,
     controlMain, cullFromMain, trailPoints}`.
7. **UI**: the Camera window gets a Debug camera section (on, control main / debug, cull from view / main, trail and clear, "place behind
   main", both poses); the View menu an item and the shortcut for the controls (one not bound yet in `view.cpp`'s key handling, shown in
   the menu). The Engine Map's "Input and camera" node says there are two cameras, and its edges name which goes where: the main camera to
   LOD selection, Residency and Culling, the view camera to the frame's RenderSnapshot.
8. **READMEs**: `src/view/README.md` (the two cameras, what reads which), `src/renderer/README.md` (`lodEye`, `lodWindowArea`), the
   `run-headless` skill (the verbs, screenshots from the debug camera).

## Visibility

- **Records**: `status` gains `debugCamera`; `camera` in it stays the main camera's pose. The `lodtool`, `streamingtool`, `streaming` and
  `renderlod` records keep measuring from the main camera.
- **RPC and verbs**: `view.camera.debug` and `debug-camera …` (new); `view.camera.set` / `view.camera.get` gain `camera`.
- **Windows and overlays**: the Camera window's Debug camera section; the main camera's frustum, body and trail in the viewport; the debug
  camera's marker on the Streaming map; the Engine Map's camera node and edges.

## Verification

- **Nothing changes while off**: the 8 reference images (three_cubes and Sponza: lit, albedo, normals, prepass) byte-identical, with
  validation on; the `culling` and `render` records equal.
- **The main camera decides**: at `/cameras/village`, `debug-camera on`, then the main camera stepped 1 m a frame for 60 frames with
  `camera` (not a whole flight): the debug camera's pose in `status` doesn't change; the `streaming` record's states and the `lodtool`
  record's measures follow the main camera, and equal a run without the debug camera at the same main poses.
- **Culling**: with `debug-camera cull main`, the `culling` record's counts equal the run without the debug camera; with `cull view`, more
  instances are drawn (those behind the player).
- **Seen**: a screenshot from the debug camera shows the main camera's frustum, body and trail, the streaming view's colours changing
  around it between two main poses.
- **Controls**: `debug-camera control main` then `control debug` reported in `status`; a key press moves the camera it names (by hand in
  the window; handed off as a check).
- No warnings or errors in the trace.

## Results

Built 2026-10-31, release build. Where the build differs from the steps:
- **The state lives in the Camera window's state** (`DebugCameraState` in `src/ui/camerawindow.h`, as the other windows' states that
  `src/view` reads), not in `ViewContext` beside `CameraFlight`; placing, the trail and drawing the main camera are `src/view/debugcamera.*`.
- **Only `lodEye`** was added to `RenderSnapshot`: both cameras share the projection, so the window area still comes from it.
- **The shortcut is C**; F (frame selected) and the wheel act on the camera that has the controls.
- **`camera x,y,z,yaw,pitch debug`** places the debug camera from a script (the verb's optional second word), for the checks.
- **Gap**: the corner axis gizmo shows and turns the main camera, not the one rendered from (it holds the main camera; re-pointing it
  would touch it from the render thread).

Checks, at `/cameras/village`, the main camera stepped 1 m a frame for 60 frames with `camera`:
- **Nothing changes while off**: the 8 reference images byte-identical, validation on.
- **The main camera decides**: with the debug camera on, its pose in `status` stays put (placed 15 m behind and 10 m above:
  -550.25, 273, 19.8) while the main camera moves; the `streaming` record's 396 roots have the same state, drawn, selected and resident
  levels as the run without the debug camera; render LOD's levels not drawn are the same (91,205).
- **Culling**: with `cull main`, the `culling` record equals the run without the debug camera (625,738 drawn, 9.66 M triangles); culling
  from the debug camera draws what it sees instead.
- **Seen**: a screenshot from above the path with the streaming view shows the main camera's box, frustum and trail among the drawn and
  prefetched tiles.
- No warnings or errors in the trace. The controls (C, the mouse) handed off as a check by hand.

Changed 2026-10-31 (Henrik): **the main camera drives everything by default**, culling and the shadow cascades too (decision 1's A
reversed), and each of LOD and streaming, culling and shadows can be switched to the debug camera on its own (`lodFromDebug`,
`cullFromDebug`, `shadowsFromDebug`; `debug-camera lod|cull|shadows main|debug`), replacing `cullFromMain` and `debug-camera cull
view|main`. LOD and streaming are one switch: the scene's LOD heuristic, residency and the GPU's render LOD answer the same "where is the
viewer". Checked at the village with the main camera stepped 30 frames: with the defaults, the `streaming` record's 396 roots, the `culling`
record and render LOD's levels not drawn equal the run without the debug camera; with all three on the debug camera, 82 roots change
state, culling draws what the debug camera sees, and the measures are from it. The 8 reference images byte-identical.

## Deferred / follow-ups

- **Picture in picture** of the main camera's view. Trigger: the renderer rendering more than one view per frame.
- **A chase mode**: the debug camera following the main camera at a fixed offset. Trigger: losing the player during a fast `fly`.
- **Debug camera bookmarks**, saved beside the main camera's. Trigger: returning to the same vantage points across sessions.
