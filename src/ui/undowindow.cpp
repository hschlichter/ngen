#include "undowindow.h"

#include "sceneupdater.h"
#include "undostack.h"
#include "usdscene.h"

#include <imgui.h>

static auto labelFor(const SceneEditCommand& cmd) -> const char* {
    switch (cmd.type) {
        case SceneEditCommand::Type::SetTransform: return "Move";
        case SceneEditCommand::Type::SetVisibility: return cmd.boolValue ? "Show" : "Hide";
        case SceneEditCommand::Type::MuteLayer: return cmd.boolValue ? "Mute" : "Unmute";
        case SceneEditCommand::Type::AddSubLayer: return "AddLayer";
        case SceneEditCommand::Type::ClearSession: return "ClearSession";
    }
    return "?";
}

static auto pathFor(const SceneEditCommand& cmd, const USDScene& scene) -> const char* {
    if (!cmd.prim) {
        return "";
    }
    const auto* rec = scene.getPrimRecord(cmd.prim);
    return rec ? rec->path.c_str() : "<unknown>";
}

static auto drawEntryRow(const UndoEntry& entry, const USDScene& scene, bool isCursor, bool greyed) -> void {
    if (greyed) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    const auto& first = entry.forward.front();
    if (entry.forward.size() == 1) {
        ImGui::Text("%s %-8s %s", isCursor ? "\xe2\x96\xb6" : " ", labelFor(first), pathFor(first, scene));
    } else {
        ImGui::Text("%s %-8s %s  (%zu)", isCursor ? "\xe2\x96\xb6" : " ", labelFor(first), pathFor(first, scene), entry.forward.size());
    }
    if (greyed) {
        ImGui::PopStyleColor();
    }
}

void drawUndoWindow(bool& show, SceneUpdater& sceneUpdater, const USDScene& scene) {
    if (!show) {
        return;
    }

    ImGui::Begin("History", &show);

    auto& stack = sceneUpdater.undoStack();

    auto runReplay = [&](std::vector<SceneEditCommand>&& cmds) {
        for (auto& c : cmds) {
            sceneUpdater.addEdit(std::move(c));
        }
    };

    ImGui::BeginDisabled(!stack.canUndo());
    if (ImGui::Button("Undo")) {
        runReplay(stack.undo());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!stack.canRedo());
    if (ImGui::Button("Redo")) {
        runReplay(stack.redo());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    // Re-fetch spans AFTER the buttons — undo()/redo() mutate the underlying
    // vectors and invalidate any spans grabbed beforehand.
    auto undoEntries = stack.undoEntries();
    auto redoEntries = stack.redoEntries();
    ImGui::TextDisabled("Depth: %zu / %zu", undoEntries.size() + redoEntries.size(), UndoStack::kMaxDepth);

    ImGui::Separator();

    // Undo entries: oldest at top, most recent (next-to-undo) marked with cursor.
    for (size_t i = 0; i < undoEntries.size(); i++) {
        if (undoEntries[i].forward.empty()) {
            continue;
        }
        bool isCursor = (i == undoEntries.size() - 1);
        drawEntryRow(undoEntries[i], scene, isCursor, false);
    }

    ImGui::Separator();

    // Redo entries: greyed; iterate from most-recently-undone (top of redo
    // stack = first thing redo would replay) downward.
    for (size_t i = redoEntries.size(); i-- > 0;) {
        if (redoEntries[i].forward.empty()) {
            continue;
        }
        drawEntryRow(redoEntries[i], scene, false, true);
    }

    ImGui::End();
}
