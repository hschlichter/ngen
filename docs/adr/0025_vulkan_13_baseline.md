# 25. Vulkan 1.3 baseline with required features, no fallbacks

Date: 2026-09-25

## Status

Accepted

## Context

The frame graph (ADR 5) had already moved to dynamic rendering in its first phase: render-pass and framebuffer objects made every new pass
boilerplate. GPU-driven rendering (ADR 26) then needed `multiDrawIndirect`, `drawIndirectFirstInstance`, `drawIndirectCount` and dynamically uniform
indexing into sampler arrays (ADR 28).

Each of these could be probed for and given a fallback path, but every fallback doubles the passes that use it, and a learning engine gains little
from supporting old drivers.

## Decision

- The device must report API version 1.3 or later, so `synchronization2` and `dynamicRendering` are core and enabled without extensions.
- `shaderSampledImageArrayDynamicIndexing`, `multiDrawIndirect`, `drawIndirectFirstInstance` and `drawIndirectCount` are required. `init` fails with
  a message naming the missing feature.
- Optional features — wide lines, anisotropy, pipeline statistics, geometry shaders, memory budget, calibrated timestamps — are enabled when present
  and reported in `limits()`; the engine checks before using them.
- Pipelines take colour and depth formats; there are no render-pass objects.
- No extension fallbacks and no CPU-driven path. Going beyond 1.3 or adding an optional extension is an explicit decision (the one approved so far is
  `VK_EXT_mesh_shader`, ADR 64).

## Consequences

- Easier: one code path per pass, and a clear failure on devices that cannot run it.
- Easier: barriers use `vkCmdPipelineBarrier2` throughout.
- Harder: pre-1.3 drivers are excluded.
- Harder: every new optional feature needs a decision and a reported limit, not just a code change.

## Verification

- Sources: [frame_graph_phase1_summary.md](../frame_graph_phase1_summary.md), [plan_gpu_driven.md](../plan_gpu_driven.md),
  [plan_bindless_materials.md](../plan_bindless_materials.md) decision 1, [plan_indirect_draws.md](../plan_indirect_draws.md) decision 5,
  [src/rhi/README.md](../../src/rhi/README.md) "Required features".
- Code: `src/rhi/vulkan/rhidevicevulkan.cpp:493` (dynamic indexing), `:500` (multi-draw and first instance), `:511` (`drawIndirectCount`), `:524`
  (API 1.3 or fail).
- Differs from plan: [plan_indirect_draws.md](../plan_indirect_draws.md) and [plan_gpu_driven.md](../plan_gpu_driven.md) call 1.2 the baseline;
  the code and the RHI README require 1.3.
