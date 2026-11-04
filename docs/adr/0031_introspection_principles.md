# 31. Introspection: dump parity, one capture service, own debugger plus RenderDoc

Date: 2026-09-26

## Status

Accepted

Amended by [ADR 46](0046_records_and_ngen_introspect.md): CPU-side data became records on the RPC endpoint; dumps remain for GPU-native data only.

## Context

GPU-driven rendering (ADR 26) removed per-draw timing and moved culling out of the CPU's sight. The engine exists to learn from: every resource,
barrier, draw and byte of GPU memory should be something a human can see and an agent can read. Several inspectors were about to be built at once —
debug names and memory, frame capture, a frame debugger, a GPU scene inspector, counters, debug views, RenderDoc — and needed common rules.

## Decision

- **Parity.** Every inspector has a UI view, a headless dump and a verb, so an agent can read what a human sees.
- **One capture service.** The frame graph captures any resource after any pass on a chosen frame; inspectors ask it instead of adding their own
  readbacks.
- **Both debuggers.** An own frame debugger for engine semantics (passes, barriers with Vulkan details, the command log, descriptor contents) and
  RenderDoc's in-app API for GPU depth. They answer different questions.
- **Free when off.** Nothing is recorded or read back unless asked for.
- **Names.** Every GPU object carries a debug name; an allocation registry backs a Memory window.
- **Shipping.** The tooling is compiled out of `gamerelease` only, behind one define.
- Separate windows that link to each other, rather than one merged GPU inspector.

## Consequences

- Easier: every GPU-side inspector is scriptable, and new inspectors reuse one capture and readback path.
- Harder: each view needs a JSON writer and a verb, about a fifth more work per view.
- Harder: "free when off" and "compiled out" hold only in part (see Verification).

## Verification

- Sources: [plan_introspection.md](../plan_introspection.md) and its children: [plan_debug_names_and_memory.md](../plan_debug_names_and_memory.md),
  [plan_renderdoc.md](../plan_renderdoc.md), [plan_frame_capture.md](../plan_frame_capture.md), [plan_frame_debugger.md](../plan_frame_debugger.md),
  [plan_gpu_scene_inspector.md](../plan_gpu_scene_inspector.md), [plan_gpu_counters.md](../plan_gpu_counters.md), [plan_debug_views.md](../plan_debug_views.md).
- Code: `src/renderer/capture.h:97` (`CaptureService`); `src/renderer/framedebug.*`, `gpucounters.*`, `gpuscenecapture.*`; `build.cpp:64` and `:73`
  (`NGEN_INTROSPECTION` in debug and release, absent from `gamerelease`); `src/renderdoccapture.cpp:7`, `src/apps/view.cpp:476`.
- Differs from plan:
  - `NGEN_INTROSPECTION` gates only RenderDoc and the RPC endpoint; the windows, capture service and frame debugger are still compiled into `gamerelease`.
  - The per-instance visibility readback runs every frame (`src/renderer/drawlists.h:24`, `cameraVisible`), so not everything is free when off.
  - The shared `InspectorSelection` between windows was never built.
