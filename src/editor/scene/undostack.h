#pragma once

#include "editcommand.h"

#include <span>
#include <vector>

class USDScene;

struct UndoEntry {
    std::vector<SceneEditCommand> forward;
    std::vector<SceneEditCommand> reverse;
};

// Per-frame snapshot stack of inverse SceneEditCommands. One Authoring batch
// (everything queued during a single SceneUpdater::update call) becomes one
// UndoEntry. Replay flows through the same SceneUpdater::addEdit path with
// `fromHistory = true` so it isn't recorded again.
//
// Supported edit types in v1: SetTransform, SetVisibility. Other types are
// silently skipped (their inverses aren't defined yet).
class UndoStack {
public:
    // Capture inverses for `cmds` by reading current scene state, then push as
    // one entry. Skips Preview-purpose cmds, fromHistory cmds, and unsupported
    // types. Clears the redo stack only when an entry is actually pushed.
    auto recordBatch(std::span<const SceneEditCommand> cmds, const USDScene& scene) -> void;

    auto canUndo() const -> bool { return !m_undo.empty(); }
    auto canRedo() const -> bool { return !m_redo.empty(); }

    // Pop the top entry. Returns its `reverse` cmds (with `fromHistory = true`
    // and Authoring purpose) for the caller to enqueue via addEdit. Moves the
    // entry onto the redo stack. Empty vector if nothing to undo.
    auto undo() -> std::vector<SceneEditCommand>;
    auto redo() -> std::vector<SceneEditCommand>;

    auto clear() -> void;

    auto undoEntries() const -> std::span<const UndoEntry> { return m_undo; }
    auto redoEntries() const -> std::span<const UndoEntry> { return m_redo; }

    static constexpr size_t kMaxDepth = 100;

private:
    std::vector<UndoEntry> m_undo;
    std::vector<UndoEntry> m_redo;
};
