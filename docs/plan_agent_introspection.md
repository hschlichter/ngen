# Agent introspection: screenshots, camera, scripted sessions, dumps

**Status. Landed.**

## Current state

An agent can build, run headless with `--obs-output`, and read `FrameStats`, `RenderStats` and `GpuTime`. It cannot see the frame, place the
camera, pick a debug view, or read the Render Debug and Performance windows. Humans have the windows; agents have grep. The readback path
(`copyTextureToBuffer`) and PNG writer exist in the RHI examples; the profiler and render debug snapshot exist in the engine.

## Scope

**In**

- **Command verbs** shared by CLI flags and a script file, applied on the main thread at a given frame:
  `camera x,y,z,yaw,pitch`, `camera-frame scene|/prim/path`, `select /prim/path`, `view lit|albedo|normals|depth|shadowfactor|shadowmap|shadowuv|worldpos`,
  `overlay name=on|off` (grid, origin, gizmo, aabbs, lightgizmos, buffer, shadow, aa), `screenshot PATH`, `dump-render-debug PATH`,
  `dump-profile PATH`, `quit`.
- **CLI**: `--frames=N` (quit after N), `--camera=`, `--camera-frame=`, `--select=`, `--view=`, `--overlay=grid=on,aabbs=off`, `--screenshot=PATH`
  (taken on the last frame of `--frames`, or frame 3 if none), `--dump-render-debug=PATH` and `--dump-profile=PATH` (at exit), `--script=FILE`,
  `--fail-on-validation`. Existing `--validation`, `--render-debug`, `--obs-*` unchanged.
- **Script file**: one command per line, `<frame> <verb> [args]`, `#` comments. Sorted by frame; commands for frame N run at the top of frame N
  before input is polled, so a screenshot at N+1 shows the result.
- **Screenshot**: renderer records a readback of the presented backbuffer into the frame's command buffer, waits on that frame's fence, writes a
  PNG (BGRA swizzled), emits `Screenshot{path, width, height}`. Editor: Screenshot button in the Render Debug View tab and F12, writing
  `screenshot_<frame>.png` in the working directory.
- **Camera**: `CameraPose{x,y,z,yaw,pitch}` observation every 60 frames; Camera window (Windows > Camera) with the pose as a copyable string in
  the flag syntax, jump fields, Frame scene, Frame selected, and bookmarks stored in `<scene>.cameras.txt` next to the scene file, one
  `name x y z yaw pitch` per line.
- **Dumps**: `RenderDebugSnapshot` as JSON (device, swapchain, scene tables, passes with counters and GPU time, draw log with prim paths);
  profiler history as Chrome trace event JSON (`traceEvents` with `X` events per zone, one `tid` per lane, GPU as its own lane, thread names as
  metadata), readable by Perfetto and by any JSON parser.
- **`DeviceInfo`** observation at startup with the device limits.

**Out**

- Socket or stdin control of a running session. Trigger: a scripted run that needs a decision mid-run.
- Fixed timestep. Trigger: the first animated content.
- Image diffing. Agents Read the PNG; a reference compare would need a stored baseline policy first.

## Decisions

- **One verb set, two front ends.** Flags are the script's verbs at frame 0 (or frame N for the screenshot). No second implementation.
- **Screenshot from the backbuffer, not from `sceneColorAA`.** What the user sees, overlays included; debug views are selected with `view`
  before the shot, so the buffer views are all reachable.
- **Bookmarks in a text file next to the scene, not in the USD.** Camera bookmarks are editor state, not scene content, and the plain format
  needs no parser.
- **Chrome trace format for the profile dump.** Already a viewer for it in every browser; Tracy comes later for live sessions.

## Steps

1. `src/session/sessionscript.*`: verb parser, script loader, `SessionCommands` with `applyDueCommands(frame)`; CLI flags become commands.
   `SessionContext` holds the references the verbs need (camera, editor flags, scene, query, render thread, renderer requests, quit flag).
2. Screenshot: `Renderer::requestScreenshot(path)`; readback recorded after the frame graph; fence wait; PNG via stb in `src/renderer/screenshot.cpp`.
3. Editor: setters on `EditorUI` for view mode and overlay flags; Screenshot button and F12; `CameraPose` and `DeviceInfo` observations.
4. Camera window with bookmarks, `src/ui/camerawindow.*`.
5. Dumps: `renderdebugjson.cpp` writer; `profile::exportChromeTrace(path)`.
6. `--fail-on-validation`, `--frames`.

## Verification

- `ngen-view assets/three_cubes.usda --frames=30 --screenshot=/tmp/a.png --view=normals --camera=2,1.5,2,-135,-20` under the offscreen driver:
  exits 0, PNG exists at the swapchain size, obs has one `Screenshot` and `CameraPose` events with the given pose. Agent Reads the PNG and sees
  the normals view. Verified: 2560x1440 PNG, `Screenshot{ok:true}`, `DeviceInfo` present; dump JSON has 4 meshes, 8 draws with prim paths,
  9 passes; trace has 1360 events on Main, Render and GPU lanes.
- Script with `camera`, `screenshot`, `view`, `screenshot`, `quit`: two PNGs, both differ, run exits at the scripted frame. Verified: exit at
  frame 14, lit and albedo shots differ.
- `--dump-render-debug`: JSON parses with `jq`; mesh count and draw count match `RenderStats`. `--dump-profile`: `jq '.traceEvents | length'`
  is nonzero and Perfetto opens it (Henrik).
- `--fail-on-validation` with a deliberately bad barrier in an example-like scenario is not possible in the engine; verify instead that a clean
  run exits 0 with the flag and that the flag plus `--validation` reports the count in `DeviceInfo`-style summary at exit.
- Camera window: pose string matches the flag syntax; jump moves the view; bookmark survives restart (Henrik).

## Deferred / follow-ups

- Socket control, fixed timestep, image diff against a baseline, as above.
- Video capture (sequence of screenshots at every frame). Trigger: first animation bug report.
