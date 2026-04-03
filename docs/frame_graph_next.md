# Frame Graph: Future Phases

Ambitious features to add once the core frame graph (see `frame_graph_implementation_plan.md`) is stable and the renderer has enough passes to benefit. These are ordered roughly by expected need, not by difficulty.

---

## Multi-Queue Scheduling + Async Compute

**Problem**: All passes run on the graphics queue sequentially. Compute-heavy passes (SSAO, light culling, particle simulation) could overlap with graphics work on a separate hardware queue.

**What to build**:
- `FgQueue` enum: `Graphics`, `Compute0`, `Compute1` (extensible per hardware)
- `builder.setQueue(queue)` assigns a pass to a queue
- Compiler partitions passes by queue, inserts cross-queue synchronization
- Cross-queue sync through `RhiDevice` fence/semaphore abstraction

**RHI impact**:
- Multiple `VkQueue` handles in `RhiDeviceVulkan` (graphics + compute families)
- Multiple command pools/buffers per queue
- Fence-based synchronization between queue submissions

**Prerequisite**: The compiler must handle split dependency graphs — a compute pass on `Compute0` depends on a graphics pass's output, and a later graphics pass depends on the compute result. The topological sort must span queues.

**When needed**: When a compute-heavy pass (SSGI, light clustering, GPU culling) is bottlenecked behind graphics work.

---

## Named Priority Classes + Scheduling Barriers

**Problem**: Topological sort alone produces a valid ordering but not necessarily an optimal one. Independent passes that could overlap are arbitrarily ordered instead of being grouped for maximum GPU utilization.

**What to build**:
- Named priority enum covering the engine's pass categories:
  ```
  DepthPrepass, GBufferFill, ShadowGeometry, DeferredShading,
  ScreenSpaceGI, ScreenSpaceReflections, VolumetricLighting,
  Composition, Upscaling, PostProcessing, ...
  ```
- `builder.setSchedulingPriority(priority)` assigns a pass to a class
- Compiler uses priorities to order passes within a queue and identify overlap opportunities
- `builder.schedulingBarrier()` inserts a hard sync point (all prior passes must complete)

**Design notes**:
- 30-50+ priority classes is typical for a production renderer
- Scheduling barriers should be rare — only for full pipeline flushes (CPU readback, etc.)
- The scheduler should group passes sharing render targets to reduce state transitions

**When needed**: When the pass count is high enough (10+) that ordering within the topological sort matters for GPU performance.

---

## Parallel Command List Recording

**Problem**: Single-threaded command recording becomes a CPU bottleneck as pass count and draw call count grow.

**What to build**:
- `builder.setNumJobs(n)` splits a pass's execute lambda across `n` worker threads
- Each worker records into a secondary command buffer (Vulkan) or deferred command list
- After all workers complete, primary command buffer executes the secondary buffers in order
- Thread pool / job system for worker dispatch

**RHI impact**:
- Secondary command buffer support in `RhiCommandBuffer`
- `vkCmdExecuteCommands` for secondary buffer submission
- Thread-safe command pool allocation (one pool per thread)

**Threading model**:
- Build phase: always single-threaded on render thread (pass registration order must be deterministic)
- Execute phase: per-pass, fanned out to worker threads when `numJobs > 1`
- GPU submission: command lists batched with fence-based sync

**When needed**: When CPU command recording time shows up in profiling as a bottleneck — typically with many draw calls per pass (large scenes, shadow maps with many casters).

---

## Persistent Resource Tokens

**Problem**: Some resources survive across frames (TAA history, SSGI accumulators, reprojection buffers) but still need frame graph tracking for barrier insertion and lifetime management.

**What to build**:
- `frameGraph.allocPersistentToken(name, lifetimeFrames)` — creates a token tracking a named resource
- `builder.createPersistentTexture(token, desc)` — creates or retrieves the persistent resource
- Token monitors the descriptor. If it changes (resolution change, format change), auto-reallocates
- `frameGraph.freePersistentToken(token)` — explicit release

**Design notes**:
- Persistent resources are NOT pooled — they have identity across frames
- The token system detects descriptor mismatches and triggers reallocation transparently
- Lifetime in frames allows automatic cleanup of resources that are no longer referenced

**When needed**: When temporal effects are added (TAA, temporal reprojection, screen-space GI accumulation).

---

## Optimal Scheduling (Pass Reordering)

**Problem**: Even with priorities, the compiler may not find the best execution order for GPU utilization. Independent passes on different queues could overlap more aggressively.

**What to build**:
- Reorder independent passes to minimize barrier stalls
- Overlap compute passes with graphics passes on different queues
- Group passes sharing the same render targets to reduce state transitions
- Cost model: estimate pass execution time from historical GPU timings to inform reordering

**When needed**: When async compute is in place and profiling shows idle GPU time between passes that could be overlapped.

---

## Debug and Profiling Infrastructure

**Problem**: As the frame graph grows complex, developers need visibility into what it's doing — pass ordering, resource lifetimes, aliasing decisions, per-pass timing.

**What to build**:

### Event Hierarchy
- `ctx.pushEvent(name, color)` / `ctx.popEvent()` wrapping each pass
- Auto-emitted from pass registration name — visible in GPU profilers (RenderDoc, Nsight, PIX)
- RHI extension: `RhiCommandBuffer::beginDebugLabel` / `endDebugLabel`

### Resource Annotation
- `builder.setResourceDescription(handle, text)` — names resources in GPU captures
- Maps to `vkSetDebugUtilsObjectName` (Vulkan), `SetName` (D3D12)

### Pass Isolation Mode
- Config var `FrameGraphIsolatePasses` — inserts barriers between every pass
- Disables all overlap, making each pass independently timeable
- Development builds only

### Visual Inspection UI
- ImGui panel showing:
  - Pass schedule as a timeline (per-queue lanes)
  - Resource lifetimes as horizontal bars
  - Memory aliasing decisions (which resources share backing memory)
  - Per-pass GPU timing
- Read-only visualization of the compiled graph each frame

### Configuration Variables
| Variable | Purpose |
|----------|---------|
| `FrameGraphMultithreading` | 0 = off, 1 = on (job queue), 2 = on (inline) |
| `FrameGraphAliasTransientResources` | Enable/disable memory aliasing |
| `FrameGraphClearDiscardedResources` | Clear on discard (debug — catches use-after-free) |
| `FrameGraphEnableCulling` | Enable/disable pass culling |
| `FrameGraphEnableAsyncCompute` | Enable async compute queues |
| `FrameGraphEnableOptimalScheduling` | Enable advanced pass reordering |
| `FrameGraphIsolatePasses` | Barrier between every pass (debug) |
| `EnableAutomaticUAVBarriers` | Auto UAV sync between writing passes |

**When needed**: Incrementally, as the frame graph grows. Debug labels should come first (cheap, high value). Visual inspection UI when the pass count makes mental tracking impractical.

---

## Platform Extensions

**Problem**: Each GPU API has operations that can't be expressed in the common frame graph API. Extensions provide an escape hatch without polluting the shared interface.

**What to build**:
- Extension point in `FrameGraphContext` for platform-specific operations
- Accessed via `ctx.extension<VulkanExtension>()` or similar
- `#ifdef`-guarded per platform

**Per-platform examples**:

| Platform | Extension Examples |
|----------|-------------------|
| Vulkan | Pipeline barriers with explicit stage masks, dynamic rendering control, debug markers |
| D3D12 | Resource state transitions, RTV/DSV/UAV clearing, PIX markers, resource discard |
| Metal | Render/compute encoder management, resource usage declarations, GPU capture |

**Design notes**:
- Keep the common API covering 90%+ of pass needs
- A new backend only implements extensions for operations its API handles differently
- Stub/no-op extension is valid for platforms that don't need specialized behavior

**When needed**: When a second backend (D3D12, Metal) is added, or when Vulkan-specific features need to be exposed to passes without breaking the abstraction.

---

## Cross-Frame Synchronization

**Problem**: Some passes need to wait on work from a previous frame (temporal effects, GPU readback).

**What to build**:
- `builder.getFence()` — returns a fence handle from the current pass
- `builder.waitFence(fence, timeoutMs)` — a pass waits on a fence from a previous frame
- Backed by `VkFence` / `ID3D12Fence` through the RHI

**Use cases**:
- TAA reading previous frame's history buffer
- GPU readback (waiting for GPU-written data to reach CPU)
- Temporal reprojection
- CPU-GPU latency control (configurable frame-ahead distance)

**When needed**: When temporal effects or GPU readback are implemented.

---

## Early Graph Pattern

**Problem**: Some work must complete before the main frame graph begins building (environment reflections, acceleration structure builds, GPU-driven instance setup).

**What to build**:
- Support multiple `FrameGraph` instances per viewport
- Early graph runs and completes first; its outputs become external resources for the main graph
- Pattern: `earlyGraph.execute()` → `mainGraph.importTexture(earlyGraph.output)` → `mainGraph.execute()`

**When needed**: When the renderer has pre-frame compute work that the main pipeline depends on (environment probes, GPU culling, acceleration structure builds).
