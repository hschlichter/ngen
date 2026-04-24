# Plan: Incremental GPU Upload

## Context

`uploadRenderWorld` takes 1.2 seconds on the 5x5 city (~100 meshes) because it calls `waitIdle()`, destroys every GPU resource, and recreates everything from
scratch. This runs on the main thread and can't be backgrounded (Vulkan). The fix is to make it incremental — only create/destroy resources that actually
changed.

## Root Causes

1. **No resource caching** — every mesh instance gets its own vertex buffer, index buffer, and texture, even when multiple instances reference the same
   MeshHandle/MaterialHandle
2. **Full teardown on every upload** — all GPU resources destroyed before any are created
3. **Descriptor pool rebuilt from scratch** — pool + all sets reallocated every time
4. **Fallback texture recreated** — the checkerboard is regenerated every upload
5. **`waitIdle()`** — stalls the entire GPU pipeline

## Approach

### Phase 1: Resource Caching (biggest win)

Cache GPU resources by their library handle. If 50 building instances all reference the same `MeshHandle`, they share one vertex buffer and one index buffer.
Same for textures by `MaterialHandle`.

**New members in Renderer:**
```cpp
struct GpuMeshData {
    RhiBuffer* vertexBuffer;
    RhiBuffer* indexBuffer;
    uint32_t indexCount;
    uint32_t refCount = 0;
};

struct GpuTextureData {
    RhiTexture* texture;
    uint32_t refCount = 0;
};

std::unordered_map<uint32_t, GpuMeshData> meshCache;     // MeshHandle.index → GPU buffers
std::unordered_map<uint32_t, GpuTextureData> textureCache; // MaterialHandle.index → GPU texture
```

**GpuMesh becomes lightweight:**
```cpp
struct GpuMesh {
    MeshHandle mesh;
    MaterialHandle material;
    glm::mat4 transform;
};
```

Draw calls look up the cached buffers/textures by handle at render time instead of storing pointers per-instance.

For the 5x5 city: 100 building instances all reference 1 MeshHandle → 1 vertex buffer + 1 index buffer instead of 100. Upload drops from ~100 buffer creates to
~1.

### Phase 2: Diff-Based Upload

Compare the new `RenderWorld` against the current state instead of tearing down everything:

```
for each instance in new world:
    if mesh handle not in meshCache → create vertex/index buffers, add to cache
    if material handle not in textureCache → create texture, add to cache
    store {meshHandle, materialHandle, transform} in gpuMeshes[i]

for each entry in meshCache not referenced by any instance → destroy, remove
for each entry in textureCache not referenced by any instance → destroy, remove
```

Only `waitIdle()` if resources are actually being destroyed (to ensure the GPU isn't using them). If only transforms changed, no GPU stall needed — transforms
are push constants, updated per-draw.

### Phase 3: Descriptor Set Management

Current: `imgCount * meshCount` sets, all recreated every upload.

New: Descriptor sets bind UBO + texture. Since textures are now cached and shared, sets can be organized by `{frameIndex, materialHandle}` instead of
`{frameIndex, instanceIndex}`. This dramatically reduces the set count and eliminates per-upload reallocation.

Alternatively, keep per-instance sets but only recreate when the texture binding changes (rare — only on material reassignment or first upload).

### Phase 4: Persistent Fallback Texture

Move the checkerboard texture creation from `uploadRenderWorld` to `Renderer::init()`. Store it as a member. Reuse every frame.

## Implementation Order

1. **Move fallback texture to init** — trivial, immediate win
2. **Add mesh/texture caching** — biggest performance win, moderate complexity
3. **Diff-based upload with reference counting** — avoids destroying resources that are still used
4. **Skip `waitIdle()` when only transforms changed** — transforms are push constants, no GPU resource mutation needed
5. **Incremental descriptor sets** — only recreate sets whose texture binding changed

## Files Modified

| File | Changes |
|------|---------|
| `src/renderer/renderer.h` | Add `GpuMeshData`/`GpuTextureData` caches, simplify `GpuMesh`, add fallback texture member |
| `src/renderer/renderer.cpp` | Rewrite `uploadRenderWorld` to use caching + diffing, move fallback to `init()`, update `render()` to look up cached buffers, update `destroy()` to clean caches |

## Expected Impact

| Scene | Current | After Phase 1-2 | Reason |
|-------|---------|-----------------|--------|
| City 5x5 (100 buildings, 1 mesh asset) | 1200ms | ~20ms | 1 buffer create instead of 100, no texture recreation |
| City 20x20 (1500 buildings, 1 mesh asset) | ~18s est. | ~20ms | Same — caching makes it O(unique assets) not O(instances) |
| Kitchen_set (unique meshes) | ~500ms | ~100ms | Less caching benefit but skip unchanged resources |
| Transform-only change | 1200ms | ~0ms | No GPU resource mutation, just update push constant data |

## Verification

1. Load city_5x5, mute building.usda — should complete in <100ms total (background job + upload)
2. Load city_20x20 — should load in reasonable time
3. Change a transform — should be instant (no GPU stall)
4. Toggle visibility — should only destroy/create the affected instances
5. Verify no GPU validation errors
6. Verify no resource leaks (watch VRAM usage over repeated mute/unmute cycles)
