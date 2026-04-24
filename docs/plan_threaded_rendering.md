# Pipelined Rendering Architecture Plan

## 1. Overview

Split the engine into a 3-stage pipeline:

```
Frame N:   [Main: update + prepare]  -->  [Render: build FG + record CB + submit]  -->  [GPU: execute]
Frame N+1: [Main: update + prepare]  -->  [Render: ...]                             -->  ...
```

The main thread runs at most one frame ahead of the render thread. The render thread runs at most one frame ahead of the GPU. Total pipeline depth: 3 frames.

## 2. What Moves Where

### Main Thread (stays)
- SDL event polling (required by SDL)
- Camera update
- Scene logic (sceneUpdater)
- ImGui new frame + UI logic + `ImGui::Render()` (all ImGui calls on one thread)
- Debug draw generation
- Snapshot creation: copy per-frame render state into a `RenderSnapshot`
- Scene file open / hot-reload logic

### Render Thread (new, dedicated `std::jthread`)
- Wait for a new `RenderSnapshot` from main thread
- Wait for in-flight GPU fence (for the frame slot being reused)
- Acquire swapchain image
- Write UBO (memcpy into mapped buffer)
- Build frame graph, compile, record command buffer
- Submit + present
- Signal main thread that the frame slot is available

### GPU (unchanged)
- Executes submitted command buffers asynchronously
- Signals fences and semaphores

## 3. Data Types

Two separate channels pass data from the main thread to the render thread: a per-frame snapshot (every frame, lightweight) and a scene upload channel
(infrequent, heavy).

### RenderSnapshot (per-frame, every frame)

A self-contained, value-type snapshot of everything the render thread needs to draw one frame. No pointers or references to main-thread data that could be
mutated.

```cpp
struct RenderSnapshot {
    // Camera
    glm::mat4 viewMatrix;
    glm::mat4 projMatrix;

    // Viewport
    int windowWidth;
    int windowHeight;

    // Render settings
    GBufferView gbufferViewMode;
    bool showBufferOverlay;

    // Debug lines (moved, not copied)
    DebugDrawData debugData;

    // ImGui draw data (deep-copied from ImGui::GetDrawData())
    ImGuiFrameSnapshot imguiSnapshot;
};
```

`DebugDrawData` is **moved** into the snapshot. `ImGuiFrameSnapshot` is a deep copy of ImGui's draw lists.

### RenderUpload (infrequent, on scene change)

Scene changes (file open, edit) are a separate concern from per-frame rendering. They travel through a dedicated channel, not inside the snapshot.

```cpp
struct RenderUpload {
    RenderWorld world;
    std::shared_ptr<const MeshLibrary> meshLib;
    std::shared_ptr<const MaterialLibrary> matLib;
};
```

Use `shared_ptr` for libraries to avoid copying vertex/texture data. The main thread pushes a `RenderUpload` into a separate slot; the render thread checks for
pending uploads before each frame.

## 4. Thread Lifecycle

### Startup
1. Main thread initializes SDL, Vulkan device, swapchain, renderer (all as today)
2. Main thread creates the render thread
3. Render thread enters loop, blocks waiting for first snapshot

### Render thread loop
```
while not shutdown:
    snapshot = waitForSnapshot()           // blocks on condvar
    waitForFence(inflightFences[frame])   // GPU done with this slot
    resetFence(inflightFences[frame])

    if pendingRenderUpload:
        processRenderUpload()               // calls waitIdle, separate channel

    index = acquireNextImage(...)
    if failed: signal slot available, continue

    write UBO from snapshot matrices
    build frame graph using snapshot data
    compile + record + submit + present

    signal frame slot available
    frame = (frame + 1) % imageCount
```

### Shutdown
1. Main thread pushes sentinel snapshot
2. Render thread exits loop
3. Main thread joins render thread
4. Main thread calls `renderer.destroy()` (waitIdle + cleanup)

### Why a dedicated thread, not the JobSystem
The render thread blocks on GPU fences and swapchain acquire. Using the JobSystem (work-stealing pool) would tie up a worker indefinitely. A dedicated
`std::jthread` is simpler.

## 5. Synchronization: Double-Buffered Snapshot Slot

A single-slot handoff with back-pressure. Not a queue — the main thread can prepare at most 1 frame ahead.

```cpp
struct SnapshotSlot {
    std::mutex mutex;
    std::condition_variable snapshotReady;    // render thread waits
    std::condition_variable slotAvailable;    // main thread waits
    RenderSnapshot snapshot;
    bool hasSnapshot = false;
    bool shutdown = false;
};
```

**Protocol:**
1. **Main produces**: Lock. If `hasSnapshot`, wait on `slotAvailable`. Move snapshot in. Set `hasSnapshot = true`. Notify `snapshotReady`.
2. **Render consumes**: Lock. Wait on `snapshotReady` until `hasSnapshot`. Move snapshot out. Set `hasSnapshot = false`. Notify `slotAvailable`.

**Frame pacing** is automatic: if the GPU is slow, the render thread blocks on `waitForFence`, which delays consuming the snapshot, which causes the main thread
to block on `slotAvailable`.

## 6. ImGui Integration

ImGui is single-threaded: `NewFrame()`, UI calls, `Render()` must all happen on the same thread.

### Split into two phases

**Phase 1 — Main thread:**
1. `ImGui_ImplVulkan_NewFrame()`, `ImGui_ImplSDL3_NewFrame()`, `ImGui::NewFrame()`
2. All UI draw calls (`editorUI.draw(...)`)
3. `ImGui::Render()` to finalize draw lists
4. **Clone** `ImGui::GetDrawData()` into an `ImGuiFrameSnapshot` (deep copy of vertex/index/command buffers)

**Phase 2 — Render thread:**
1. `ImGui_ImplVulkan_RenderDrawData()` with the cloned draw data

### ImGuiFrameSnapshot

ImGui provides `ImDrawData::Clone()` since 1.89.9. Use that if available, otherwise deep-copy manually:
- Copy the `ImDrawData` header
- Deep-copy each `ImDrawList` (VtxBuffer, IdxBuffer, CmdBuffer are `ImVector`s that own memory)
- Fix up the `CmdLists` pointer array

### RhiEditorUI refactoring

Split the current `render()` method:
- `beginFrame()` — calls NewFrame functions
- `endFrame() -> ImGuiFrameSnapshot` — calls `ImGui::Render()`, clones draw data
- `renderDrawData(cmd, snapshot)` — called on render thread, only does `ImGui_ImplVulkan_RenderDrawData`

## 7. Scene Uploads

Currently `uploadRenderWorld` calls `device->waitIdle()` which stalls the GPU. Scene changes are infrequent (file open, edits) and fundamentally different from
per-frame rendering.

### Separate channel

Scene uploads use their own synchronization, independent of the per-frame snapshot slot:

```cpp
struct RenderUploadSlot {
    std::mutex mutex;
    std::optional<RenderUpload> pending;  // set by main, consumed by render
};
```

**Protocol:**
1. **Main thread** detects scene change. Locks mutex, moves `RenderUpload` into `pending`.
2. **Render thread** checks `pending` at the top of each frame (after fence wait). If set, locks mutex, takes the upload, processes it (`waitIdle` + GPU
   resource upload). The stall is contained to the render thread.
3. **Main thread** can continue preparing frames. If the render thread is still uploading, the main thread is naturally throttled by the snapshot slot
   back-pressure (render thread hasn't consumed the previous snapshot yet).

No version counters needed — the `std::optional` is either empty or contains the latest upload. If the main thread pushes two uploads before the render thread
processes one, the second overwrites the first (latest wins).

## 8. Updated Main Loop

```
initialization (unchanged, main thread)
renderThread.start(&renderer)
sceneVersion = 0

while !quit:
    // 1. Timing
    dt = computeDeltaTime()

    // 2. Scene file open (if pending)
    // 3. Scene update (returns bool sceneChanged, no longer takes Renderer&)
    // 4. SDL events
    // 5. Camera update
    // 6. Debug draw

    // 7. ImGui frame (main thread)
    editorui->beginFrame()
    editorUI.draw(...)
    imguiSnapshot = editorui->endFrame()

    // 8. Compute matrices (main thread, no SDL needed on render thread)
    viewMatrix = cam.viewMatrix()
    projMatrix = perspective(...)

    // 9. Scene upload (separate channel)
    if sceneChanged:
        renderThread.submitRenderUpload(RenderUpload{renderWorld, meshLib, matLib})

    // 10. Build snapshot
    snapshot = { viewMatrix, projMatrix, windowSize, debugData, imguiSnapshot, ... }

    // 11. Submit (blocks if render thread hasn't consumed previous snapshot)
    renderThread.submitSnapshot(move(snapshot))

renderThread.stop()
```

## 9. Implementation Steps

### Step 1: Introduce RenderSnapshot, compute matrices on main thread
- Create `rendersnapshot.h`
- Move view/proj computation from `Renderer::render()` to main loop
- Pass through snapshot instead of `Camera` + `SDL_Window*`
- Everything still single-threaded; pure refactor

### Step 2: Split ImGui phases
- Add `beginFrame()`, `endFrame()`, `renderDrawData()` to RhiEditorUI
- Implement `ImGuiFrameSnapshot` with draw-data cloning
- Main loop calls `beginFrame/draw/endFrame`; frame graph EditorUI pass uses `renderDrawData`
- Still single-threaded; verify ImGui renders correctly

### Step 3: Decouple scene uploads
- `SceneUpdater::update` returns bool, no longer takes `Renderer&`
- Create `RenderUpload` struct and `RenderUploadSlot`
- Main loop pushes `RenderUpload` through the separate channel on change
- `Renderer::uploadScene()` called by render logic when pending upload exists
- Still single-threaded

### Step 4: Extract Renderer::renderFrame(const RenderSnapshot&)
- Single method containing: fence wait, acquire, UBO write, frame graph build/compile/execute, submit, present
- Main loop calls it directly (still single-threaded)

### Step 5: Introduce the render thread
- Create `RenderThread` with `SnapshotSlot`
- Spawn `std::jthread`
- Main loop calls `renderThread.submitSnapshot()` instead of `renderFrame()`
- Proper shutdown sequence

### Step 6: Stress test and tune
- Verify 1-frame-ahead pipeline overlap
- Profile main-thread time vs render-thread time
- Handle edge cases: window resize (swapchain recreation on render thread), minimize

## 10. Edge Cases

### Window resize
Swapchain is render-thread-owned. On `acquireNextImage` failure, the render thread recreates it. Main thread's window dimensions may be 1 frame stale —
acceptable.

### Vulkan threading
Command buffer recording and queue submission happen only on the render thread. Device functions are thread-safe when objects aren't used concurrently.
`waitIdle()` in scene upload is safe (only the render thread submits).

### ImGui context
All ImGui calls on main thread. Only `ImGui_ImplVulkan_RenderDrawData` on render thread — it reads cloned data, not the ImGui context. Font descriptor sets are
allocated at init (before render thread starts) and immutable.

### Shutdown ordering
1. Main thread pushes sentinel
2. Render thread exits
3. Main thread joins
4. Main thread destroys (waitIdle + cleanup)

## 11. Files to Create or Modify

| File | Action | Description |
|------|--------|-------------|
| `src/renderer/rendersnapshot.h` | Create | RenderSnapshot, RenderUpload, ImGuiFrameSnapshot |
| `src/renderer/renderthread.h` | Create | RenderThread, SnapshotSlot, RenderUploadSlot |
| `src/renderer/renderthread.cpp` | Create | Thread loop, synchronization |
| `src/renderer/renderer.h` | Modify | Add renderFrame(snapshot), uploadScene(), remove SDL_Window* from render() |
| `src/renderer/renderer.cpp` | Modify | Implement renderFrame, extract matrix computation |
| `src/rhi/rhieditorui.h` | Modify | Add beginFrame(), endFrame(), renderDrawData() |
| `src/rhi/vulkan/rhieditoruivulkan.h` | Modify | Override new methods |
| `src/rhi/vulkan/rhieditoruivulkan.cpp` | Modify | Split render() into phases |
| `src/scene/sceneupdater.h` | Modify | Remove Renderer& param, return bool |
| `src/scene/sceneupdater.cpp` | Modify | Remove direct uploadRenderWorld calls |
| `src/main.cpp` | Modify | New main loop per section 8 |
| `src/debugdraw.h` | Modify | Add move support for DebugDrawData |
| `README.md` | Modify | Update architecture diagram and features to reflect threaded rendering |
