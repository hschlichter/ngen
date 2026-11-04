# 29. GPU culling with ordered compaction

Date: 2026-09-25

## Status

Accepted

Supersedes [ADR 22](0022_cpu_frustum_culling.md)

## Context

After the indirect-draw stage, command lists were still filled from the CPU's per-instance visibility masks (ADR 22), so the CPU still walked every
instance every frame. Moving culling to the GPU is the last stage of GPU-driven rendering (ADR 26).

Screenshots are compared byte for byte. Wherever depths tie, draw order decides the pixel, so the order of compacted draw commands must be stable from
run to run — which rules out the usual atomic-append compaction.

## Decision

Three compute passes per frame, over every view (the camera and each shadow cascade):

- `InstanceCull` tests each instance, counts survivors per region in each 256-instance workgroup, and writes visibility bits and the cull planes.
- `InstanceCullScan` turns the per-workgroup counts into offsets and totals.
- `InstanceCullScatter` writes each command at its workgroup's offset plus its local prefix sum, so commands stay in instance order.

Cascades draw whole meshes; a prim with a cutout submesh draws per submesh. Counters and visibility (and the commands, only while the debugger is
open) are read back after the slot fence. The CPU culling was deleted; the frozen frustum survives as a matrix the GPU culls with.

Rejected: atomic append (nondeterministic order changes pixels), single-workgroup compaction (does not scale), decoupled lookback (forward progress not
guaranteed everywhere). Async compute is deferred until GPU timestamps show idle gaps.

## Consequences

- Easier: deterministic output, byte-identical screenshots, and a design that scales to millions of instances.
- Easier: per-view cull planes and counters, including a region overflow counter, can be inspected.
- Harder: the CPU must mirror the shader's bucket rule to size regions; a disagreement is traced as `CullRegionOverflow`.
- Harder: culling is per instance only — no occlusion and no meshlets (ADR 64).

## Verification

- Sources: [plan_gpu_culling.md](../plan_gpu_culling.md), [src/renderer/README.md](../../src/renderer/README.md) "Culling and draw lists".
- Code: `src/renderer/passes/instancecullpass.cpp:127` (`InstanceCull`), `:141` (`InstanceCullScan`), `:152` (`InstanceCullScatter`);
  `shaders/instancecull.comp` uses `atomicAdd` only on shared and total counters, not for command placement (`:347` counts overflow);
  `src/renderer/culling.h:15` (`CullState`).
