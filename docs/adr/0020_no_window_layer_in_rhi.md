# 20. No window layer in the RHI (`RhiWindow`)

Date: 2026-09-19

## Status

Accepted

## Context

The Vulkan backend first used SDL directly to get its instance extensions and create its surface. That tied every program using the RHI to SDL, and
mixed two concerns in one library: talking to the graphics API, and owning a window. The engine, the RHI examples and headless runs each own their
window differently, or have none on screen at all.

A backend should know its graphics API and nothing else. A future D3D12 or Metal backend would need an HWND or a `CAMetalLayer`, not an SDL
window.

## Decision

Nothing under `src/rhi/` includes SDL, GLFW, xlib, wayland or win32. The window owner fills in an `RhiWindow` and passes it to `RhiDevice::init`:

- `instanceExtensions` — the instance extensions the window layer needs (Vulkan; unused by other APIs).
- `createSurface(nativeInstance, nativeSurface)` — a callback that creates the surface from opaque, backend-native handles.

The swapchain extent is passed in by the caller; the backend never asks the window for its size. ngen-view's SDL glue is one static function in
its `main`; the examples have their own copy in `src/rhi/examples/common/`.

## Consequences

- Easier: the backend is window-library agnostic, and a second backend needs no change to the contract beyond what its `createSurface` returns.
- Easier: the examples and the engine each own their window glue, so the examples depend on `src/rhi/` alone.
- Easier: offscreen runs (`SDL_VIDEODRIVER=offscreen`) and other window owners need no RHI change.
- Harder: the SDL glue exists twice, in ngen-view and in the examples' common code, by choice.
- Harder: a surfaceless device was considered for tests and dropped; every RHI program still creates a window, even offscreen.

## Verification

- Sources: [src/rhi/README.md](../../src/rhi/README.md) "No window layer in the RHI", [plan_rhi_examples.md](../plan_rhi_examples.md),
  [plan_rhi_validation.md](../plan_rhi_validation.md) (the surfaceless device, superseded).
- Code: `src/rhi/rhiwindow.h:16` (`struct RhiWindow`), `src/apps/view.cpp:50` (`makeRhiWindowSdl`). `rg SDL src/rhi --glob '!examples/**'` finds only
  comments and README text.
- Differs from plan: `src/rhi/README.md:55` says the SDL glue is "a static function in `src/main.cpp`"; it is in `src/apps/view.cpp`.
