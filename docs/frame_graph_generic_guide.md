# Frame Graph: Implementation Guide

A reference for implementing a frame graph rendering system in any engine. Covers the data model, APIs, scheduling, resource management, and threading — all
engine-agnostic.

---

## 1. What a Frame Graph Is

A frame graph replaces a traditional fixed-order render loop with a declarative model. Instead of passes calling the GPU device directly in a hardcoded
sequence, each pass declares what resources it reads and writes. The frame graph then:

1. Compiles an optimal execution schedule from those declarations
2. Allocates GPU memory and determines aliasing opportunities
3. Inserts resource barriers automatically
4. Manages resource lifetimes
5. Culls passes whose outputs are unused

The key insight is **separating declaration from execution**. Setup lambdas are pure descriptions of intent — no GPU work happens until the entire frame's
dependency graph is known. This gives the system full global visibility before committing to any execution order.

### Multiple Graphs per Viewport

A common pattern is to run two frame graphs per viewport:

- **Early graph** — pre-frame work that the main pipeline depends on (environment reflections, acceleration structure builds, GPU-driven instance setup)
- **Main graph** — the full rendering pipeline (geometry through post-processing)

The early graph completes before the main graph begins building, ensuring its outputs are available as external resources.

---

## 2. Core Classes

A minimal frame graph implementation requires three core types:

| Class | Role |
|-------|------|
| `FrameGraph` | Owns the pass list, resource caches, and the build/compile/execute lifecycle. One instance per viewport per graph type. |
| `FrameGraphBuilder` | Passed to each pass's setup lambda. Provides the API for declaring resources and access patterns. Must not perform any GPU work. |
| `FrameGraphContext<T>` | Passed to each pass's execute lambda. Resolves frame graph handles to real GPU objects and provides draw/dispatch/copy commands. `T` is the device context type (graphics or compute). |

Internal bookkeeping uses a fourth type:

| Class | Role |
|-------|------|
| `PassNode` | Per-pass metadata: dependency lists (parents, children, parents after transitive reduction), queue assignment, priority, per-resource dependency records. |

---

## 3. Lifecycle: Build, Compile, Execute

Every frame follows a strict three-phase sequence:

```
BeginBuild()
  |-- Setup lambdas run (single-threaded)
  |     Each pass declares resources via FrameGraphBuilder
  |     Pass data structs populated with typed handles
  +-- EndBuild()
        |-- Compile:  build resource/pass dependency graph
        |-- Schedule: topological sort respecting priorities and barriers
        |-- Cull:     remove passes whose outputs are unused
        +-- Allocate: assign transient GPU memory, determine aliases

Execute()
  +-- Execute lambdas run per-pass (potentially multithreaded)
        Barriers inserted automatically between passes
        Platform-specific state transitions handled by extensions
```

### 3.1 Build Phase

`BeginBuild()` resets the graph state for a new frame. Between `BeginBuild()` and `EndBuild()`, passes are registered. Each pass's setup lambda runs immediately
and populates a data struct with typed resource handles.

Setup lambdas must be single-threaded and must not issue GPU work. This constraint is non-negotiable — it is what enables the frame graph to reason globally
before committing to an execution plan.

`EndBuild()` triggers compilation in four steps:

1. **Dependency graph construction** — For each resource, record which passes write it and which passes read it. This produces a directed acyclic graph of pass
   dependencies.
2. **Topological sort** — Order passes respecting data dependencies, queue assignments, and priority classes.
3. **Pass culling** — Remove passes whose outputs are never read by any subsequent pass (unless marked as having side effects).
4. **Memory allocation** — Assign GPU memory to transient resources. Identify non-overlapping lifetimes and alias them to the same memory.

### 3.2 Execute Phase

Walk the sorted pass list and invoke each pass's execute lambda. The context object resolves frame graph handles into real GPU objects and wraps the platform
device context.

Between passes, automatically insert resource barriers based on declared access flags. Platform-specific extensions handle native state transitions.

If multithreading is enabled and a pass has declared parallel jobs, split the execute lambda across worker threads for parallel command list recording.

---

## 4. Defining Passes

A pass consists of three parts: a **data struct**, a **setup lambda**, and an **execute lambda**.

```cpp
struct MyPassData {
    FGRenderTargetView   ColorRTV;
    FGShaderResourceView DepthSRV;
};

auto setup = [&](FrameGraphBuilder& builder, MyPassData& data) {
    data.ColorRTV = builder.RenderTarget(0, colorTex);  // write to RT slot 0
    data.DepthSRV = builder.ShaderResource(depthTex);    // read depth
};

auto execute = [](FrameGraphContext& ctx, const MyPassData& data) {
    ctx.BindSRV(0, data.DepthSRV);
    ctx.DrawIndexed(startIdx, numIdx, baseVtx);
};

const auto& passData = frameGraph.AddPass("MyPass", MyPassData{}, setup, execute);
```

### Design Rationale

- The **data struct** bridges phases: setup populates it with opaque handles, execute reads those handles and resolves them to real GPU objects via the context.
- Returning a `const&` to the data struct allows subsequent passes to reference this pass's outputs (e.g., passing the underlying texture of `passData.ColorRTV`
  to a later pass's shader resource input).
- For compute-only passes, the context type parameter should be the compute context rather than the graphics context.

---

## 5. Resource Model

The frame graph manages three categories of resources.

### 5.1 External Resources

Owned outside the frame graph (swap chain back buffer, persistent shadow atlas, etc.). Imported with an initial state indicating whether to preserve existing
content:

```cpp
auto backBuffer = builder.ImportTexture2D(swapChainTexture, InitialState::Preserve);
```

The frame graph does not allocate or free external resources — it only tracks their state transitions within the current frame.

### 5.2 Transient Resources

Single-frame lifetime. Created from a pool, returned after the frame completes. This is the workhorse for all intermediate buffers — GBuffer surfaces, blur
temporaries, lighting accumulation targets.

```cpp
auto gbuffer0 = builder.CreateTransientTexture2D("GBuffer0", Format::RGBA8_UNORM, w, h);
```

Transient resources with non-overlapping lifetimes should be aliased to the same GPU memory. The allocator tracks per-pass allocation/deallocation windows and
finds compatible overlaps, significantly reducing VRAM usage.

### 5.3 Persistent Resources

Survive across frames (temporal AA history, screen-space GI accumulators, reprojection buffers). Managed through tokens that track a lifetime in frames and
auto-reallocate if the descriptor changes (e.g., after a resolution change):

```cpp
auto token = frameGraph.AllocPersistentToken("TAA_History", lifetimeFrames);
auto history = builder.CreatePersistentTexture2D(token, "TAA_History", format, w, h);
```

If the descriptor changes (resolution, format), the token system detects the mismatch and reallocates automatically.

### 5.4 Initial State Controls

Each resource import/creation should specify an initial state:

| State | Behavior |
|-------|----------|
| `Discard` | Don't care about previous content — enables fast clears on some hardware |
| `Preserve` | Keep existing content |
| `ClearColor(r,g,b,a)` | Clear to a specific color |
| `ClearDepth(d)` | Clear depth buffer |
| `ClearStencil(s)` | Clear stencil buffer |
| `ClearDepthStencil(d,s)` | Clear both |

### 5.5 Buffer Creation Helpers

Provide specialized helpers matching common GPU usage patterns:

| Helper | Pattern |
|--------|---------|
| `CreateTransientTypedBuffer` | Fixed element format (e.g., R32_UINT) |
| `CreateTransientStructuredBuffer` | User-defined stride |
| `CreateTransientByteAddressBuffer` | Raw byte access |
| `CreateTransientIndirectDrawArgs` | Indirect draw argument buffer |
| `CreateTransientIndirectDispatchArgs` | Indirect dispatch argument buffer |

### 5.6 Buffer Usage Modes

| Mode | Access Pattern |
|------|---------------|
| `Default` | GPU read/write only |
| `Dynamic` | GPU read, CPU write (double-buffered) |
| `Volatile` | Single-frame lifetime (CPU write, GPU read) |
| `Readback` | GPU write, CPU read |

---

## 6. Typed Resource Handles

All resource references must be strongly typed at compile time. A read-only texture handle must not be assignable where a writable one is expected. This
prevents write-after-read hazards at the API level rather than relying on runtime checks.

### 6.1 Recommended Handle Types

| Handle | Access |
|--------|--------|
| `FGTexture2D` / `FGWritableTexture2D` | 2D texture (read / read-write) |
| `FGTexture3D` / `FGWritableTexture3D` | 3D texture |
| `FGBuffer` / `FGWritableBuffer` | GPU buffer |
| `FGSRV` | Shader resource view (read) |
| `FGUAV` | Unordered access view (read-write) |
| `FGRTV` | Render target view (write) |
| `FGReadOnlyDSV` / `FGReadWriteDSV` | Depth-stencil view (read-only / read-write) |
| `FGIBV` / `FGVBV` / `FGCBV` | Index / vertex / constant buffer views |
| `FGIndirectArgs` | Indirect draw/dispatch arguments |
| `FGCopySrc` / `FGCopyDst` | Copy source / destination |

### 6.2 Access Flags

The underlying access flags map directly to GPU resource states and drive barrier insertion:

| Flag | Meaning |
|------|---------|
| `ShaderResource` | Read in a shader (SRV) |
| `UnorderedAccess` | Read/write in a shader (UAV) |
| `RenderTarget` | Write as a render target (RTV) |
| `DepthStencilWrite` | Read/write depth-stencil |
| `DepthStencilRead` | Read-only depth-stencil |
| `CopySrc` / `CopyDst` | Copy operations |
| `IndirectArgument` | Indirect draw/dispatch |
| `VertexBuffer` / `IndexBuffer` / `ConstantBuffer` | Fixed-function bindings |

When a resource transitions between access modes across passes (e.g., `RenderTarget` in pass A to `ShaderResource` in pass B), the frame graph emits the
appropriate GPU barrier.

---

## 7. Builder API (Setup Phase)

The builder provides methods to declare every type of resource access. All methods are pure declarations — no GPU work.

### 7.1 Texture Views

All should support mip/array slice selection with sensible defaults (all mips, all array slices):

```cpp
builder.ShaderResource(texture2D, mostDetailedMip, mipLevels, firstSlice, arraySize);
builder.UnorderedAccess(writableTexture2D, mipSlice, firstSlice, arraySize);
builder.RenderTarget(rtIndex, writableTexture2D, mipSlice, firstSlice, arraySize);
builder.DepthStencilReadOnly(texture2D, mipSlice, firstSlice, arraySize);
builder.DepthStencilReadWrite(writableTexture2D, mipSlice, firstSlice, arraySize);
```

### 7.2 Buffer Views

```cpp
builder.ShaderResource(buffer);
builder.UnorderedAccess(writableBuffer);
builder.IndexBuffer(buffer);
builder.IndirectArgs(buffer);
```

### 7.3 Copy Operations

```cpp
builder.CopySource(resource, subresource);
builder.CopyDest(writableResource, subresource);
```

### 7.4 Bindless Resources

```cpp
builder.RegisterBindlessSRV(...);
builder.RegisterBindlessUAV(...);
builder.BindlessSRV(...);
builder.BindlessUAV(...);
```

Support static bindless slots for long-lived resources (noise textures, LUTs, etc.).

### 7.5 Custom Descriptors

All view methods should have overloads accepting full descriptor structs for manual control over format reinterpretation, typed/structured buffer views, etc.

### 7.6 Pass Hints

| Hint | Purpose |
|------|---------|
| `SetWriteOnlyUAV()` | UAV is write-only — skip UAV barrier if next pass also writes |
| `SetHasSideEffects()` | Prevent culling even if no other pass reads outputs (present, screenshots, debug rendering) |
| `SetNumJobs(n)` | Split execute lambda across `n` worker threads for parallel command list recording |
| `GetFence()` | Return a fence handle for explicit cross-frame synchronization |
| `SetQueue(queue)` | Assign to a specific GPU queue (graphics, compute, etc.) |
| `SetSchedulingPriority(prio)` | Control ordering within a queue via named priority classes |
| `SchedulingBarrier()` | Force all prior passes to complete before subsequent ones |

---

## 8. Context API (Execute Phase)

The context resolves frame graph handles into real GPU objects and wraps the platform device context.

### 8.1 Handle Resolution

```cpp
ctx.GetTexture2D(handle)  -> NativeTexture2D*
ctx.GetBuffer(handle)     -> NativeBuffer*
ctx.GetSRV(handle)        -> NativeShaderResourceView*
ctx.GetUAV(handle)        -> NativeUnorderedAccessView*
ctx.GetRTV(handle)        -> NativeRenderTargetView*
ctx.GetDSV(handle)        -> NativeDepthStencilView*
```

### 8.2 Resource Binding

```cpp
ctx.BindSRV(slot, srvHandle);
ctx.BindUAV(slot, uavHandle);
ctx.SetVertexBuffer(buffer, stride);
ctx.SetIndexBuffer(ibvHandle, byteOffset, size);
```

### 8.3 Draw and Dispatch

```cpp
ctx.DrawIndexed(startIdx, numIdx, baseVertex);
ctx.DrawIndexedInstanced(...);
ctx.DrawIndexedInstancedIndirect(indirectArgs, offset);
ctx.Dispatch(groupsX, groupsY, groupsZ);
ctx.DispatchIndirect(indirectArgs, offset);
```

### 8.4 Transfer Operations

```cpp
ctx.CopyResource(dst, src);
ctx.CopySubresource(dst, dstMip, dstSlice, src, srcMip, srcSlice);
```

### 8.5 Access Boundary

The context must not expose the raw device context through the frame graph handle API — all resource accesses should go through the typed handle system so
barriers remain correct. However, the underlying device context should be accessible for draw loop functions that need direct command recording (e.g., iterating
a sorted primitive list and recording individual draw calls). This bridges the declarative frame graph with imperative draw call recording.

---

## 9. Scheduling

### 9.1 GPU Queues

Support multiple GPU queues to enable async compute overlap:

```cpp
enum class FrameGraphQueue {
    Graphics,
    Compute0,
    Compute1,
    // ... additional queues as hardware supports
};
```

`builder.SetQueue(queue)` assigns a pass to a specific queue. Example: a screen-space GI pass runs on `Compute0` while shadow geometry renders on `Graphics`.

### 9.2 Priority Classes

Define named priority classes that give the scheduler enough information to overlap independent work while respecting data dependencies:

```
DepthPrepass, GBufferFill, ShadowGeometry, DeferredShading,
ScreenSpaceGI, ScreenSpaceReflections, VolumetricLighting,
Composition, Upscaling, PostProcessing, ...
```

`builder.SetSchedulingPriority(prio)` assigns a pass to a class. The scheduler uses priorities to order passes within a queue and to determine which independent
passes can overlap.

Define enough classes to express the engine's pass ordering requirements — 30-50+ is typical for a production renderer.

### 9.3 Scheduling Barriers

`builder.SchedulingBarrier()` forces all prior passes to complete before subsequent ones — a hard sync point. Use sparingly: only when a full pipeline flush is
required (e.g., before CPU readback of GPU results).

### 9.4 Optimal Scheduling

When enabled, the scheduler performs advanced pass reordering:

- Overlap compute passes with graphics passes on different queues
- Reorder independent passes to minimize barrier stalls
- Group passes sharing the same render targets to reduce state transitions

---

## 10. Pass Culling

Passes whose outputs are never read by any subsequent pass are automatically removed. This is a key design feature:

- Features can be conditionally registered during build without `if/else` guards
- If nothing downstream needs the output, the pass simply doesn't execute
- No runtime cost for disabled features

`SetHasSideEffects()` overrides culling for passes with externally-visible results:
- Present to swap chain
- Screenshot capture
- Debug visualization

### Implementation

Each pass node maintains:
- **Parents** — passes this pass depends on (reads their outputs)
- **Children** — passes that depend on this pass (read its outputs)
- **ParentsReduced** — parents after transitive reduction (minimizes unnecessary barrier edges)
- Per-resource dependency records

The culling algorithm walks the graph backward from side-effect passes, keeping only reachable nodes. Everything else is removed from the execution schedule.

---

## 11. Resource Pooling and Memory Aliasing

### 11.1 Resource Pools

Maintain hash-keyed caches for both resources and views:

| Cache | Contents |
|-------|----------|
| Texture2D pool | Transient 2D textures — returned after each frame, reused when a matching descriptor is requested |
| Texture3D pool | Transient 3D textures |
| Buffer pool | Transient GPU buffers |
| SRV pool | Shader resource view descriptors — avoid per-frame allocation |
| UAV pool | Unordered access view descriptors |
| RTV pool | Render target view descriptors |
| DSV pool | Depth-stencil view descriptors |

When a pass requests a transient resource, check the pool first for a matching descriptor (format, dimensions, flags). Only allocate a new GPU resource on cache
miss.

### 11.2 Memory Aliasing

Track the lifetime of each transient resource (which passes allocate and deallocate it) and identify non-overlapping lifetimes. Resources that never coexist
alias to the same GPU memory region.

Example:

```
Pass A writes IntermediateA
Pass B reads IntermediateA, writes IntermediateB
Pass C reads IntermediateB, writes IntermediateC
-> IntermediateA and IntermediateC can alias (non-overlapping lifetimes)
```

This is particularly effective for intermediate buffers in the pipeline — GBuffer surfaces used only during the lighting pass, blur temporaries used only during
post-processing, etc.

### 11.3 Persistent Resource Tokens

Persistent resources use a separate token system:

- `AllocPersistentToken(name, lifetimeFrames)` — creates a token tracking a named persistent resource
- `FreePersistentToken(token)` — releases the token and its backing resource

Tokens monitor the resource descriptor. If it changes (e.g., due to a resolution change), the token automatically reallocates its backing GPU resource with the
new parameters.

---

## 12. Synchronization

### 12.1 Intra-Frame Barriers

The frame graph inserts resource barriers automatically based on declared access flags. When a resource transitions between access modes across passes (e.g.,
`RenderTarget` to `ShaderResource`), emit the appropriate GPU barrier.

Optionally support automatic UAV barriers between passes that both write to the same resource. Provide a toggle to disable this when passes are known to write
non-overlapping regions.

### 12.2 Cross-Queue Synchronization

Handle this through platform-specific extensions:
- **D3D12**: fence-based synchronization between queues
- **Vulkan**: semaphores, timeline semaphores, or events
- **Metal**: MTLEvent or MTLSharedEvent
- **Console APIs**: platform-specific labels or synchronization primitives

### 12.3 Cross-Frame Synchronization

Provide a fence mechanism for passes that need to wait on work from a previous frame:

```cpp
// Frame N setup:
auto fence = builder.GetFence();

// Frame N+1 setup:
builder.WaitFence(fence, timeoutMs);
```

Use cases: temporal AA (reading previous frame's history), GPU readback (waiting for GPU-written data to reach CPU), temporal reprojection.

---

## 13. Platform Extensions

Each backend should be able to extend the frame graph with platform-specific operations that cannot be expressed in the common API. These extensions are
accessed through the context during execute and should be conditionally compiled per platform.

### Recommended Extension Points

| Platform | Extension Examples |
|----------|-------------------|
| D3D12 | Explicit state transition barriers, RTV/DSV/UAV clearing (float and integer variants), resource discard, PIX markers |
| Vulkan | Pipeline barriers with stage masks, render pass begin/end, dynamic rendering, debug markers |
| Metal | Render/compute encoder management, resource usage declarations, GPU capture markers |
| Console | Platform-specific GPU synchronization, cache management, multiple compute queues, specialized memory operations |

### Design Guidance

- Keep the common API minimal — it should cover 90%+ of pass needs
- Extensions should be `#ifdef`-guarded per platform and accessed through the context
- A new backend only needs to implement extensions for operations its API handles differently — a stub/no-op extension is valid for platforms that don't need
  specialized behavior

---

## 14. Integration with Draw Loops

Draw loops bridge the frame graph's declarative pass model with actual draw call recording. A pass's execute lambda typically delegates to a draw loop function:

```cpp
DrawLoop(drawContext, subContextIndex, deviceContext);
DrawLoopShadow(drawContext, subContextIndex, deviceContext);
DrawLoopWithTessellation(drawContext, subContextIndex, deviceContext);
DispatchLoop(dispatchContext, subContextIndex, deviceContext);
```

### Draw Context

The draw context carries everything needed to record draw calls for a pass:

| Field | Purpose |
|-------|---------|
| Sorted primitive list | From the gather/visibility phase |
| Camera matrices | World-to-view, view-to-projection (current + previous frame) |
| Viewport | Dimensions, eye point |
| Render targets | Array of render target views + depth-stencil view |
| Render layer | Current layer being drawn |
| Draw loop state | Blend, rasterizer, depth-stencil, VRS state |
| Per-view constant buffer | Shared GPU constants |
| Flags | Material overrides, mirror rendering, cloth, etc. |
| Threading info | Device context count, job entry count |

### Draw Loop State

State management within the loop:

| State | Source |
|-------|--------|
| Blend | Per-material or global override |
| Rasterizer | Per-material (backface culling) or global |
| Depth-stencil | Global with stencil ref override |
| VRS hints | Transparency, emissive |
| Shader overrides | Pixel shader override for specialized modes |

### Pipeline State Lookup

The draw loop selects pipeline state objects (PSOs) based on each primitive's material. Use a compact, hashable pipeline state descriptor encoding all relevant
fields (shader IDs, input layout, blend/rasterizer/depth-stencil state, topology, render target format). This enables O(1) cache lookups.

Provide async PSO compilation for cache misses to avoid stalls on first use.

---

## 15. Example Rendering Pipeline

The frame graph hosts the complete rendering pipeline as a series of passes. A typical deferred renderer organizes passes as follows:

### Main Graph Pass Sequence

```
1. Geometry
   |-- Depth Prepass
   |-- GBuffer Fill (N channels: normals, albedo, specular, roughness, velocity, etc.)
   |-- Terrain
   +-- Decals

2. Lighting
   |-- Clustered/Tiled Light Classification
   |-- Deferred Shading
   +-- Shadow Map Updates (cascaded, tiled atlas)

3. Global Illumination
   |-- Screen-Space GI / Reflections
   |-- GI Probes (irradiance, radiance)
   +-- Environment Reflections (from early graph)

4. Atmospheric Effects
   +-- Volumetric Lighting (froxel grid scattering)

5. Composition
   +-- Combine lighting + emissive + decals + transparency resolve

6. Upscaling (optional)
   +-- Temporal upscaling (with reactive mask + motion vectors)

7. Post-Processing
   |-- Temporal Anti-Aliasing
   |-- Motion Blur
   |-- Bloom / Glare
   |-- Tone Mapping
   |-- Depth of Field
   |-- Color Grading
   +-- Sharpening
```

### Early Graph

Runs before the main graph:
- Environment reflection cubemap rendering
- GPU instance acceleration structure builds
- Pre-frame compute work the main pipeline depends on

### Render Layer Concept

Organize geometry into named categories (layers) that map to frame graph passes. Each layer implies a specific rendering technique and pipeline configuration:

| Layer | Purpose |
|-------|---------|
| DepthPrepass | Depth-only rendering for early-Z |
| Shadows | Shadow map geometry |
| GBuffer | Deferred GBuffer fill |
| Emissive | Self-lit geometry |
| Decals | Projected decals |
| Particles | Particle rendering |
| Terrain | Terrain-specific rendering |
| Water | Water surface rendering |
| Transparency | Order-independent transparency |
| Voxelization | Voxelize geometry for GI |

Material classification queries (e.g., `IsDecal()`, `IsTransparent()`) determine which layer a primitive belongs to during the gather phase.

---

## 16. Configuration Variables

Expose these as runtime-toggleable settings for development and profiling:

| Variable | Purpose |
|----------|---------|
| `FrameGraphMultithreading` | 0 = off, 1 = on (with job queue), 2 = on (inline threading) |
| `FrameGraphAliasTransientResources` | Enable/disable memory aliasing for transients |
| `FrameGraphClearDiscardedResources` | Clear resources on discard (debug — catches use-after-free visually) |
| `FrameGraphEnableCulling` | Enable/disable automatic pass culling |
| `FrameGraphEnableAsyncCompute` | Enable async compute on separate queues |
| `FrameGraphEnableOptimalScheduling` | Enable advanced pass reordering |
| `FrameGraphEnableSchedulingPriorities` | Use named priority classes for ordering |
| `FrameGraphEnableSchedulingBarriers` | Enable explicit scheduling barriers |
| `FrameGraphIsolatePasses` | Insert barriers between all passes (debug — disables overlap, makes each pass independently timeable) |
| `EnableAutomaticUAVBarriers` | Automatic UAV synchronization between writing passes |

---

## 17. Debug and Profiling

### 17.1 Event Hierarchy

```cpp
ctx.PushEvent(name, color, sourceLocation);
ctx.PopEvent();
```

Each pass should automatically emit events using its registration name and color. Events produce a hierarchical timeline visible in GPU profilers (PIX,
RenderDoc, Nsight, platform tools).

### 17.2 Priority Visualization

```cpp
ctx.PushPriority(priorityClass);
ctx.PopPriority();
```

Visualize which priority class each pass belongs to — useful for verifying that the scheduler is overlapping work as expected.

### 17.3 Development Build Features

In non-shipping builds, support:
- **Resource annotation**: `SetResourceDescription(handle, text)` for naming resources in GPU captures
- **Event annotation**: `SetEventDescription(text)` for adding detail to profiler events
- **Visual inspection**: real-time UI (e.g., ImGui) showing the pass schedule, resource lifetimes, memory aliasing decisions, and per-pass timing
- **Pass isolation mode**: `FrameGraphIsolatePasses` inserts barriers between every pass, making each independently timeable at the cost of disabling all
  overlap

---

## 18. Threading Model

The frame graph operates within a multi-domain threading model:

```
+-------------+     Scene Command Ring   +--------------+
| Main Thread  | ----------------------> | Render Thread |
|              |  (scene change batches) |              |
| - Simulation |                         | - Consume ring|
| - Scene push |                         | - Build FG    |
| - Frame sync |                         | - Execute FG  |
+-------------+                          +------+-------+
                                                | Parallel jobs
                                                v
                                         +--------------+
                                         |Worker Threads |
                                         |              |
                                         | - Parallel    |
                                         |   command list|
                                         |   recording   |
                                         +------+-------+
                                                | Submit
                                                v
                                         +--------------+
                                         |     GPU      |
                                         |              |
                                         | - Async exec  |
                                         | - Fence sync  |
                                         +--------------+
```

### Domain Responsibilities

| Domain | Role |
|--------|------|
| Main thread | Game simulation, push scene changes into a command ring, frame synchronization |
| Render thread | Consume command ring, update render graph nodes, build and execute the frame graph. The build phase is always single-threaded. |
| Worker threads | When enabled and a pass declares parallel jobs, execute lambdas are split across workers for parallel command list recording. |
| GPU | Executes submitted command lists asynchronously. Fence-based synchronization controls CPU-ahead distance (typically 1 frame). |

### Key Design Points

- **Build phase**: always single-threaded on the render thread — pass registration order matters for deterministic scheduling
- **Execute phase**: per-pass, optionally fanned out to worker threads
- **GPU submission**: command lists batched into a ring buffer (typed: direct/compute/copy) with fence-based synchronization
- **CPU-GPU latency**: configurable (default 1 frame ahead) — governs how far the CPU can advance before waiting for the GPU

---

## 19. Implementation Checklist

Summary of components to implement, ordered by dependency:

### Phase 1: Core

- [ ] `FrameGraph` class with build/compile/execute lifecycle
- [ ] `FrameGraphBuilder` with resource declaration API
- [ ] `FrameGraphContext<T>` with handle resolution and GPU command wrappers
- [ ] `PassNode` with dependency tracking (parents, children)
- [ ] Typed resource handles (read-only vs. writable)
- [ ] Access flag enum mapping to GPU resource states
- [ ] Pass registration macro/function (data struct + setup + execute)

### Phase 2: Compilation

- [ ] Dependency graph construction from resource declarations
- [ ] Topological sort
- [ ] Pass culling (backward walk from side-effect passes)
- [ ] Automatic barrier insertion based on access flags

### Phase 3: Resource Management

- [ ] External resource import
- [ ] Transient resource creation and pool return
- [ ] Resource pool with hash-keyed descriptor matching
- [ ] View descriptor caching (SRV, UAV, RTV, DSV pools)
- [ ] Persistent resource token system with auto-reallocation

### Phase 4: Memory Optimization

- [ ] Transient resource lifetime tracking (per-pass allocation/deallocation windows)
- [ ] Memory aliasing for non-overlapping transients
- [ ] Initial state controls (discard, preserve, clear variants)

### Phase 5: Scheduling

- [ ] GPU queue assignment (graphics, compute, copy)
- [ ] Named priority classes
- [ ] Scheduling barriers (hard sync points)
- [ ] Optimal scheduling (pass reordering for overlap)

### Phase 6: Threading

- [ ] Per-pass multithreaded execute (parallel command list recording)
- [ ] Cross-frame fences for temporal effects

### Phase 7: Platform Extensions

- [ ] Extension point in the context for platform-specific operations
- [ ] Per-platform barrier/clear/sync implementations
- [ ] Conditional compilation guards

### Phase 8: Debug

- [ ] Event hierarchy (push/pop with name, color)
- [ ] Resource and event annotation
- [ ] Pass isolation mode
- [ ] Visual inspection UI (pass schedule, resource lifetimes, aliasing)
- [ ] Configuration variables (runtime toggles for all major features)

---

## 20. Complete Pass Example

End-to-end example showing a deferred lighting pass:

```cpp
struct DeferredLightingData {
    // Inputs (read-only)
    FGSRV   GBuffer0SRV;
    FGSRV   GBuffer1SRV;
    FGSRV   GBuffer2SRV;
    FGSRV   DepthSRV;

    // Output (write)
    FGRTV   LightAccumRTV;
    FGUAV   LightAccumUAV;
};

// Setup lambda — pure declaration, no GPU work
auto setup = [&](FrameGraphBuilder& builder, DeferredLightingData& data) {
    // Read GBuffer channels as shader resources
    data.GBuffer0SRV = builder.ShaderResource(gbufferPass.GBuffer0Tex);
    data.GBuffer1SRV = builder.ShaderResource(gbufferPass.GBuffer1Tex);
    data.GBuffer2SRV = builder.ShaderResource(gbufferPass.GBuffer2Tex);
    data.DepthSRV    = builder.ShaderResource(depthPrepass.DepthTex);

    // Create transient output
    auto lightAccumTex = builder.CreateTransientTexture2D(
        "LightAccum", Format::R11G11B10_FLOAT, w, h);
    data.LightAccumRTV = builder.RenderTarget(0, lightAccumTex);
    data.LightAccumUAV = builder.UnorderedAccess(lightAccumTex);

    // Schedule on graphics queue with deferred lighting priority
    builder.SetQueue(FrameGraphQueue::Graphics);
    builder.SetSchedulingPriority(PassPriority::DeferredShading);
};

// Execute lambda — actual GPU work
auto execute = [](FrameGraphContext& ctx, const DeferredLightingData& data) {
    ctx.BindSRV(0, data.GBuffer0SRV);
    ctx.BindSRV(1, data.GBuffer1SRV);
    ctx.BindSRV(2, data.GBuffer2SRV);
    ctx.BindSRV(3, data.DepthSRV);
    // Full-screen compute dispatch for deferred shading
    ctx.Dispatch(groupsX, groupsY, 1);
};

const auto& lightData = frameGraph.AddPass(
    "DeferredLighting", DeferredLightingData{}, setup, execute);

// Subsequent passes reference lightData outputs:
// e.g., compositionPass reads lightData.LightAccumRTV's underlying texture
```

This demonstrates:
- Reading outputs from previous passes via their data struct references
- Creating transient resources scoped to this frame
- Queue and priority assignment for scheduling
- The data struct bridging setup and execute phases
- Output forwarding to subsequent passes via the `const&` return
