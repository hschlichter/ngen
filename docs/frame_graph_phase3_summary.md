# Frame Graph Phase 3 — Implementation Summary

## What Changed

Phase 3 added transient resource lifetime tracking and intra-frame aliasing. Transient resources are now allocated at first use and released at last use, allowing resources with non-overlapping lifetimes to share the same GPU memory through pool reuse.

---

## Lifetime Tracking

### FgResource (framegraphresource.h)

Added two fields:

```cpp
uint32_t firstUseOrder = UINT32_MAX;
uint32_t lastUseOrder = 0;
```

These store the index into the sorted pass order where a transient resource is first and last accessed.

### compile() (framegraph.cpp)

After topological sort and pass culling, a new step scans all non-culled passes in execution order. For each transient (non-external) resource referenced by a pass's reads or writes, updates its `firstUseOrder` and `lastUseOrder`.

---

## Interleaved Allocation and Release

### Previous behavior

All transients were allocated before any pass ran and released after all passes completed. Two transients with the same descriptor always consumed separate GPU memory, even if their lifetimes didn't overlap.

### New behavior

The `execute()` loop now interleaves allocation and release with pass execution:

1. **Before each pass**: allocate transients whose `firstUseOrder` matches the current pass
2. **Compute barriers**: layout transitions for this pass (computed inline, now that physical pointers are set)
3. **Execute pass**
4. **After each pass**: release transients whose `lastUseOrder` matches the current pass

Released resources return to the `ResourcePool` immediately, making them available for later transients with matching descriptors within the same frame.

### Example

```
Pass 0: writes IntermediateA (1024x1024 RGBA8)
Pass 1: reads IntermediateA
Pass 2: writes IntermediateB (1024x1024 RGBA8)  <- reuses A's allocation
Pass 3: reads IntermediateB
```

IntermediateA is released after pass 1. When IntermediateB is allocated before pass 2, the pool returns the same GPU texture.

---

## Cleanup

- Removed `BarrierBatch` struct and `passBarriers` vector from `FrameGraph` — barriers are now computed inline during the execution loop
- Simplified `reset()` accordingly

---

## Files Modified

| File | Change |
|------|--------|
| `src/renderer/framegraphresource.h` | Added `firstUseOrder`, `lastUseOrder` to `FgResource` |
| `src/renderer/framegraph.h` | Removed `BarrierBatch` and `passBarriers` |
| `src/renderer/framegraph.cpp` | Lifetime computation in `compile()`, interleaved alloc/release in `execute()` |
