---
name: run-headless
description: Build and run ngen-view headless with the observation bus to verify engine behavior. Use when running the app, confirming a change works, or checking rendering/scene/material behavior — headless obs-bus runs are this project's primary verification loop (the app is a Vulkan window; this machine cannot screenshot it).
---

# Run ngen headless and read the evidence

The verification loop for engine changes: build, run headless with the observation bus, read the JSONL
evidence, judge whether the intended behavior actually happened. Prefer this over screenshots or "it
compiles".

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
SDL_VIDEODRIVER=offscreen timeout --signal=TERM 5 ./ngen-cli view --obs-output=/tmp/obs.jsonl <scene>
```

Rebuild and restart the asset server after changing `pack.cpp` or anything under `src/asset/server/`. `./ngen-cli rpc call asset server.status`
shows what it is doing; with servers for several variants running, name one by pid (`asset:<pid>`, from `./ngen-cli rpc list`).

- `ngen-cli view` replaces itself with the set variant's `ngen-view`, so `timeout`, signals and exit codes behave exactly as with the binary.
- The `timeout` kill is the expected exit — judge the run by the JSONL contents, not the exit code.
- Write `--obs-output` to `/tmp` or the session scratchpad, not into the repo.
- Size the timeout to the scene: 3–5 s for small scenes, 45+ s for Sponza (4K PNG decode takes ~30–40 s
  before textures appear).

## See and drive the frame

`ngen-view` takes session flags so a run can be looked at, not only grepped:

```sh
SDL_VIDEODRIVER=offscreen ./ngen-cli view <scene> --frames=30 \
  --camera=2,1.5,2,-135,-20 --view=normals --overlay=grid=off \
  --screenshot=/tmp/shot.png --dump-render-debug=/tmp/rd.json --dump-profile=/tmp/trace.json \
  --fail-on-validation --obs-output=/tmp/obs.jsonl
```

- `--screenshot=PATH` writes the presented frame as PNG on the last frame of `--frames`; Read the PNG to see it. Screenshots leave the UI out
  (the menu bar, windows and the status bar, whose numbers change every run), so they compare byte for byte; `--show-ui` keeps it in.
- `--view=lit|albedo|normals|depth|shadowfactor|shadowmap|shadowuv|worldpos|miplevel|cascades` (miplevel: red level 0 to white
  level 7+; cascades: red, green, blue, yellow near to far; shadowmap shows the cascade atlas), `--overlay=grid=on,aabbs=off,...`
  (grid, origin, gizmo, aabbs, lightgizmos, buffer, shadow, aa), `--camera=x,y,z,yaw,pitch`, `--camera-frame=scene|/prim`, `--select=/prim`.
- `--dump-render-debug=PATH`: meshes, textures, passes with draw counters, draw log with prim paths, as JSON.
  `--dump-profile=PATH`: profiler history as Chrome trace JSON (`jq '.traceEvents'`, or open in Perfetto).
- `--script=FILE`: `<frame> <verb> [args]` per line, same verbs as the flags plus `quit`, `translate /prim dx,dy,dz` (preview transform
  edit, no layer write) and `cull on|off|freeze|unfreeze|show|hide`
  (frustum culling toggle, frozen frustum, red/green AABB overlay), `prepass on|off` (depth prepass) and
  `sampler aniso=8,bias=0,minlod=0,mip=linear|nearest` (material sampler), `shadow cascades=3,tile=1024,lambda=0.5,pcf=on|off`
  (cascaded shadows), `inspect <material> <level>|off` (texture inspector
  capture) and `dump-texture <material> <level> <path>` (one mip level as PNG); several screenshots in one run.
- `--fail-on-validation`: exit code 2 if the validation layer reported anything. `--render-debug`: draw log on without the window.
- Introspection verbs (debug and release builds; each writes when its data arrives, a few frames after the verb):
  - `capture <pass|-> <resource> <path> [x y w h]`: any frame-graph resource right after `<pass>` (`-` = end of frame, also `gpuscene.meshtable`,
    `gpuscene.materials`). Buffers as JSON rows decoded with their schema; textures as PNG plus `<path>.json` (format, size, channel ranges, and the
    texel values when the region is 64 texels or fewer).
  - `dump-frame DIR`: `DIR/frame.json` (every pass with barriers and their Vulkan stages/layouts, command log, descriptor contents) plus a capture of
    every resource each pass writes.
  - `dump-gpuscene DIR`: the GPU scene tables and culling buffers, and `DIR/instances_joined.json` (per instance: prim, mesh, material, bounds,
    visibility and cull plane per view).
  - `dump-counters PATH`: one frame's GPU zones (passes and per-region indirect calls) and pipeline statistics per pass.
  - `dump-memory PATH` (also `--dump-memory=PATH`): every allocation and heap.
  - `debugview off|wireframe|trianglesize|overdraw|instance|mesh|material|primitive|uv`: replaces the lit image; read raw values with
    `capture DebugViewPass debugview.value <path> X Y W H`.
  - `cull view N`: colour the AABB overlay by view N's visibility (0 camera, 1+ cascades); `overlay cascadefrusta=on` draws the cascade frusta.
  - `window memory|capture|framedebugger|gpuscene|counters|shaders|culling on|off`: open an introspection window (or the Culling window), to
    exercise its drawing headless; with `--show-ui` the screenshot shows it.
  - `renderdoc-capture` with `--renderdoc`: a RenderDoc capture of the next frame under `captures/`.
- Observations `DeviceInfo` (startup), `CameraPose` (every 60 frames), `Screenshot` (per shot) complement `FrameStats`, `RenderStats`
  (`instances`, `culled`, `cascades`, `shadow_culled`, `draws`, `primitives`), `GpuTime`.
- Culling runs on the GPU (`src/renderer/README.md`). `culled`, `shadow_culled`, per-pass `draws`/`primitives`, the draw log and `CullReadback`
  are read back and lag a few frames: judge them at steady state (frame 120 or later), not right after a camera or scene change.

## Live investigation over RPC

A running ngen-view answers RPC calls (`src/rpc/README.md`). Every script verb is also a method, and the dumps return their JSON directly:

```sh
SDL_VIDEODRIVER=offscreen ./ngen-cli view <scene> &          # keeps running; no --frames
./ngen-cli rpc list                                            # wait until it's listed
./ngen-cli rpc describe view                                   # every method with its parameters
./ngen-cli rpc call view view.status                           # frame, scene, selection, camera
./ngen-cli rpc call view view.camera.set '{"x":5.3,"y":11.3,"z":1.2,"yaw":-169.8,"pitch":-0.2}'
./ngen-cli rpc call view view.screenshot '{"path":"/tmp/shot.png"}'   # answers when the file is written
./ngen-cli rpc call view introspect.gpuscene                   # also .render .memory .counters .frame
./ngen-cli rpc call view capture.request '{"pass":"GeometryPass","resource":"gbuffer.normal"}'
./ngen-cli rpc call view view.quit
```

- Offline `--script` runs stay the default for reproducible verification; live calls are for investigating interactively.
- With several views running, `view` is ambiguous: use `view:<pid>` from `ngen-cli rpc list`.
- Exit codes: 0 ok, 1 the call returned an error (printed as JSON), 2 no endpoint.

## Inspect

Read the stream with `jq`. Typical checks:

```sh
jq -r .name /tmp/obs.jsonl | sort | uniq -c        # what happened, by event
jq 'select(.name == "TextureUploaded")' /tmp/obs.jsonl
```

If the behavior under test is not visible in existing observations, add an `OBS_EVENT` at the decision
point (conventions in `obs.md`: stable field values, no pointers/handles, side-effect-free arguments),
rebuild, rerun. Observations added for a change stay in the code — there is no "remove when done" step.

## Test scenes

- Minimal: `assets/three_cubes.usda` — cheap smoke test for extraction, lighting, frame graph.
- Materials/textures stress test: Intel NewSponza at `assets/main_sponza/NewSponza_Main_USD_Zup_003.usda` (git-ignored; scenes must be
  under the asset server's directory, since they are assets). Correct result: ≈25 `TextureUploaded` events at 4096×4096 plus a few 1×1
  (materials without a diffuse map), 28 unique materials. All-1×1 means texturing is broken. It exercises GeomSubset per-face materials, NodeGraph-wrapped textures, backslash
  asset paths, and indexed faceVarying primvars.

## Machine constraints

This machine is Wayland-only — X11 screenshot tools (`import` etc.) cannot capture the Vulkan window.
When visual confirmation is genuinely needed, ask the user to look rather than trying to capture it.
