#pragma once

#include "rotategizmo.h"
#include "scalegizmo.h"
#include "tools.h"
#include "translategizmo.h"
#include "viewhooks.h"
#include "viewscene.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

// The tools' part of ngen-view's frame (src/tools/README.md): the LOD and Streaming tools' rows and inspectors kept while shown, the debug
// camera taking over the cameras it is set to drive, the transform gizmos and the axis gizmo, the viewport's overlays and the tools' layers,
// and the requests the tools' windows leave in ToolsState carried out.
class ToolsHooks : public ViewHooks {
public:
    explicit ToolsHooks(ToolsState& tools) : tools(tools) {}

    auto state() -> ToolsState& { return tools; }

    auto afterScene(ViewFrame& frame) -> void override;
    auto cameras(ViewFrame& frame) -> void override;
    auto event(ViewFrame& frame, const SDL_Event& event) -> bool override;
    auto afterCamera(ViewFrame& frame) -> void override;
    auto debugDraw(ViewFrame& frame) -> void override;
    auto drawUi(ViewFrame& frame) -> void override;
    auto fillSnapshot(ViewFrame& frame, RenderSnapshot& snapshot) -> void override;

private:
    // The LOD window's requests carried out, and what it shows refreshed: the GPU's counts, the tuning, the selected root, the inspector.
    auto syncLodTool(ViewFrame& frame) -> void;
    // The Streaming window's requests carried out, and what it shows refreshed: the chart, the cameras, the flight, the inspector.
    auto syncStreamingTool(ViewFrame& frame) -> void;
    // Where the gizmo of the selection sits: its pivot, or its bounds' centre when the pivot lies outside what it draws.
    auto gizmoAnchor(const ViewFrame& frame, const ViewScene::PrimTransform& transform) const -> glm::vec3;

    ToolsState& tools;
    // Whether each tool is shown (its window, a viewport layer, or the streaming view): its rows are kept only then, once per scene snapshot.
    bool lodShown = false;
    bool streamingShown = false;
    bool streamingView = false;
    uint64_t lodRowsSnapshot = UINT64_MAX;
    uint64_t streamingRowsSnapshot = UINT64_MAX;
    // What each inspector was last built for: the root, the scene snapshot, the time; again four times a second while shown.
    std::string lodInspected;
    uint64_t lodInspectedSnapshot = UINT64_MAX;
    std::chrono::steady_clock::time_point lodInspectedAt;
    std::string streamingInspected;
    uint64_t streamingInspectedSnapshot = UINT64_MAX;
    std::chrono::steady_clock::time_point streamingInspectedAt;
    bool debugCameraWasOn = false;

    TranslateGizmo translateGizmo;
    RotateGizmo rotateGizmo;
    ScaleGizmo scaleGizmo;
    // The selection's bounds (its box) and transform (its gizmo): again only when the selection or the scene snapshot changes, as bounds()
    // walks every draw and transform() the placements.
    std::string selectedBoundsPath;
    const SceneSnapshot* selectedBoundsSnapshot = nullptr;
    AABB selectedBounds;
    std::string selectedTransformPath;
    const SceneSnapshot* selectedTransformSnapshot = nullptr;
    std::optional<ViewScene::PrimTransform> selectedTransform;
};
