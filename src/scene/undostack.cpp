#include "undostack.h"

#include "usdscene.h"

#include <optional>

static auto inverseOf(const SceneEditCommand& cmd, const USDScene& scene) -> std::optional<SceneEditCommand> {
    SceneEditCommand inv = cmd;
    inv.fromHistory = true;
    inv.purpose = SceneEditRequestContext::Purpose::Authoring;

    switch (cmd.type) {
        case SceneEditCommand::Type::SetTransform: {
            if (cmd.inverseTransform) {
                inv.transform = *cmd.inverseTransform;
                return inv;
            }
            const auto* xf = scene.getTransform(cmd.prim);
            if (!xf) {
                return std::nullopt;
            }
            inv.transform = xf->local;
            return inv;
        }
        case SceneEditCommand::Type::SetVisibility: {
            if (cmd.inverseBoolValue) {
                inv.boolValue = *cmd.inverseBoolValue;
                return inv;
            }
            const auto* rec = scene.getPrimRecord(cmd.prim);
            if (!rec) {
                return std::nullopt;
            }
            inv.boolValue = rec->visible;
            return inv;
        }
        default:
            return std::nullopt; // unsupported in v1
    }
}

auto UndoStack::recordBatch(std::span<const SceneEditCommand> cmds, const USDScene& scene) -> void {
    UndoEntry entry;
    for (const auto& cmd : cmds) {
        if (cmd.fromHistory || cmd.purpose != SceneEditRequestContext::Purpose::Authoring) {
            continue;
        }
        auto inv = inverseOf(cmd, scene);
        if (!inv) {
            continue;
        }
        entry.forward.push_back(cmd);
        entry.forward.back().fromHistory = true; // when replayed via redo
        entry.reverse.push_back(*inv);
    }
    if (entry.forward.empty()) {
        return;
    }
    m_undo.push_back(std::move(entry));
    if (m_undo.size() > kMaxDepth) {
        m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
}

auto UndoStack::undo() -> std::vector<SceneEditCommand> {
    if (m_undo.empty()) {
        return {};
    }
    auto entry = std::move(m_undo.back());
    m_undo.pop_back();
    auto cmds = entry.reverse;
    m_redo.push_back(std::move(entry));
    return cmds;
}

auto UndoStack::redo() -> std::vector<SceneEditCommand> {
    if (m_redo.empty()) {
        return {};
    }
    auto entry = std::move(m_redo.back());
    m_redo.pop_back();
    auto cmds = entry.forward;
    m_undo.push_back(std::move(entry));
    return cmds;
}

auto UndoStack::clear() -> void {
    m_undo.clear();
    m_redo.clear();
}
