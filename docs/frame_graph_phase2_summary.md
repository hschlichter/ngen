# Frame Graph Phase 2 — Implementation Summary

## What Changed

Phase 2 added resource management: texture usage flags in the RHI, a pooled resource allocator, and transient texture support in the frame graph. The frame
graph can now create and manage GPU resources that live only for a single frame.

---

## RHI Extension: Texture Usage Flags

### New type (rhitypes.h)

`RhiTextureUsage` — bitmask enum:
- `Sampled` — shader read (SRV)
- `ColorAttachment` — render target
- `DepthAttachment` — depth-stencil target
- `Storage` — UAV / storage image
- `TransferSrc` / `TransferDst` — copy operations

### RhiTextureDesc update

Added `RhiTextureUsage usage` field. Defaults to `Sampled | TransferDst` for backward compatibility with existing texture creation code.

### Vulkan implementation

- `toVkImageUsage()` maps `RhiTextureUsage` flags to `VkImageUsageFlags`
- `createTexture()` uses `desc.usage` instead of hardcoded `TRANSFER_DST | SAMPLED`
- Image view aspect mask selects `DEPTH_BIT` when `DepthAttachment` usage is set

---

## Resource Pool

### New files

| File | Purpose |
|------|---------|
| `src/renderer/resourcepool.h` | `ResourcePool` class definition |
| `src/renderer/resourcepool.cpp` | Pool implementation |

### API

```cpp
pool.init(device);
auto* tex = pool.acquireTexture(desc);  // reuse or allocate
pool.releaseTexture(desc, tex);         // return to pool
pool.destroy();                         // free all GPU resources
```

### Design

- Keyed by `(width, height, format, usage)` — matching descriptors reuse the same GPU texture
- Linear scan of available pool (sufficient for expected transient count)
- Pool persists across frames in `Renderer`, frame graph is reset each frame
- On cache miss, allocates a new GPU texture via `RhiDevice::createTexture`

---

## Frame Graph: Transient Resources

### Builder API

```cpp
auto setup = [&](FrameGraphBuilder& builder, MyPassData& data) {
    auto tex = builder.createTexture({width, height, format, usage});
    data.output = builder.write(tex, FgAccessFlags::ColorAttachment);
};
```

`FrameGraphBuilder::createTexture(desc)` registers a non-external resource in the graph. The frame graph allocates it from the pool before execution and
releases it after.

### FgTextureDesc update

Added `RhiTextureUsage usage` field to match the RHI texture descriptor.

### Execution lifecycle

1. `compile()` — topological sort + pass culling (unchanged)
2. `execute()`:
   - Allocate transient resources from `ResourcePool`
   - Compute barriers (physical pointers now available)
   - Execute passes with automatic barrier insertion
   - Release transient resources back to pool

Barrier computation was moved from `compile()` to `execute()` because transient resources don't have physical GPU allocations until the pool assigns them.

### FrameGraph::setResourcePool

```cpp
frameGraph.setResourcePool(&resourcePool);
```

Called once during renderer init. The pool pointer persists across frame graph resets.

---

## Renderer Integration

- `ResourcePool resourcePool` member added to `Renderer`
- Initialized in `Renderer::init()`, destroyed in `Renderer::destroy()`
- Connected to frame graph via `frameGraph.setResourcePool(&resourcePool)`

---

## Files Modified

| File | Change |
|------|--------|
| `src/rhi/rhitypes.h` | Added `RhiTextureUsage` enum, updated `RhiTextureDesc` with `usage` field |
| `src/rhi/vulkan/rhidevicevulkan.h` | Declared `toVkImageUsage` |
| `src/rhi/vulkan/rhidevicevulkan.cpp` | Implemented `toVkImageUsage`, updated `createTexture` to use usage flags and correct aspect mask |
| `src/renderer/framegraphresource.h` | Added `RhiTextureUsage usage` to `FgTextureDesc` |
| `src/renderer/framegraphbuilder.h` | Added `createTexture(desc)` |
| `src/renderer/framegraph.h` | Added `setResourcePool()`, `ResourcePool*` member |
| `src/renderer/framegraph.cpp` | Added `createTexture` impl, moved barriers to `execute()`, added transient alloc/release |
| `src/renderer/renderer.h` | Added `ResourcePool` member |
| `src/renderer/renderer.cpp` | Init/destroy pool, connect to frame graph |

## New Files

| File | Purpose |
|------|---------|
| `src/renderer/resourcepool.h` | Hash-keyed texture pool |
| `src/renderer/resourcepool.cpp` | Pool implementation |
