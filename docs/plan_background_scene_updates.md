# Plan: Background Threaded Scene Updates

## Context

When muting/unmuting layers, changing transforms, or toggling visibility, the engine stalls visibly because all work (USD recomposition, prim cache rebuild,
mesh extraction, GPU upload) runs sequentially on the main thread. For large scenes (20x20 city = ~1800 prims), this is noticeable. The goal is to move the
expensive CPU work to a background thread so the main thread keeps rendering the old scene, then swap in results when ready. During background processing,
editing UI is disabled to prevent concurrent USD stage mutation.

## Approach: Edit Command Queue + JobSystem Fence

Uses the existing `JobSystem` for background execution. A `JobFence` tracks completion.

### New State in main()

```cpp
JobFence sceneUpdateFence;
bool editingBlocked = false;
std::vector<SceneEditCommand> pendingEdits;

// Pending results (written by background job, swapped on main)
RenderWorld pendingRenderWorld;
MeshLibrary pendingMeshLib;
MaterialLibrary pendingMatLib;
SceneQuerySystem pendingSceneQuery;
```

### SceneEditCommand struct

```cpp
struct SceneEditCommand {
    enum class Type { MuteLayer, SetTransform, SetVisibility, AddSubLayer, ClearSession };
    Type type;
    LayerHandle layer;
    PrimHandle prim;
    Transform transform;
    bool boolValue = false;
    std::string stringValue;
};
```

### Step 1: Draw callback queues edits instead of direct mutation

Replace all USDScene write calls in the draw callback with pushes to `pendingEdits`:

| Current | Replacement |
|---------|------------|
| `usdScene.setLayerMuted(h, v)` | `pendingEdits.push_back({MuteLayer, .layer=h, .boolValue=v})` |
| `usdScene.setTransform(h, t)` | `pendingEdits.push_back({SetTransform, .prim=h, .transform=t})` |
| `usdScene.setVisibility(h, v)` | `pendingEdits.push_back({SetVisibility, .prim=h, .boolValue=v})` |
| `usdScene.addSubLayer(p)` | `pendingEdits.push_back({AddSubLayer, .stringValue=p})` |
| `usdScene.clearSessionLayer()` | `pendingEdits.push_back({ClearSession})` |

Keep `setEditTarget()` and `saveAllDirty()` synchronous — they don't trigger recomposition.

### Step 2: Disable UI during background processing

When `editingBlocked` is true:
- Scene, Properties, and Layers windows show "Scene updating..." placeholder instead of interactive content
- AABB highlight subtree walk is skipped (reads USDScene hierarchy)
- Transform drags, visibility checkboxes, mute toggles are all behind the guard

### Step 3: Main loop update

```
Each frame:
  // Check if background work finished
  if (editingBlocked && sceneUpdateFence.ready()):
      swap pending → active (RenderWorld, MeshLib, MatLib, SceneQuery)
      renderer.uploadRenderWorld()  ← only GPU work on main thread
      editingBlocked = false

  // Kick off background work if edits are pending
  if (!editingBlocked && !pendingEdits.empty()):
      editingBlocked = true
      copy meshLib/matLib to pending copies
      move edits into lambda capture
      sceneUpdateFence = JobSystem::submit([...] {
          execute deferred edits on USDScene
          beginFrame() + processChanges() + endFrame()
          updateAssetBindings()
          extract() → pendingRenderWorld
          rebuild() → pendingSceneQuery
      })

  // Normal frame processing (no edits pending, not blocked)
  if (!editingBlocked):
      beginFrame() + processChanges() + endFrame()
      if dirty: extract + upload synchronously (rare — external changes only)
```

### Step 4: Thread safety guarantees

- **No concurrent access**: When background job runs, main thread only renders using OLD data. Draw callback shows placeholder, doesn't read USDScene.
- **No locks needed**: `pendingEdits` is written only on main thread, read only inside the job (after move). The fence is the only synchronization.
- **Clean shutdown**: `JobSystem::shutdown()` waits for all jobs. The `openScene` lambda calls `JobSystem::wait(sceneUpdateFence)` before loading a new scene.

### Step 5: Rapid edit batching

If user makes multiple edits while a background job is running, commands accumulate in `pendingEdits`. On next frame after the fence is ready and results are
swapped, the new batch is submitted. Natural debouncing.

## Files Modified

| File | Changes |
|------|---------|
| `src/main.cpp` | Add SceneEditCommand struct, JobFence + editingBlocked state, pendingEdits queue. Modify draw callback to queue edits + show placeholder. Modify main loop to submit/poll/swap. Guard AABB highlight. |

## Verification

1. Load `models/city/city_5x5/city.usda`
2. Open Layers window, mute `building.usda` — should see "Scene updating..." briefly, then buildings disappear without a frame stall
3. Unmute — buildings reappear smoothly
4. Open Properties, drag a transform — should queue and apply without stutter
5. Toggle visibility checkbox — same smooth behavior
6. While processing, verify all edit controls are greyed out / disabled
7. Rapid-click mute/unmute several times — should batch and not crash
8. Open `models/Kitchen_set/Kitchen_set.usd`, repeat mute test with a referenced layer
9. File > Open a different scene while background work is in progress — should wait for job, then open cleanly
