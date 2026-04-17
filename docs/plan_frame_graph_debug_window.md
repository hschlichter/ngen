# Frame Graph Debug Window

## Context

The render graph in `src/renderer/` is rebuilt, compiled, and executed every frame, but nothing about its state is visible at runtime — only stray `std::println` on pass culling. When passes get culled unexpectedly, a resource layout transition goes wrong, or a transient's lifetime is longer than expected, the only way to investigate is to add log lines and rebuild.

Goal: a toggleable ImGui window listing the compiled passes (in execution order) with their reads/writes, culled flags, and the resources flowing between them — so the data shape and dependency chain can be inspected interactively.

## Architecture

**Threading constraint**: the `FrameGraph` lives on the render thread (`RenderThread::threadLoop`), the ImGui window runs on the main thread. Data must be copied across, not aliased.

**Data flow**:
1. Main thread sets an atomic "want debug" flag on `RenderThread` whenever the window is open.
2. After `FrameGraph::compile()` runs on the render thread, if the flag is set, build a self-contained `FrameGraphDebugSnapshot` and store it under a dedicated mutex.
3. Main thread's `latestFrameGraphDebug()` getter consumes the latest snapshot before calling `editorUI.draw(...)`.
4. `EditorUI` caches the last non-empty snapshot so the window keeps rendering between publishes.

Gating on the flag keeps per-frame cost at zero while the window is closed.

## Snapshot shape

New file `src/renderer/framegraphdebug.h`:

```cpp
struct FgResourceAccessDebug {
    uint32_t resourceIndex;
    FgAccessFlags access;
};

struct FgPassDebug {
    std::string name;
    uint32_t executionIndex;   // index into executionOrder; UINT32_MAX if not scheduled
    bool scheduled;
    bool culled;
    bool hasSideEffects;
    std::vector<FgResourceAccessDebug> reads;
    std::vector<FgResourceAccessDebug> writes;
};

struct FgResourceDebug {
    uint32_t index;
    uint32_t width, height;
    const char* formatName;    // static string from toString(RhiFormat)
    const char* usageName;
    bool external;
    uint32_t firstUseOrder, lastUseOrder;   // UINT32_MAX if unused
    uint32_t producerPass;                   // first writer; UINT32_MAX if imported-only
    std::vector<uint32_t> consumerPasses;
    std::string label;         // e.g. "res#3 [1920x1080 R8G8B8A8_SRGB]" or "res#0 (imported)"
};

struct FrameGraphDebugSnapshot {
    uint64_t frameIndex = 0;
    std::vector<FgPassDebug> passes;           // source (insertion) order
    std::vector<uint32_t> executionOrder;      // indices into passes[]
    std::vector<FgResourceDebug> resources;
};
```

No raw pointers into render-thread memory — `const char*` fields point to static strings from `toString` helpers; all dynamic data is owned `std::string`/`std::vector`.

## Files to create

- **`src/renderer/framegraphdebug.h`** — structs above + inline `toString(RhiFormat)`, `toString(FgAccessFlags)`, `toString(RhiTextureUsage)` helpers returning `const char*` to static strings.
- **`src/ui/framegraphwindow.h`** — `void drawFrameGraphWindow(bool& show, const std::optional<FrameGraphDebugSnapshot>& snap, std::optional<uint32_t>& selPass, std::optional<uint32_t>& selResource);`
- **`src/ui/framegraphwindow.cpp`** — two-pane layout (see below).

## Files to modify

- **`src/renderer/framegraph.h`** — add public `auto buildDebugSnapshot() const -> FrameGraphDebugSnapshot;` and forward declaration.
- **`src/renderer/framegraph.cpp`** — implement `buildDebugSnapshot()`: iterate `passes`/`resources`/`passOrder` once, derive `producerPass` from `passes[i].writes` and `consumerPasses` from `reads` (same linear scan pattern already used in `compile()` at `framegraph.cpp:90-127`). Keeps raw vectors private, no friend/getter leak.
- **`src/renderer/renderer.h`** — add `auto frameGraphRef() const -> const FrameGraph& { return frameGraph; }`.
- **`src/renderer/renderthread.h`** — add members:
  ```cpp
  std::atomic<bool> fgDebugWanted{false};
  std::mutex fgDebugMutex;
  std::optional<FrameGraphDebugSnapshot> fgDebugSlot;
  uint64_t fgDebugFrameCounter = 0;
  ```
  Add methods `setFrameGraphDebugEnabled(bool)` and `latestFrameGraphDebug() -> std::optional<FrameGraphDebugSnapshot>`.
- **`src/renderer/renderthread.cpp`** — in `threadLoop` after `renderer->render(...)`:
  ```cpp
  if (fgDebugWanted.load(std::memory_order_relaxed)) {
      auto snap = renderer->frameGraphRef().buildDebugSnapshot();
      snap.frameIndex = ++fgDebugFrameCounter;
      std::lock_guard lock(fgDebugMutex);
      fgDebugSlot = std::move(snap);
  }
  ```
  Getter uses `std::exchange(fgDebugSlot, std::nullopt)` under the lock.
- **`src/ui/editorui.h`** — add `bool showFrameGraphWindow = false;`, `std::optional<uint32_t> fgSelectedPass;`, `std::optional<uint32_t> fgSelectedResource;`, `std::optional<FrameGraphDebugSnapshot> fgLastSnapshot;`, `getShowFrameGraphWindow() const`.
- **`src/ui/editorui.cpp`** — extend `EditorUI::draw(...)` signature to accept `std::optional<FrameGraphDebugSnapshot>&& freshSnap`. If non-empty, move it into `fgLastSnapshot`. Call `drawFrameGraphWindow(showFrameGraphWindow, fgLastSnapshot, fgSelectedPass, fgSelectedResource)`.
- **`src/ui/mainmenubar.h`** — add `bool& showFrameGraphWindow` to `MainMenuBarState`.
- **`src/ui/mainmenubar.cpp`** — add `ImGui::MenuItem("Frame Graph", nullptr, &state.showFrameGraphWindow);` inside the Debug menu (after `Show Buffer Overlay`, around line 95) with a surrounding `Separator`.
- **`src/main.cpp`** — before `editorUI.draw(...)` at line 314:
  ```cpp
  renderThread.setFrameGraphDebugEnabled(editorUI.getShowFrameGraphWindow());
  auto fgSnap = renderThread.latestFrameGraphDebug();
  ```
  Pass `std::move(fgSnap)` into `editorUI.draw(...)`.

The Makefile uses `find src -name '*.cpp'` (line 41) — new files are picked up automatically.

## Window layout

`ImGui::Begin("Frame Graph", &show)`. Header line: `"Frame #N — P passes (C culled), R resources"`.

`BeginTable` with two resizable columns.

**Left pane** — `BeginTable("passes", 4)` columns `#`, `Name`, `R`, `W`. Rows iterated in `executionOrder`. Each row is `Selectable`; clicking sets `selPass = originalIndex, selResource = nullopt`. Culled rows use `ImGuiCol_TextDisabled`. R/W counts shown as small colored chips by access flag.

**Right pane** — context-sensitive:
- Pass selected: name, flags (culled, side-effects, exec index), two sub-tables for Reads and Writes. Each resource row is a button that sets `selResource` (and clears `selPass`).
- Resource selected: label, full desc (w/h, formatName, usageName, external), `firstUseOrder..lastUseOrder`, producer button, list of consumer buttons. Clicking any button switches selection back to the corresponding pass.

Deliberately list-based — a DAG renderer with `ImDrawList` is a follow-up once the list proves useful. Existing imnodes is not vendored; keeping it list-based avoids adding a dependency.

## Verification

1. `make` — build succeeds (no Makefile edit needed).
2. Run `./_out/ngen` with any USD scene (e.g. `./_out/ngen assets/something.usda`).
3. `Debug` menu → `Frame Graph` — window appears.
4. Pass list shows (in execution order): `GeometryPass`, `LightingPass`, `DebugRenderer`, `GizmoPass`, `EditorUIPass`. None culled.
5. Click `GeometryPass` — right pane shows three writes (albedo/normal/depth) with sizes matching the window.
6. Click a GBuffer resource chip — detail shows producer=GeometryPass, consumer=LightingPass, lifetime spans those two execution indices.
7. Resize the window — on the next frame the resource width/height in the snapshot update.
8. Close the window — confirm (via breakpoint or temporary log) that `buildDebugSnapshot` is no longer called on subsequent frames (`fgDebugWanted` is false).
9. Toggle the window closed+open — selection state persists (lives on `EditorUI`), snapshot refreshes.
