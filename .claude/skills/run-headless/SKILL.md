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
./_out/ngen-build -p linux-vulkan -c debug
```

If `_out/ngen-build` is missing (fresh clone), bootstrap first:

```sh
mkdir -p _out && c++ -std=c++23 -O0 -g -pthread -o _out/ngen-build build/bootstrap.cpp
```

The binary lands at `_out/linux-vulkan/debug/ngen-view`.

## Run headless

```sh
SDL_VIDEODRIVER=offscreen timeout --signal=TERM 5 ./_out/linux-vulkan/debug/ngen-view --obs-output=/tmp/obs.jsonl <scene>
```

- The `timeout` kill is the expected exit — judge the run by the JSONL contents, not the exit code.
- Write `--obs-output` to `/tmp` or the session scratchpad, not into the repo.
- Size the timeout to the scene: 3–5 s for small scenes, 45+ s for Sponza (4K PNG decode takes ~30–40 s
  before textures appear).

## See and drive the frame

`ngen-view` takes session flags so a run can be looked at, not only grepped:

```sh
SDL_VIDEODRIVER=offscreen ./_out/linux-vulkan/debug/ngen-view <scene> --frames=30 \
  --camera=2,1.5,2,-135,-20 --view=normals --overlay=grid=off \
  --screenshot=/tmp/shot.png --dump-render-debug=/tmp/rd.json --dump-profile=/tmp/trace.json \
  --fail-on-validation --obs-output=/tmp/obs.jsonl
```

- `--screenshot=PATH` writes the presented frame as PNG on the last frame of `--frames`; Read the PNG to see it.
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
  - `window memory|capture|framedebugger|gpuscene|counters on|off`: open an introspection window, to exercise its drawing headless.
  - `renderdoc-capture` with `--renderdoc`: a RenderDoc capture of the next frame under `captures/`.
- Observations `DeviceInfo` (startup), `CameraPose` (every 60 frames), `Screenshot` (per shot) complement `FrameStats`, `RenderStats`
  (`instances`, `culled`, `cascades`, `shadow_culled`, `draws`, `primitives`), `GpuTime`.
- Culling runs on the GPU (`src/renderer/README.md`). `culled`, `shadow_culled`, per-pass `draws`/`primitives`, the draw log and `CullReadback`
  are read back and lag a few frames: judge them at steady state (frame 120 or later), not right after a camera or scene change.

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
- Materials/textures stress test: Intel NewSponza at
  `~/Downloads/main_sponza/NewSponza_Main_USD_Zup_003.usda`. Correct result: ≈25 `TextureUploaded` events
  at 4096×4096 plus a few 1×1 (materials without a diffuse map), 28 unique materials. All-1×1 means
  texturing is broken. It exercises GeomSubset per-face materials, NodeGraph-wrapped textures, backslash
  asset paths, and indexed faceVarying primvars.

## Machine constraints

This machine is Wayland-only — X11 screenshot tools (`import` etc.) cannot capture the Vulkan window.
When visual confirmation is genuinely needed, ask the user to look rather than trying to capture it.
