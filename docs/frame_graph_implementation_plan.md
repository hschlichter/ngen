# Frame Graph Implementation Plan

## Context

The engine currently has a monolithic `Renderer::render()` that hardcodes a single forward pass with manual sync and no resource management. New passes will be added from now on, so getting the frame graph in place early means each new pass slots in cleanly instead of retrofitting later.

The frame graph sits in `src/renderer/` and consumes the existing RHI layer without replacing it.

---

## Phase 1: Dynamic Rendering + Frame Graph Core + Compilation

**Goal**: In one milestone, deliver a working frame graph that new passes can be added to. This combines dynamic rendering (RHI prerequisite), core types, and compilation — because a frame graph that can't compile is just a wrapper.

### Step 1a: Dynamic Rendering RHI Extension

Dynamic rendering (`VK_KHR_dynamic_rendering` / Vulkan 1.3) eliminates `VkRenderPass` and `VkFramebuffer` boilerplate. Without it, every new pass needs manual render pass + framebuffer setup — the exact problem the frame graph solves.

**RHI changes**:

| File | Change |
|------|--------|
| `src/rhi/rhitypes.h` | Add `RhiRenderingAttachmentInfo`, `RhiRenderingInfo`, `RhiBarrierDesc`, `RhiImageLayout` enum |
| `src/rhi/rhicommandbuffer.h` | Add `beginRendering(const RhiRenderingInfo&)`, `endRendering()`, `pipelineBarrier(std::span<const RhiBarrierDesc>)` |
| `src/rhi/vulkan/rhicommandbuffervulkan.h/.cpp` | Implement via `vkCmdBeginRendering`/`vkCmdEndRendering`, `vkCmdPipelineBarrier` |

**Pipeline creation change**: Current `RhiGraphicsPipelineDesc` takes `RhiRenderPass*`. With dynamic rendering, replace with format info (`VkPipelineRenderingCreateInfo`). This touches:
- `src/rhi/rhitypes.h` — update `RhiGraphicsPipelineDesc`
- `src/rhi/vulkan/rhidevicevulkan.cpp` — update pipeline creation

**Swapchain simplification**: `RhiSwapchain` no longer needs to own `VkRenderPass`/`VkFramebuffer`. Reduces to image management + acquire/present.

**Verify**: Existing forward pass renders using `beginRendering`/`endRendering` instead of `beginRenderPass`/`endRenderPass`.

### Step 1b: Frame Graph Core Types

**New files** (all `src/renderer/`):

| File | Contents |
|------|----------|
| `framegraph.h/.cpp` | `FrameGraph` — owns passes, resource registry, build/compile/execute lifecycle |
| `framegraphbuilder.h` | `FrameGraphBuilder` — setup-phase declaration API (no GPU work) |
| `framegraphcontext.h` | `FrameGraphContext` — execute-phase handle resolution, wraps `RhiDevice*` + `RhiCommandBuffer*` |
| `framegraphresource.h` | Typed handles, `FgAccessFlags` enum, `FgTextureDesc`, `FgBufferDesc`, `FgResource` registry entry |
| `passnode.h` | `PassNode` — name, read/write lists, side-effect flag, culled flag, execute function |

**Key design decisions**:
- Handles are struct wrappers around `uint32_t` indices (flat vector, O(1) lookup, type safety)
- Read-only vs writable handles are distinct types (`FgTextureHandle` vs `FgWritableTextureHandle`) — prevents write-after-read at compile time
- `addPass<DataT>(name, setup, exec)` returns `const DataT&` for pass chaining
- Data structs stored in a per-frame linear allocator (reset each frame, no per-pass heap alloc)
- Execute lambdas stored as `std::function<void(FrameGraphContext&)>`

### Step 1c: Compilation

Implement `FrameGraph::compile()`:

1. **Adjacency** — for each resource, record writer pass and reader passes. Reader depends on writer.
2. **Topological sort** — Kahn's algorithm → `passOrder` vector.
3. **Culling** — backward walk from `hasSideEffects` passes. Unreachable passes marked `culled`.
4. **Barrier insertion** — between consecutive passes, check resource access transitions, emit `RhiBarrierDesc`.

### Step 1d: Renderer Integration

**Modified files**:
- `src/renderer/renderer.h` — add `FrameGraph` member
- `src/renderer/renderer.cpp` — refactor `render()`: reset graph → import swapchain → addPass("ForwardPass", ...) → compile → execute

The existing forward pass becomes the first frame graph pass. Swapchain acquire/present and fence sync stay in `Renderer::render()` surrounding the frame graph calls.

**Verify**: Engine renders the same scene as before, now through the frame graph. Adding a dummy pass with no readers confirms culling works.

---

## Phase 2: Resource Management — Transient Pool + External Import

**Goal**: Frame graph creates and manages GPU resources. Add this when the first intermediate render target is needed (e.g., depth prepass writing a depth buffer that a lighting pass reads).

**New files**:

| File | Contents |
|------|----------|
| `src/renderer/resourcepool.h/.cpp` | `ResourcePool` — hash-keyed caches for textures and buffers. `acquireTexture(desc)`, `releaseTexture(desc, ptr)`, `resetFrame()`, `destroy()` |

**RHI extensions**:
- `src/rhi/rhitypes.h` — add `RhiTextureUsage` flags (ColorAttachment, DepthAttachment, Sampled, Storage, TransferSrc, TransferDst)
- Extend `RhiTextureDesc` with `RhiTextureUsage usage`
- `src/rhi/vulkan/rhidevicevulkan.cpp` — handle new usage flags in `createTexture` (map to `VkImageUsageFlags`)

**Resource lifecycle in execute**:
1. Pre-execute: acquire transient resources from pool → set `FgResource::physicalTexture/Buffer`
2. Run passes
3. Post-execute: release transients back to pool

**Pool lives in `Renderer`** (persists across frames). `FrameGraph` is reset each frame.

**Verify**: Create a transient texture in one pass, read it in another. Confirm pool reuse across frames (no allocation churn after first frame).

---

## Phase 3: Memory Optimization — Lifetime Tracking + Aliasing

**Goal**: Reduce VRAM by aliasing transient resources with non-overlapping lifetimes. Add this when enough transient resources exist that VRAM pressure matters — not needed early.

**No new files.** Extend `framegraph.cpp` compile step and `resourcepool.cpp`.

**Approach**:
1. For each transient resource, compute `[firstUsePass, lastUsePass]` intervals from sorted pass order
2. Interval-coloring: when a resource's last use completes, return its allocation to an intra-frame free list
3. When a new resource is needed, check the free list for a matching descriptor before pool acquire

This is pool-based reuse within a single frame. True `VkBindImageMemory` aliasing is a future optimization.

**Verify**: Create a pipeline where IntermediateA is used only in passes 1-2 and IntermediateC only in passes 3-4 with the same descriptor — confirm they alias to the same physical resource.

---

## Future Phases (not planned in detail)

- Multi-queue scheduling, async compute overlap
- Named priority classes, scheduling barriers
- Parallel command list recording (worker threads)
- Persistent resource tokens with auto-reallocation
- Debug/profiling — event hierarchy, pass visualization, isolation mode

---

## Critical Files

| File | Role |
|------|------|
| `src/renderer/renderer.h/.cpp` | Primary integration point — refactored to use frame graph |
| `src/rhi/rhitypes.h` | Extended with barriers, texture usage, rendering info |
| `src/rhi/rhicommandbuffer.h` | Extended with barrier + dynamic rendering commands |
| `src/rhi/vulkan/rhicommandbuffervulkan.h/.cpp` | Vulkan implementation of new commands |
| `src/rhi/vulkan/rhidevicevulkan.cpp` | Pipeline creation + texture creation updates |
| `src/rhi/rhiswapchain.h` | Simplified — drops VkRenderPass/VkFramebuffer ownership |

## Key Pitfalls to Watch

- **Pipeline creation**: Current `RhiGraphicsPipelineDesc` takes `RhiRenderPass*`. Dynamic rendering requires pipelines created with rendering format info instead — non-trivial RHI change that must happen in Phase 1.
- **Swapchain refactor**: Currently owns VkRenderPass + VkFramebuffer. Phase 1 makes these unnecessary. Needs careful refactoring to not break the existing render path during transition.
- **Descriptor sets**: Current pre-allocation per mesh x swapchain image is orthogonal to the frame graph. Leave it alone until a separate descriptor management refactor.
