# GPU timestamps

**Status. Landed.**

## Current state

No GPU timing anywhere. The frame graph debugger shows pass order and resources but not cost, so decisions like "is async compute worth it"
(`src/rhi/README.md` group 1) have no numbers behind them. The RHI has no query API.

## Scope

**In**

- RHI: `RhiQueryPool`, `createQueryPool(count)`, `destroyQueryPool`, `readTimestamps(pool, first, outTicks) -> bool` (never blocks; false when
  results are not available), `cmd->resetQueryPool`, `cmd->writeTimestamp`; `RhiDeviceLimits::timestamps` and `timestampPeriodNs`.
- Frame graph: optional timestamp pool; `execute` resets it and brackets every executed pass with two timestamps. `executedPassNames()`
  exposes the order so the reader can label results after the frame completes.
- Renderer: one pool per frame slot, read back when the slot's fence has signalled (results lag by frames in flight), per-pass and whole-frame
  GPU milliseconds in `FrameGraphDebugSnapshot`, shown in the frame graph window, plus a `GpuTime` observation per completed frame.
- Example `timestamps`: brackets a heavy dispatch and a draw, prints times, asserts plausibility.

**Out**

- Pipeline statistics queries, occlusion queries. CPU-side frame timing (already visible through obs timestamps).
- Nested or per-draw timing inside a pass.

## Decisions

- **Two timestamps per pass, whole pool reset per frame.** Simplest correct shape; the pool is sized for 32 passes and timing is silently skipped
  if a frame exceeds it. `ALL_COMMANDS` stage for both writes: conservative, and the number wanted is wall time of the pass on the queue, not
  a stage-specific one.
- **Read without waiting.** `readTimestamps` uses availability bits and returns false rather than blocking, so a never-written query cannot hang
  the caller. The renderer only reads after the slot's fence, where results are always available.
- **Command-buffer reset, not host reset.** `vkCmdResetQueryPool` needs no feature; host reset would need `hostQueryReset` enabled.

## Verification

- Headless three_cubes run: `GpuTime` events with `gpu_ms` between 0 and 100 on every frame after the first frames in flight; validation
  clean. Verified: 18203 frames, GPU frame time min 1.22 ms, median 1.24 ms, max 1.62 ms at 2560x1440 offscreen.
- `ngen-example-timestamps --frames=60 --check --validation` exits 0: compute time positive and below a second, draw time likewise, compute
  slower than the draw, output grey matches the fixed point of the shader's iteration.
- Frame graph window shows "GPU x.xx ms" in the header and a per-pass GPU time in the pass detail pane (Henrik, visual).

## Deferred / follow-ups

- Per-pass GPU time as a column in the pass list, and a small history graph. Trigger: first time the number is used to make a decision.
- Async compute plan (`docs/plan_rhi_queues.md`) once these numbers show GPU idle worth recovering.
