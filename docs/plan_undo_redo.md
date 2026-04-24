# Basic Undo/Redo

## Context

The Preview/Authoring split (`docs/architecture_preview_vs_authoring.md`) gave us a clean *commit boundary*: every interactive edit ends with exactly one
`Authoring` `SceneEditCommand`. That's the natural undo step — Preview edits are by definition transient, so the existing architecture already separates
"in-flight" from "committed" cleanly.

This plan adds a stack of inverse-command snapshots that the user can rewind/replay with **Ctrl+Z** / **Ctrl+Shift+Z**.

## Scope (v1)

- Edit types: **`SetTransform`** and **`SetVisibility`** only. Both are symmetric — the inverse is "the same op with the prior value", computable by reading
  scene state at record time.
- **One frame = one undo step.** All Authoring commits queued during a single frame coalesce into one entry. Handles the type-in-a-value case (DragFloat3 fires
  `changed` + `IsItemDeactivatedAfterEdit` on the same frame, queueing both Preview and Authoring) and future multi-select drags transparently.
- Selection state is **not** undoable.
- In-memory only; cleared on scene close. No persistence.
- Stack capped at 100 entries (per direction); oldest dropped when full.

## Architecture

### Inversion model

Each `Authoring` `SceneEditCommand` is recorded as a pair: `forward` (what the user did) and `reverse` (what reverses it). `reverse` is computed by reading the
prim's *current* state from `usdScene` immediately before the command applies. Since the recording happens synchronously inside `SceneUpdater::update` (main
thread, before either the fast path or the async dispatch mutates anything), the read is consistent.

Undo = re-queue the `reverse` cmds. Redo = re-queue the `forward` cmds. Both flow through the existing `SceneUpdater` machinery — no special apply path. They
carry a flag (`fromHistory = true`) so they're not re-recorded into the stack.

### Files to add

- `src/scene/undostack.h`
- `src/scene/undostack.cpp`
- `src/ui/undowindow.h`
- `src/ui/undowindow.cpp`

```cpp
// undostack.h
struct UndoEntry {
    std::vector<SceneEditCommand> forward;
    std::vector<SceneEditCommand> reverse;
};

class UndoStack {
public:
    // Capture inverses for `cmds` by reading current scene state, then push as
    // one entry. Skips cmd types we don't support (silently). Skips cmds
    // flagged `fromHistory`. Clears the redo stack on any new entry.
    void recordBatch(std::span<const SceneEditCommand> cmds, const USDScene& scene);

    bool canUndo() const;
    bool canRedo() const;

    // Pop the top entry; return its `reverse` cmds (with fromHistory=true) for
    // the caller to re-queue. Moves the entry onto the redo stack.
    std::vector<SceneEditCommand> undo();
    std::vector<SceneEditCommand> redo();

    void clear(); // called from openScene

private:
    std::vector<UndoEntry> m_undo;
    std::vector<UndoEntry> m_redo;
    static constexpr size_t kMaxDepth = 100;
};
```

### Files to modify

| File | Change |
|---|---|
| `src/ui/editcommand.h` | Add `bool fromHistory = false;` field. |
| `src/scene/sceneupdater.h` | Own an `UndoStack`; expose `undoStack()` accessor. |
| `src/scene/sceneupdater.cpp` | In `update()`, immediately before applying `pendingEdits` (in *both* the fast path and the Phase 2 dispatch), call `undoStack.recordBatch(pendingEdits, usdScene)`. The `fromHistory` flag prevents replay loops. |
| `src/main.cpp` | Wire **Ctrl+Z** and **Ctrl+Shift+Z** in the SDL event loop: pop from `undoStack`, queue returned cmds via `sceneUpdater.addEdit`. Clear undo stack inside the existing `openScene` path. |
| `src/ui/mainmenubar.cpp` + `mainmenubar.h` | Add an **Edit** menu with `Undo` (Ctrl+Z) / `Redo` (Ctrl+Shift+Z) entries. Disabled when the stack is empty. Add **History** entry to Windows menu. |
| `src/ui/editorui.{h,cpp}` | Own a `bool showUndoWindow` flag (default false; auto-shown on `openScene` like the others). Include in `togglePanels()`. Call `drawUndoWindow` from `EditorUI::draw`. |

### Undo history window

A simple read-only panel that visualizes the stacks. Mirrors the structure of `scenewindow.{h,cpp}`.

```cpp
// undowindow.h
void drawUndoWindow(bool& show, const UndoStack& stack, const USDScene& scene);
```

Layout:

```
History                                 [ ]
─────────────────────────────────────────
[Undo]  [Redo]      Depth: 4 / 100
─────────────────────────────────────────
  Move  /Kitchenset/Geom/Chair_3       (1)
  Move  /Kitchenset/Geom/Fridge        (1)
► Hide  /Kitchenset/Lights/Bulb        (1)        ← cursor (top of undo)
─────────────────────────────────────────
  Move  /Kitchenset/Geom/Chair_3       (1)        ← redo entries (greyed)
─────────────────────────────────────────
```

- Two action buttons at top mirror Edit menu (also bound to Ctrl+Z / Ctrl+Shift+Z).
- One row per `UndoEntry` in chronological order; the row right above the divider is the one that Ctrl+Z would undo next ("►" cursor).
- Below the divider: redo entries, greyed out.
- Each row: short label per cmd type (`Move` for `SetTransform`, `Hide`/`Show` for `SetVisibility`), the prim's path (looked up via
  `scene.getPrimRecord(cmd.prim)->path`), and the cmd-count in parens for grouped entries (e.g. multi-select).
- `UndoStack` exposes `std::span<const UndoEntry> undoEntries() const` and `redoEntries() const` for the window to read; no mutation API beyond the existing
  `undo()`/`redo()`.

For v1 the rows are not clickable. Jump-to-entry can come later — would just call `undo()` / `redo()` in a loop.

### Inversion specifics

In `UndoStack::recordBatch`, for each cmd:

- `SetTransform`: read `scene.getTransform(cmd.prim)->local` → use as the reverse's `transform`. Reverse `purpose = Authoring` (always — Preview is never
  recorded).
- `SetVisibility`: read `scene.getPrimRecord(cmd.prim)->visible` → use as the reverse's `boolValue`.
- Other types (`MuteLayer`, `AddSubLayer`, `ClearSession`): skipped in v1.

Cmds with `fromHistory = true` are skipped entirely — they're already replayed entries, recording them would duplicate the history.

If `pendingEdits` contains a mix of recorded and non-recorded types, only the recorded ones go into the entry. The entry is pushed only if non-empty.

### Frame coalescing

Because `SceneUpdater::update()` is called once per frame and `pendingEdits` accumulates everything queued during that frame's event loop + UI draw, "one frame
= one undo step" falls out for free: `recordBatch` receives the full batch and produces a single `UndoEntry`.

The Properties panel's per-frame Preview-then-Authoring case works correctly: the Preview cmd is filtered out of the batch (`purpose != Authoring`), only the
Authoring cmd is recorded. Need to confirm in implementation that `recordBatch` filters by purpose too.

### Replay loop prevention

Pressing Ctrl+Z queues the `reverse` cmds with `fromHistory = true`. They go through `pendingEdits` like any other edit. Next frame `SceneUpdater::update()`
calls `recordBatch`, which sees `fromHistory = true` on every cmd → skips them entirely → no new entry pushed → no infinite loop. The redo stack is preserved
(because `recordBatch` only clears redo when it actually pushes).

## Reused existing pieces

- `SceneEditCommand` (`src/ui/editcommand.h`) — gains one bool field, otherwise unchanged.
- `SceneUpdater::addEdit` (`src/scene/sceneupdater.h:24`) — undo/redo enqueue through the same path.
- Fast-path dedup in `sceneupdater.cpp` — undo cmds dedupe naturally per prim. (Edge case: if a single undo entry contains transforms for prim X plus visibility
  for prim X, the dedup-then-fast-path check `all_of == SetTransform` fails, so it falls through to the async batch. Acceptable.)
- `USDScene::setTransform` / `setVisibility` — used unchanged by replay.

## Out of scope (deferred)

- `MuteLayer`, `AddSubLayer`, `ClearSession` undo (need inverse ops on `USDScene`).
- Coalescing across frames (e.g., merging two consecutive transform commits within 200ms into one entry — useful for keystroke-heavy scrubbing).
- Click-to-jump in the History window (call `undo()` / `redo()` repeatedly).
- Selection on a separate stack.
- Persistence across sessions.
- Branching history.

## Verification

1. `bear -- make` builds clean.
2. Drag a prim with the gizmo, release. Press Ctrl+Z → prim returns to original position. Ctrl+Shift+Z → returns to dragged position.
3. Drag prim A, then drag prim B. Ctrl+Z twice → both back at originals; Ctrl+Shift+Z twice → both at dragged.
4. Type a value into Properties Position. Press Enter. Ctrl+Z → original value restored.
5. Toggle a prim's visibility checkbox. Ctrl+Z → restored.
6. Drag a parent (so descendants move too). Ctrl+Z → parent's transform reverts; descendants follow because we recorded only the parent's local — patch path
   handles descendants via `appendSubtree`.
7. Edit menu: Undo grays out when stack empty; Redo grays out when redo stack empty (and after any new edit).
8. Open a different scene → both stacks cleared (no stale undo into the wrong scene's prims).
9. Hold Ctrl+Z to spam undo: should cleanly walk back without crashes or replay loops.
10. Open the **History** window (Windows menu). Confirm entries appear with prim paths, cursor (►) marks the next-to-undo, redo entries appear greyed below the
    divider, and counts move correctly as Ctrl+Z / Ctrl+Shift+Z is pressed. Window is included in Ctrl+E toggle.
11. `make format` — no diff.
