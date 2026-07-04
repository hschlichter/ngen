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
