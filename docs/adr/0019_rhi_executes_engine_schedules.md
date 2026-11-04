# 19. The RHI executes, the engine schedules

Date: 2026-09-19

## Status

Accepted

## Context

`copyBuffer` called `vkQueueWaitIdle` on every call, so uploading a scene stalled the GPU once per buffer. Hidden waits and hidden caching inside the
RHI hide costs that the engine, which knows its own frame pacing, should own. The RHI's interface ([ADR 2](0002_thin_backend_agnostic_rhi.md)) also
needed a synchronisation model that a second backend could map onto.

## Decision

- `destroyX` destroys immediately. The engine defers destruction itself, in a deletion queue keyed by the frame's fence.
- Uploads are commands recorded on a command buffer the caller owns. There is no initial-data argument and no `RhiDevice::copyBuffer`.
- The RHI has no notion of a frame; fences and semaphores are its only synchronisation objects.
- Synchronisation is expressed as resource-state transitions (`RhiTextureState`, `RhiBufferState`); the backend derives pipeline stages and access
  masks. The caller tracks each resource's current state.

Alternatives considered:

- Stage and access masks in the interface: rejected in favour of states, which map directly onto D3D12's resource states.
- A separate compute pipeline type: rejected.
- A storage-image layout field: rejected; the layout follows from the descriptor type.

## Consequences

- Costs are visible at the call site, and barriers are portable in intent.
- Shader-read states cover all stages, which is conservative.
- No split barriers and no queue ownership transfers.
- Every integrator must write a deletion queue and an upload path; the renderer and the RHI examples each do.

## Verification

- Sources: [src/rhi/README.md](../../src/rhi/README.md) "Principles", [plan_rhi_compute.md](../plan_rhi_compute.md).
- Code:
  - `src/rhi/rhitypes.h:238` (`RhiTextureState`), `:251` (`RhiBufferState`).
  - `src/rhi/vulkan/rhicommandbuffervulkan.cpp:353` (`vkCmdPipelineBarrier2` from derived masks).
  - `src/renderer/deletionqueue.h:15` (`class DeletionQueue`).
  - `vkQueueWaitIdle` no longer appears in the RHI's code.
