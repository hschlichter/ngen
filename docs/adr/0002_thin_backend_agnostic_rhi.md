# 2. Thin backend-agnostic RHI, backends in sibling folders

Date: 2026-04-01

## Status

Accepted

Amended by [ADR 19](0019_rhi_executes_engine_schedules.md): the RHI's principles (no hidden waits, explicit uploads, caller-owned state).

Amended by [ADR 20](0020_no_window_layer_in_rhi.md): no window library in the RHI.

## Context

ngen is a learning engine that may one day target D3D12 or Metal next to Vulkan. Renderer code should not depend on Vulkan, and the graphics API
should sit behind one layer that a second backend can implement. The reference notes argued for a layered stack — engine, render graph, RHI, backend —
built by composition rather than deep inheritance.

## Decision

- `RhiDevice`, `RhiCommandBuffer` and `RhiSwapchain` are abstract classes in header-only `src/rhi/`.
- Each backend lives in a sibling folder (`src/rhi/vulkan/`, later `src/rhi/d3d12/`) and is the only code that includes its API's headers. The build
  links one backend per platform.
- There is no factory: the application constructs the concrete device and hands `RhiDevice*` down to everything else.
- The RHI's vocabulary has no engine concepts — no scene, material, frame graph or ImGui.
- Errors are returned as `std::expected<T, RhiError>`.

Alternatives considered:

- A runtime factory or an `#ifdef` chain choosing the backend: rejected, because the build already selects one backend per platform.
- Generational handles and managers, as the reference notes proposed: deferred until a second backend makes the cost worth it. The interface uses raw
  pointers with `createX` / `destroyX`.

## Consequences

- Renderer code is free of backend headers, and the engine and the RHI examples construct the backend the same way.
- The descriptor model and the barriers are still Vulkan-shaped; that is admitted, and revisited when a second backend exists.
- Raw pointers carry no generation, so a stale pointer (for example `swapchain->image(i)` after `recreate`) is not caught.
- The ImGui Vulkan backend downcasts to `RhiDeviceVulkan`, outside the RHI, at application level.

## Verification

- Sources: [modern_multi_backend_renderer_architecture_c.md](../modern_multi_backend_renderer_architecture_c.md),
  [plan_rhi_examples.md](../plan_rhi_examples.md), [src/rhi/README.md](../../src/rhi/README.md).
- Code:
  - `src/rhi/rhidevice.h:23` (`class RhiDevice`), `src/rhi/rhitypes.h:104` (`enum class RhiError`).
  - `src/apps/view.cpp:352` constructs `RhiDeviceVulkan rhiDevice;` directly.
  - `src/imguibackendvulkan.cpp:14` downcasts to `RhiDeviceVulkan`.
- Differs from plan:
  - No second backend exists yet; handles and managers from the reference notes were not adopted.
  - `src/rhi/README.md:55` says the SDL glue is in `src/main.cpp`; it is in `src/apps/view.cpp:50` (`makeRhiWindowSdl`).
