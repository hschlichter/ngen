# Render Observations — Implementation Plan

Iterable. Push back on anything.

Companion to [`plan_observability.md`](plan_observability.md), which defined the bus and shipped Phase 1 (`Scene` + `Engine` categories). This doc covers the `Render` category only — Phase 2 for the renderer. RHI is explicitly out of scope; emissions live in `src/renderer/`, never in `src/rhi/`.

---

## 1. Scope

**In:** decisions and state changes inside the renderer frontend — frame lifecycle, frame graph compile/execute, pass execution and culling, swapchain recreation, GPU resource upload on cache miss, render-world handoff from main thread, render-thread lifecycle.

**Out (by design):**
- Anything in `src/rhi/` — that's a hardware abstraction layer, not a Claude-relevant concern. No RHI command recording, queue submits, resource creation, or device events.
- Per-draw / per-instance / per-vertex emissions — hot-loop noise (see `plan_observability.md` §3.2 "Explicitly not instrumented").
- Transient resource pool allocate/reuse/release (chatty; behind a future opt-in category if ever needed).
- Descriptor-pool recreation, `RenderSnapshotProcessed`, `FrameGraphDebugSnapshotCaptured` — internal/redundant (see parent plan).

---

## 2. Hook sites (verified)

All sites run on the **render thread** unless noted. Line numbers below were verified against the current code — recheck before coding, they drift.

| Emission | Site | Fields | Notes |
|---|---|---|---|
| `FrameBegin` | `src/renderer/renderer.cpp:332`, top of `Renderer::render()` | `frame` (monotonic uint64) | Single frame entry point. See §3 for the counter. |
| `FrameEnd` | `src/renderer/renderer.cpp:478`, after `device->present()` | `frame` | Same value as the matching `FrameBegin`; lets readers pair them without timestamp math. Emit even on present failure; `SwapchainRecreate` narrates the failure separately. |
| `FrameGraphCompiled` | `renderer.cpp:462`, after `frameGraph.compile()` | `pass_count`, `culled_count` | Needs new public accessors on `FrameGraph` — see §3. |
| `PassExecuted` | `src/renderer/framegraph.cpp:303`, inside execute loop, before `passes[i].execute(ctx)` | — (name = pass name) | Single site, not per-pass file. See §4. |
| `PassCulled` | `src/renderer/framegraph.cpp:179-184`, cull marking loop | `reason` (`"no_downstream_reads"`) | Only one cull reason exists today; keep the field so future reasons don't change schema. |
| `SwapchainRecreate` | `renderer.cpp:338` (acquire-failed) and `:479` (present-failed) | `reason` (`"acquire_failed"` / `"present_failed"`) | Two sites, same emission shape. |
| `MeshUploaded` | `renderer.cpp:~224`, inside `if (!meshCache.contains(...))` branch | `vertex_count`, `index_count` | Cache-miss only. |
| `TextureUploaded` | `renderer.cpp:~238`, inside `if (!textureCache.contains(...))` branch | `width`, `height` | Cache-miss only. |
| `SceneUploadReceived` | `src/renderer/renderthread.cpp:62-65`, `if (pendingUpload)` block | `mesh_count` (from `world.meshInstances.size()`) | Material / instance counts not trivially exposed — see §5. |
| `RenderThreadStart` | `renderthread.cpp:~9`, in `RenderThread::start()` | — | **Main thread** — the only main-thread emission in this category. |
| `RenderThreadStop` | `renderthread.cpp:~20`, in `RenderThread::stop()` after join | — | **Main thread.** |

`name` convention: for frame-scope events, use a stable string like `"frame"` or the frame graph name; for pass events use the pass's own `name` member; for swapchain/thread events use `"swapchain"` / `"RenderThread"`.

---

## 3. API additions needed

### 3.1 Frame counter on `Renderer`

A monotonic `uint64_t` incremented once per `Renderer::render()` call. Lives on the render thread; never read from any other thread. One of the existing debug paths already has `fgDebugFrameCounter` (`renderthread.cpp:76`) but it only increments when the frame-graph debug snapshot is enabled, so it's not a reliable counter for always-on observations.

```cpp
// src/renderer/renderer.h — private member
uint64_t m_frameIndex = 0;

// src/renderer/renderer.cpp — top of Renderer::render()
auto frame = ++m_frameIndex;
OBS_EVENT("Render", "FrameBegin", "frame").field("frame", frame);
// ... at end of render(), after present():
OBS_EVENT("Render", "FrameEnd", "frame").field("frame", frame);
```

Keep the counter on `Renderer`, not on `RenderThread` — `Renderer::render()` is the single frame boundary and owning it there means no plumbing. Pre-increment (start at 1) so frame 0 never appears; readers don't have to guess whether 0 means "first frame" or "uninitialized."

### 3.2 FrameGraph counts

`FrameGraph` does not currently expose `pass_count` / `culled_count`. Add two public methods:

```cpp
// src/renderer/framegraph.h
auto passCount() const -> size_t { return passes.size(); }
auto culledCount() const -> size_t {
    return std::count_if(passes.begin(), passes.end(),
                         [](const PassNode& p) { return p.culled; });
}
```

Trivial, observation-only. Alternative (return a struct from `compile()`) is more invasive and not worth it for two counts.

---

## 4. `PassExecuted`: one site, not nine

The abstract plan said "Execute lambda in each of 9 `src/renderer/passes/*.cpp`." Reconsidered: a single hook inside `framegraph.cpp`'s execute loop is strictly better here.

- **Nine per-pass emissions:** boilerplate in every pass, nine opportunities to forget one, and every new pass added later needs the same copy-paste.
- **One centralized emission at `framegraph.cpp:303`:** fires for every pass by construction, automatically picks up new passes, reads `passes[i].name` which the passes already author. Same observation shape as the per-pass version — the observation says "this pass ran," which is the intent.

Pick centralized. The parent plan's per-pass wording is a finding-worth-correcting, not a requirement.

(If a specific pass later wants to emit *additional* observations — `shadowpass` emitting cascade counts, `lightingpass` emitting light-cluster stats — those go in the pass file as ordinary observations, not as replacements for `PassExecuted`.)

---

## 5. `SceneUploadReceived` field availability

`RenderWorld` exposes `meshInstances` (countable). Material and instance counts are not directly surfaced:

- **`mesh_count`**: `world.meshInstances.size()` — trivial.
- **`material_count`**: no direct accessor. Options: (a) iterate `meshInstances` unique-by-material (O(N), fine for current scales); (b) add a `materialCount()` accessor on `RenderWorld`; (c) drop the field in Phase 2 and add later when needed.
- **`instance_count`**: same situation as materials.

Recommendation: ship `mesh_count` only in Phase 2; treat materials/instances as a follow-up. The parent plan listed all three but this is exactly the kind of thing that gets adjusted when the code is in front of us.

---

## 6. Thread-safety notes

All render-thread emissions are already safe — `emit()` is lock-free and the bus is MPSC-ready. No new synchronization.

One subtle thing: `RenderThreadStart` emits *before* the render thread exists. Emitting from `start()` is on the main thread, which is fine. `RenderThreadStop` emits on the main thread *after* the join completes, so any in-flight render-thread emissions are already enqueued ahead of the stop marker — ordering is honest.

Category filter is still frozen by the time any of these hooks fire: `main.cpp` installs the sink and configures categories before `RenderThread::start()` is called.

---

## 7. Rollout

One phase; the whole set is small enough.

**Step 1.** Add `passCount()` / `culledCount()` to `FrameGraph`. Confirms the only API change lands cleanly.

**Step 2.** Wire the 10 emissions in order:
1. `RenderThreadStart` / `RenderThreadStop` (simplest, main-thread, proves the sink path from both threads)
2. `SceneUploadReceived` (handoff — useful boundary marker for every subsequent emission)
3. `FrameBegin` / `FrameEnd` (bracket everything else; reading `obs.jsonl` becomes structurally scannable)
4. `FrameGraphCompiled` + `PassCulled` (compile-time decisions)
5. `PassExecuted` (runtime decisions)
6. `SwapchainRecreate` (rare; tests the error-path emissions)
7. `MeshUploaded` / `TextureUploaded` (upload-path emissions)

**Step 3.** Smoke-test by running the editor, moving the camera to force a frame or two, loading a fresh scene to force uploads, and resizing the window to force swapchain recreation. Confirm each observation type appears at least once in the resulting `.jsonl`.

**Step 4.** Update `obs.md` only if we find emissions whose field shape differs from what §9 of the parent plan documented.

Estimate: ~1 hour of code, most of it repetitive `OBS_EVENT(...)` lines.

---

## 8. What we're *not* adding

- Timing / duration fields on any Render emission. Observations answer "what happened," not "how fast" (parent plan §8). Profiling is a separate system.
- GPU-side observations (pipeline binds, draw counts, descriptor writes). That's RHI territory.
- Frame-number fields on emissions *other than* `FrameBegin`/`FrameEnd`. Those two bracket everything; readers can compute frame membership from ordering without every row carrying the number.
- Per-pass custom shape (e.g. `ShadowCascadesRendered`, `LightClusterBinned`). Those are follow-ups once the basic `PassExecuted` narration proves useful.
