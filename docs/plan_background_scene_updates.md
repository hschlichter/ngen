# Plan: Background Threaded Scene Updates

## Context

When muting/unmuting layers, changing transforms, or toggling visibility, the engine stalls visibly because all work (USD recomposition, prim cache rebuild, mesh extraction, GPU upload) runs sequentially on the main thread. For large scenes (20x20 city = ~1800 prims), this is noticeable. The goal is to move the expensive CPU work to a background thread so the main thread keeps rendering the old scene, then swap in results when ready. During background processing, editing UI is disabled to prevent concurrent USD stage mutation.

## Approach: Edit Command Queue + Background Worker

All changes are contained in `src/main.cpp`. No other files need modification.

### New State in main()

```
std::atomic<int> sceneUpdateState = 0;  // 0=Idle, 1=Processing, 2=ResultReady
bool editingBlocked = false;
std::vector<SceneEditCommand> pendingEdits;
std::jthread backgroundThread;

// Pending results (written by background thread, swapped on main)
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

### Step 3: Main loop three-phase update

```
Each frame:
  if (state == ResultReady):
      join background thread
      swap pending → active (RenderWorld, MeshLib, MatLib, SceneQuery)
      renderer.uploadRenderWorld()  ← only GPU work on main thread
      state = Idle
      editingBlocked = false
  
  if (state == Idle && !pendingEdits.empty()):
      editingBlocked = true
      state = Processing
      copy meshLib/matLib to pending copies
      move edits to background thread
      launch jthread:
          execute deferred edits on USDScene
          processChanges() (prim rebuild + transforms)
          updateAssetBindings()
          extract() → pendingRenderWorld
          rebuild() → pendingSceneQuery
          state = ResultReady
  
  if (state == Idle && pendingEdits.empty()):
      run processChanges() synchronously (for external changes)
      if dirty: extract + upload synchronously (rare path)
```

### Step 4: Thread safety guarantees

- **No concurrent access**: When background thread runs, main thread only renders using OLD data (active RenderWorld/MeshLib/etc). Draw callback shows placeholder, doesn't read USDScene.
- **No locks needed**: The atomic state variable is the only synchronization. Writes to pendingEdits happen only on main thread (before launch). Reads happen only on background thread (after launch).
- **Clean shutdown**: `std::jthread` joins automatically on destruction. Explicit join before `openScene` and before program exit.

### Step 5: Rapid edit batching

If user makes multiple edits while background is Processing, commands accumulate in `pendingEdits`. On next Idle transition, they're all batched into one background pass. Natural debouncing.

## Files Modified

| File | Changes |
|------|---------|
| `src/main.cpp` | Add SceneEditCommand struct, threading state, edit queue. Modify draw callback to queue edits + show placeholder. Modify main loop to use 3-phase background update. Guard AABB highlight and raycast. |

## Verification

1. Load `models/city/city_5x5/city.usda`
2. Open Layers window, mute `building.usda` — should see "Scene updating..." briefly, then buildings disappear without a frame stall
3. Unmute — buildings reappear smoothly
4. Open Properties, drag a transform — should queue and apply without stutter
5. Toggle visibility checkbox — same smooth behavior
6. While Processing, verify all edit controls are greyed out / disabled
7. Rapid-click mute/unmute several times — should batch and not crash
8. Open `models/Kitchen_set/Kitchen_set.usd`, repeat mute test with a referenced layer
9. File > Open a different scene while background work is in progress — should join thread first, then open cleanly
