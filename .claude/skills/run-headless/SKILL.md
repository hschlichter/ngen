---
name: run-headless
description: Build and run ngen-view headless, read its records and its trace through ngen-introspect to verify engine behavior. Use when running the app, confirming a change works, or checking rendering/scene/material behavior — headless runs traced with ngen-introspect are this project's primary verification loop (the app is a Vulkan window; this machine cannot screenshot it).
---

# Run ngen headless and read the evidence

The verification loop for engine changes: build, run headless with a script that records the values the change is about, while
`ngen-introspect trace` records every process's flow and messages; read both, judge whether the intended behavior actually happened. Records
hold the numbers (passes, draws, textures, culling, memory); the trace holds what happened in what order, and the warnings and errors, which a
clean run has none of. Prefer this over screenshots or "it compiles".

## Build

```sh
./ngen-cli build            # the set variant; ./ngen-cli set prints which
```

If `./ngen-cli` is missing (fresh clone), bootstrap `ngen-build`, then build the cli and set a variant (`AGENTS.md`, "Build"):

```sh
mkdir -p _out && c++ -std=c++23 -O0 -g -pthread -o _out/ngen-build src/build/bootstrap.cpp
./_out/ngen-build -p linux-vulkan -c debug ngen-cli && ./_out/linux-vulkan/debug/ngen-cli set linux-vulkan debug
```

Don't change the set variant unless asked; it is shared with the human's session. For another variant, `./ngen-cli build -p … -c …` and run
`_out/<platform>/<config>/ngen-view` directly.

## Run headless

ngen-view needs its variant's asset server running in the same working directory (they find each other through `.ngen-discovery/` there, and
asset ids are relative to it); without one it exits at start-up. Start it once in the background from the repository root and leave it running
across runs (it packs only what a view requests, and caches the result in `.ngen-assets/`):

```sh
./ngen-cli asset-server > /tmp/asset-server.log 2>&1 &        # the set variant's server; leave it running
./ngen-cli introspect trace --until-exit=view --output=/tmp/trace.jsonl & trace=$!   # first; records until the view has exited
SDL_VIDEODRIVER=offscreen ./ngen-cli view <scene> --frames=200
wait $trace                                                     # the trace file is written when the view has gone
```

Rebuild and restart the asset server after changing `pack.cpp` or anything under `src/asset/server/`. `./ngen-cli introspect get asset status`
shows what it is doing (also `rules`, `cache`, `requests`, `clients`); with servers for several variants running, name one by pid (`asset:<pid>`,
from `./ngen-cli introspect list`).

- Start the trace before the view. It takes events from its own start, from the view and the asset server alike, merged by time; the file
  is written, sorted, once the view has exited and its last events are in (`ProcessExiting` is the view's last event). Without
  `--until-exit` it runs until Ctrl-C. Filters: `--category=`, `--type=`, `--process=` (comma lists), `--level=warning|error`; `--history`
  takes everything the processes' rings still hold.
- A view's trace is short: the start-up timeline (each step with its time, up to `FirstFramePresented`), the files the run wrote, exit — a
  few dozen events for a clean run, however many frames. Nothing is traced per frame; per-frame numbers are records. The console shows the
  same lines, so `./ngen-cli view … 2>&1 | less` reads a run without the tool; for a slow start, look at which step's `ms` is large.
- `ngen-cli view` replaces itself with the set variant's `ngen-view`, so `timeout`, signals and exit codes behave exactly as with the binary.
  Prefer `--frames` to a `timeout` kill, so the view exits cleanly and its last events are sent.
- Write the trace to `/tmp` or the session scratchpad, not into the repo.
- The view's first frame comes at once; the scene arrives while it runs. `--frames`, `--screenshot` and script frame numbers count from when
  the scene is first resident (everything it wants loaded, uploaded and drawn; the trace's `SceneResident`), so a script sees the loaded
  scene. `--no-wait-resident` counts from start-up instead, to look at loading itself. Verbs that change what is drawn (`lod`, `purposes`,
  `repack`) hold the script's count until the change is drawn, and `wait-resident` does the same. Sponza takes a few seconds to be resident:
  give the trace command a `timeout` above that if you add one.

## See and drive the frame

`ngen-view` takes session flags so a run can be looked at, not only grepped:

```sh
SDL_VIDEODRIVER=offscreen ./ngen-cli view <scene> --frames=30 \
  --camera=2,1.5,2,-135,-20 --view=normals --overlay=grid=off \
  --screenshot=/tmp/shot.png --script=/tmp/records.txt --fail-on-validation
```

- `--screenshot=PATH` writes the presented frame as PNG on the last frame of `--frames`; Read the PNG to see it. Screenshots leave the UI out
  (the menu bar, windows and the status bar, whose numbers change every run), so they compare byte for byte; `--show-ui` keeps it in.
- `--view=lit|albedo|normals|depth|shadowfactor|shadowmap|shadowuv|worldpos|miplevel|cascades|lodlevel|hzb` (hzb: occlusion's depth pyramid at
  the level `hzb-level <n>` sets, shaded as the depth view, dark red while occlusion is off; miplevel: red level 0 to white
  level 7+; cascades: red, green, blue, yellow near to far; lodlevel: at half strength over the lit image, the drawn level of the nearest LOD root, red level 0,
  orange 1, yellow 2, green 3, … white 7+ (an index within that root), or with `lod-show color depth` how many LOD roots deep it is (1 blue,
  2 green, 3 orange, 4+ magenta), grey under none; shadowmap shows the cascade atlas), `--overlay=grid=on,aabbs=off,...`
  (grid, origin, gizmo, aabbs, lightgizmos, buffer, shadow, aa), `--camera=x,y,z,yaw,pitch` or `--camera=/camera/prim` (a camera the scene authors; Caldera has 13 under `/cameras`; placed before the scene loads, so the first residency is at that camera), `--camera-frame=scene|/prim` (what the camera frames while the scene arrives), `--select=/prim` (full path, instances expanded).
- Records at a frame go to a file: a script line `120 record render /tmp/records.jsonl` appends the render debug record (meshes, textures,
  passes with draw counters and GPU time, culled passes, draw log with prim paths) as one JSON line, `value` holding the record,
  `requested_frame` the frame asked for and `frame` the frame it was ready (GPU readbacks take a few). Any record works: `render`, `memory`,
  `counters`, `culling`, `scene`, `assets` (per asset: state, size, times), `lod` (every LOD root: levels and their payloads, heuristic, default, wanted, drawn, loaded), `payloads` (every payload placement
  reached: assets, loaded, extentsHint in world metres), `streaming` (residency: per streaming root `selected` and why, its streaming radii and `state`, `resident` and why, `drawn`, `late`/`hole`, per level requested/loaded/ready/drawn and lead, authoring `findings`; `measured` totals: late switches, holes, root-seconds coarser, loads, reloads, lead, `arrivedUnwanted`), `scenethread` (requests, composes, extracts, assets in flight and held,
  residency), `gpuscene` (change lists, upload queue and budget, geometry pool), `pipelines`, `profile`, `status` (`src/introspect/README.md`); several lines into one file are fine. Read it
  with `jq 'select(.record == "render") | .value.passes' /tmp/records.jsonl`.
- `dump-profile PATH` (script verb): the profiler history as a Chrome trace file (`jq '.traceEvents'`, or open in Perfetto).
- `--script=FILE`: `<frame> <verb> [args]` per line, same verbs as the flags plus `quit`, `pick <x> <y>` (select what is drawn at that framebuffer pixel,
  read back from the GPU; the `Picked` event names it), `translate /prim dx,dy,dz` (the view's own local opinion, never saved),
  `purposes guide=on|off,proxy=on|off` (draw guide and proxy prims), `repack <id>...|scene` (force the asset server to repack them, and
  rebuild the scene; a live view takes a source edit by itself, from the asset server's change notice, so this is for repacking an unchanged source),
  `goto-asset <id> [all]` (move the camera to an asset's next draw, nearest first, and select it; `all` frames every draw),
  `assets-of [/prim]` (the assets drawn at or under a prim use, or the selection's; `view.assetsOf` answers with them),
  `lod <LOD root path> <level>` (the level a LOD root wants; loads it), `lod-reset <LOD root path>` (back to its default), `lod-heuristic on|off` (the LOD heuristic; on by default), `lod-prefer distance|screen|authored` (which heuristic a LOD root naming several measures by; distance first by default), the LOD tuning (view-local, never saved: `lod-thresholds <root> [h=<i>] <t>…` and `lod-blend-thresholds …` for every placement of that authored root, `lod-override-reset [<root>]`, `lod-bias <f>`, `lod-hysteresis <fraction>`, `lod-blending on|off`), `lod-show labels|gizmo|gizmo-all|flash on|off`, `lod-show color level|depth` (the LOD tool's layers; `view lodlevel` is the level view; the `lodtool` record inspects the root at or above the selection, `view.lod.inspect {root}` any root), `fly <camera path prefix> <speed m/s>` (fly the camera through the scene's cameras with that prefix, in path order: `fly /cameras/flight_ 30` on the streaming test world; the script holds while it flies; a measurement, its numbers depend on build, cache and machine), `fly-stop`, the debug camera (`debug-camera on|off|place`, `debug-camera control main|debug`, `debug-camera lod|cull|shadows main|debug`, `debug-camera trail on|off|clear`: the view renders from it while the main camera, which `camera`, `camera-use` and `fly` move, drives LOD and streaming, culling and the shadow cascades, each switchable to the debug camera; `camera x,y,z,yaw,pitch debug` places it; the `status` record's `debugCamera`), the camera modes (`camera-follow on|off`, `camera-follow height <m>`: the eye kept that height above the highest drawn surface under it; `camera-orbit x,y,z yaw pitch [debug]`; the `status` record's `cameraModes`), `streaming-reset` (zero the `streaming` record's measured totals), the Streaming tool (`window streaming`, `streaming-show bounds|rings|rings-all|labels on|off`, `debugview streaming`, `lod-unload-thresholds <root> [h=<i>] <t>…`; the `streamingtool` record inspects the streaming root at or above the selection, `view.streaming.inspect {root}` any root; `--show-ui` puts the windows in screenshots) and `cull on|off|freeze|unfreeze|show|hide`
  (frustum culling toggle, frozen frustum, red/green AABB overlay), `prepass on|off` (depth prepass), `occlusion on|off` (two-phase occlusion culling, on by default; runs the prepass, from the culling camera into its own depth while a debug camera renders or the frustum is frozen), `aa on|off` (anti-aliasing; off to compare images pixel for pixel, a LOD fade's dither unblurred) and
  `ambient sky=r,g,b ground=r,g,b|reset` (hemisphere ambient), `gpuscene budget=<MiB>,limit=<MiB>` (upload budget, geometry pool limit),
  `sampler aniso=8,bias=0,minlod=0,mip=linear|nearest` (material sampler), `shadow cascades=3,tile=1024,lambda=0.5,pcf=on|off,distance=200`
  (cascaded shadows) — these set configuration values; `config <name> <value>` sets any (`config render.shadow.cascades 1`),
  `config-reset [<name or prefix>]` puts values back to their defaults, and the `config` record lists every value with its default and
  description, `inspect <texture> <level>|off` (texture inspector
  capture) and `dump-texture <texture> <level> <path>` (one mip level as PNG; the texture index as the `render` record's `textures` list it); several screenshots in one run.
- `--fail-on-validation`: exit code 2 if the validation layer reported anything. `--geometry-limit=<MiB>`: cap each geometry pool buffer
  below the device's limit, to exercise a full pool on a small scene (the `GeometryPoolFull` warning, the `memory` record's `geometryPool`). `--render-debug`: draw log on without the window.
- Introspection verbs (debug and release builds; each writes when its data arrives, a few frames after the verb):
  - `capture <pass|-|+> <resource> <path> [x y w h]`: any frame-graph resource right after `<pass>` (`-` = end of frame, also `gpuscene.meshtable`,
    `gpuscene.materials`; `+` = after the pass that writes it last this frame, named in the result). Buffers as JSON rows decoded with their schema; textures as PNG plus `<path>.json` (format, size, channel ranges, and the
    texel values when the region is 64 texels or fewer).
  - `dump-frame DIR`: `DIR/frame.json` (every pass with barriers and their Vulkan stages/layouts, command log, descriptor contents) plus a capture of
    every resource each pass writes.
  - `dump-gpuscene DIR`: the GPU scene tables and culling buffers, and `DIR/instances_joined.json` (per instance: prim, mesh, material, bounds,
    visibility and cull plane per view).
  - `debugview off|wireframe|trianglesize|overdraw|instance|mesh|material|primitive|uv|streaming|meshlet|meshletcull`: replaces the lit image (meshletcull: each meshlet by the test that culls it, red value 0 kept, 1 frustum, 2 cone, 3 depth); read raw values with
    `capture DebugViewPass debugview.value <path> X Y W H`.
  - `cull view N`: colour the AABB overlay by view N's visibility (0 camera, 1+ cascades); `overlay cascadefrusta=on` draws the cascade frusta.
  - `window memory|capture|framedebugger|gpuscene|counters|shaders|culling|assets|lod|streaming|camera|renderdebug|framegraph|performance on|off`: open a window, to
    exercise its drawing headless; with `--show-ui` the screenshot shows it.
  - `renderdoc-capture` with `--renderdoc`: a RenderDoc capture of the next frame under `captures/`.
- Where the numbers are: the device and swapchain, scene tables, passes and draws in `render`; camera, anti-aliasing and sampler in `status`;
  instances drawn and culled per view in `culling`; CPU and GPU frame times in `profile`; GPU zones and pipeline statistics in `counters`
  (asking for it turns them on; it answers a few frames later); every buffer and texture in `memory`.
- Culling runs on the GPU (`src/renderer/README.md`). The `culling` record, per-pass `draws`/`primitives` and the draw log are read back and
  lag a few frames: record them at steady state (frame 120 or later), not right after a camera or scene change.

## Live investigation over RPC

A running ngen-view answers RPC calls (`src/rpc/README.md`). Every script verb is also a method, and records return their JSON directly:

```sh
SDL_VIDEODRIVER=offscreen ./ngen-cli view <scene> &          # keeps running; no --frames
./ngen-cli introspect list                                     # wait until it's listed; every process and its records
./ngen-cli introspect get view status                          # frame, scene, selection, camera, purposes
./ngen-cli introspect get view culling                         # also scene, assets, profile, render, memory, counters
./ngen-cli introspect describe view                            # every method with its parameters
./ngen-cli introspect call view view.camera.set '{"x":5.3,"y":11.3,"z":1.2,"yaw":-169.8,"pitch":-0.2}'
./ngen-cli introspect call view view.screenshot '{"path":"/tmp/shot.png"}'   # answers when the file is written
./ngen-cli introspect call view introspect.gpuscene            # GPU-native data stays a method; also introspect.frame
./ngen-cli introspect call view capture.request '{"pass":"GeometryPass","resource":"gbuffer.normal"}'
./ngen-cli introspect call view view.quit
```

- Offline `--script` runs stay the default for reproducible verification; live calls are for investigating interactively.
- With several views running, `view` is ambiguous: use `view:<pid>` from `ngen-cli introspect list`.
- Exit codes: 0 ok, 1 the call returned an error (printed as JSON), 2 no endpoint.

## Inspect

Read the records and the trace with `jq`. Typical checks:

```sh
jq -c 'select(.level != "info") | {process, level, type, text}' /tmp/trace.jsonl   # warnings and errors; empty on a clean run
jq -r '[.process, .type, .text] | @tsv' /tmp/trace.jsonl                            # the run's flow
jq -c 'select(.record == "render") | [.value.textures[] | "\(.width)x\(.height)"] | group_by(.) | map({(.[0]): length}) | add' /tmp/records.jsonl
```

If the value under test is not in a record, add it to one (`registerViewRecords` in `src/view/viewcommands.cpp`). If a step of the flow or
a failure is not visible in the trace, add a `TRACE_EVENT`, `TRACE_WARNING` or `TRACE_ERROR` at that point (conventions in
`src/trace/README.md`: flow and messages, not data; stable field values, no pointers/handles), rebuild, rerun. Events added for a change stay
in the code — there is no "remove when done" step.

## Test scenes

- Minimal: `assets/three_cubes.usda` — cheap smoke test for extraction, lighting, frame graph.
- Composition: `assets/layered/root.usda` — sublayers, references (one instanced, one internal), a payload, a UsdLod root, bindings on a
  parent and a child, visibility, a guide, a texture. Default: 10 draws; `purposes guide=on` 11; `lod /World/Tree/lod 0` 12.
- Materials/textures stress test: Intel NewSponza at `assets/main_sponza/NewSponza_Main_USD_Zup_003.usda` (git-ignored; scenes must be
  under the asset server's directory, since they are assets). Correct result: the `render` record's `textures` hold 25 base colours at
  4096×4096, 3 at 1×1 (materials without a diffuse map) and the decals' 4096×4096 opacity mask (`opacity: true`), 29 in all, and
  `SceneUploaded` says 29 textures; `cutoutMaterials` 1, `cutoutInstances` 4. Byte-identical screenshots of three_cubes and
  Sponza at the default camera (`--frames=80`, headless) are the check that the scene path draws what it always drew. All-1×1 means texturing is broken. It exercises GeomSubset per-face materials, NodeGraph-wrapped textures, backslash
  asset paths, and indexed faceVarying primvars.
- Large scene: Caldera at `assets/caldera/caldera.usda` (git-ignored; its own checkout, the UsdLod-converted fork on branch `usdlod`). It
  opens its default lightweight map. Its first open after a cache wipe or a move packs thousands of files: give the run minutes, not seconds,
  and watch the server's `status` record (`tasksQueued`, `packerRuns`).

## Machine constraints

This machine is Wayland-only — X11 screenshot tools (`import` etc.) cannot capture the Vulkan window.
When visual confirmation is genuinely needed, ask the user to look rather than trying to capture it.
