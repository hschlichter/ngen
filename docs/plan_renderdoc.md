# RenderDoc integration

**Status. Landed.**

Stage 8 of [plan_introspection.md](plan_introspection.md), done second because it is small and stage 1's debug names make it readable.

## Current state

RenderDoc can be attached from outside (launch ngen-view from `qrenderdoc`), but the engine does nothing to help: there is no in-app capture trigger,
no capture from a script, and before stage 1 no object had a name. RenderDoc (`librenderdoc.so`, `renderdoc_app.h`, `renderdoccmd`, `qrenderdoc`) is
installed on this machine.

## Scope

**In**

- `src/renderdoccapture.h/.cpp`: load the RenderDoc in-app API before the Vulkan instance exists.
  - Use the library when RenderDoc already injected it (launched from `qrenderdoc`), or load `librenderdoc.so` when ngen-view is started with
    `--renderdoc`.
  - Set the capture path template (`captures/ngen`).
  - Trigger a capture of the next frame, report captures taken, open the latest in `qrenderdoc`.
- **Triggers:** Debug menu "Capture Frame (RenderDoc)" and "Open Last Capture", and the session verb `renderdoc-capture`.
- **Observation:** a `RenderDocCapture` event with the capture's path when one completes.
- **Build:** the integration compiles only with `NGEN_INTROSPECTION`, defined for `debug` and `release`, not `gamerelease`. This is umbrella
  decision 4, and this is the first code behind the define.

**Out**

- Programmatic capture ranges (`StartFrameCapture`/`EndFrameCapture` around parts of a frame). Whole-frame capture covers learning use.
- Other tools (Nsight, Radeon GPU Profiler). The same debug names serve them.

## Decisions

Made while planning, following the umbrella's recommendations.

- **Opt-in loading.** Loading `librenderdoc.so` installs RenderDoc's Vulkan layer for the whole run, which changes timings and behaviour. So it happens
  only with `--renderdoc`, or when RenderDoc already injected itself.
- **The header is vendored, not the library.** `renderdoc_app.h` (MIT) is copied into `external/renderdoc/`, since it is the stable API contract. The
  library is resolved at runtime with `dlopen`, so ngen-view neither links it nor needs it. This is a single-file copy rather than the usual submodule;
  RenderDoc's repository is large and only this header is needed.
- **Lives in `src/`, not `src/rhi/`.** RenderDoc is a tool around the whole application; the RHI stays unaware of it (`src/rhi/README.md`,
  principles).

## Steps

1. `external/renderdoc/renderdoc_app.h`; `-Iexternal/renderdoc` for the ngen-view target; `NGEN_INTROSPECTION` in the `debug` and `release`
   configurations.
2. `RenderDocCapture` with `load(bool forceLoad)`, `available()`, `triggerCapture()`, `pollNewCaptures()` (paths of captures finished since the last
   call), and `openInUi(path)`.
3. `main.cpp`: load before `RhiDeviceVulkan::init`; the flag, verb and menu actions; poll once per frame and emit `RenderDocCapture`.

## Verification

- `SDL_VIDEODRIVER=offscreen ngen-view three_cubes.usda --renderdoc --script` with `10 renderdoc-capture` writes one `.rdc` under `captures/`, and
  emits `RenderDocCapture` with that path.
- `renderdoccmd convert -f <capture> -c xml` output contains the stage 1 debug names: `gpuscene.instances`, `geometry.pipeline.cullback.less`,
  `material.N.basecolor`, `cull.commands`, the shader file names.
- Without `--renderdoc` (and without injection) the verb prints that RenderDoc is not loaded, and the run is otherwise unchanged. Screenshots stay
  byte-identical.

## Results

- `--renderdoc` with `10 renderdoc-capture` on three_cubes wrote `captures/ngen_frame8.rdc` and emitted `RenderDocCapture` with its path.
- `renderdoccmd convert -c xml` output contains the debug names (`gpuscene.instances`, the pipeline names, `material.N.basecolor`, `cull.commands`,
  shader file names).
- Without `--renderdoc` the verb reports that RenderDoc is not loaded; screenshots byte-identical.
