#pragma once

#include "camerawindow.h"
#include "capturewindow.h"
#include "counterswindow.h"
#include "cullingwindow.h"
#include "debugviewwindow.h"
#include "framedebuggerwindow.h"
#include "framegraphdebug.h"
#include "gpuscenewindow.h"
#include "introspectionflags.h"
#include "memorywindow.h"
#include "performancewindow.h"
#include "renderdebugwindow.h"
#include "statusbar.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class DebugDraw;
struct AABB;
struct Camera;
struct RenderWorld;

// What a click and a drag do in the viewport: select, or move the selection with a gizmo.
enum class ViewTool : uint8_t {
    Select,
    Translate,
    Rotate,
    Scale,
};

// A selection command from the Edit menu, for the view to carry out.
enum class ViewEditRequest : uint8_t {
    None,
    Deselect,
    SelectParent,
    FrameSelected,
};

// ngen-view's UI: the menu bar, the status bar, the overlays, the camera window and the introspection windows (frame graph,
// performance, render debug, culling, capture, frame debugger, GPU scene, counters, memory). It edits the settings the view renders
// with; the view reads them each frame. Prims are named by the path `primPath` gives for a draw's prim index.
class ViewUI {
public:
    // `hasSelection` enables the Edit menu's selection items.
    auto draw(const PrimPathOf& primPath,
              bool hasSelection,
              const RenderWorld& renderWorld,
              Camera& camera,
              std::optional<FrameGraphDebugSnapshot> freshFrameGraphSnap,
              std::optional<RenderDebugSnapshot> freshRenderDebugSnap) -> void;

    auto drawDebug(DebugDraw& debugDraw,
                   const RenderWorld& renderWorld,
                   glm::vec3 cameraPos,
                   glm::vec3 worldUp,
                   std::span<const uint8_t> visible,
                   const std::array<glm::vec3, 8>* frozenFrustum,
                   std::span<const ShadowCascade> cascades,
                   const AABB* selectedBounds) -> void;

    auto activeTool() const -> ViewTool { return (ViewTool) toolIndex; }
    auto setActiveTool(ViewTool tool) -> void { toolIndex = (int) tool; }
    // The Edit menu's request this frame, once.
    auto takeEditRequest() -> ViewEditRequest;

    auto wantsQuit() const -> bool { return requestQuit; }

    // Prims of the guide and proxy purposes are drawn when these are on; default and render prims always are.
    auto showGuides() const -> bool { return showGuidesFlag; }
    auto showProxies() const -> bool { return showProxiesFlag; }
    auto setShowGuides(bool on) -> void { showGuidesFlag = on; }
    auto setShowProxies(bool on) -> void { showProxiesFlag = on; }

    auto getShowGizmo() const -> bool { return showGizmoFlag; }
    auto getCullEnabled() const -> bool { return cullEnabledFlag; }
    auto setCullEnabled(bool on) -> void { cullEnabledFlag = on; }
    auto getCullFrozen() const -> bool { return cullFrozenFlag; }
    auto setCullFrozen(bool on) -> void { cullFrozenFlag = on; }
    auto getShowCulled() const -> bool { return showCulledFlag; }
    auto setShowCulled(bool on) -> void { showCulledFlag = on; }
    // Which view colours the culling AABB overlay: 0 camera, 1.. shadow cascades.
    auto cullOverlayView() const -> int { return cullOverlayViewIndex; }
    auto setCullOverlayView(int view) -> void { cullOverlayViewIndex = view; }
    auto getDepthPrepass() const -> bool { return depthPrepassFlag; }
    auto setDepthPrepass(bool on) -> void { depthPrepassFlag = on; }
    auto getSamplerSettings() const -> const SamplerSettings& { return samplerSettings; }
    auto samplerSettingsMutable() -> SamplerSettings& { return samplerSettings; }
    auto getShadowSettings() const -> const ShadowCascadeSettings& { return shadowSettings; }
    auto shadowSettingsMutable() -> ShadowCascadeSettings& { return shadowSettings; }
    auto setShadowCullStats(uint32_t cascades, const std::array<uint32_t, maxShadowCascades>& culled, const std::array<uint32_t, maxShadowCascades>& drawn) -> void {
        shadowCascadeCount = cascades;
        shadowCulledStats = culled;
        shadowDrawnStats = drawn;
    }
    auto getGBufferViewMode() const -> int { return gbufferViewMode; }
    auto setGBufferViewMode(int mode) -> void { gbufferViewMode = mode; }
    auto getDebugView() const -> int { return debugViewMode; }
    auto setDebugView(int view) -> void { debugViewMode = view; }
    auto getShowBufferOverlay() const -> bool { return showBufferOverlayFlag; }
    auto getShowShadowOverlay() const -> bool { return showShadowOverlayFlag; }
    auto getAntiAliasing() const -> bool { return antiAliasingFlag; }
    auto introspection() -> IntrospectionFlags& { return introspectionFlags; }
    // Whether any open window reads the render debug snapshot.
    auto wantsRenderDebug() const -> bool { return showRenderDebugWindow || introspectionFlags.memory; }
    // Whether any open window reads the frame graph debug snapshot.
    auto wantsFrameGraphDebug() const -> bool { return showFrameGraphWindow || introspectionFlags.capture; }
    // Frame Debugger: a frame capture was asked for (cleared by the call), and one arrived.
    auto takeFrameDebugRequest() -> bool { return std::exchange(frameDebuggerState.requestCapture, false); }
    auto onFrameDebugCapture(FrameDebugCapture capture) -> void;
    // Capture watches of the open introspection windows, and their results.
    auto captureWatches() const -> std::vector<CaptureWatch>;
    auto onCaptureResult(CaptureResult result) -> void;
    auto wantsCounters() const -> bool { return introspectionFlags.counters; }
    auto onCounters(GpuCounters counters) -> void { addCountersFrame(countersState, std::move(counters)); }
    // This frame's cull result, for the Culling window.
    auto setCullStats(uint32_t instances, uint32_t culled) -> void {
        cullStatInstances = instances;
        cullStatCulled = culled;
    }
    auto setCullTriangles(uint64_t drawn, uint64_t scene, const std::array<uint64_t, maxShadowCascades>& cascades) -> void {
        cullTrianglesDrawn = drawn;
        cullTrianglesScene = scene;
        cullTrianglesCascades = cascades;
    }

    // grid, origin, gizmo, aabbs, lightgizmos, buffer, shadow, aa, cascadefrusta; false if the name is unknown.
    auto setOverlay(std::string_view name, bool on) -> bool;
    // Opens or closes an introspection window by name (memory, capture, framedebugger, gpuscene, counters, shaders, culling).
    // Returns false for an unknown name.
    auto setIntrospectionWindow(std::string_view name, bool on) -> bool;
    // Screenshot requested from the UI (button or F12); main hands it to the renderer.
    auto takeScreenshotRequest() -> bool { return std::exchange(screenshotRequested, false); }
    auto requestScreenshot() -> void { screenshotRequested = true; }
    // What the status bar shows next frame; the application sets it each frame.
    auto setStatusBar(const StatusBarData& data) -> void { statusBarData = data; }
    auto cameraWindow() -> CameraWindowState& { return cameraState; }
    auto setBookmarkPath(std::string path) -> void {
        cameraState.bookmarkPath = std::move(path);
        cameraState.loaded = false;
    }
    // A texture inspector selection changed in the Render Debug window; main forwards it to the render thread.
    auto takeTextureInspectRequest() -> std::optional<TextureInspectRequest>;
    // "Dump level" in the inspector: the material and level, once.
    auto takeTextureDumpRequest() -> std::optional<TextureInspectRequest>;

private:
    bool showGridFlag = true;
    bool showOriginFlag = true;
    bool showGizmoFlag = true;
    bool showAABBsFlag = false;
    bool showSelectedAABBFlag = true;
    int toolIndex = 0; // ViewTool
    bool deselectRequested = false;
    bool selectParentRequested = false;
    bool frameSelectedRequested = false;
    bool showLightGizmosFlag = true;
    bool showGuidesFlag = false;
    bool showProxiesFlag = false;
    bool cullEnabledFlag = true;
    bool cullFrozenFlag = false;
    bool showCulledFlag = false;
    int cullOverlayViewIndex = 0;
    bool showCascadeFrustaFlag = false;
    bool depthPrepassFlag = false;
    SamplerSettings samplerSettings;
    ShadowCascadeSettings shadowSettings;
    uint32_t shadowCascadeCount = 0;
    std::array<uint32_t, maxShadowCascades> shadowCulledStats = {};
    std::array<uint32_t, maxShadowCascades> shadowDrawnStats = {};
    int gbufferViewMode = 0;
    int debugViewMode = 0; // DebugView
    bool showBufferOverlayFlag = false;
    bool showShadowOverlayFlag = false;
    bool antiAliasingFlag = true;
    bool showFrameGraphWindow = false;
    bool showPerformanceWindow = false;
    bool showStatusBar = true;
    StatusBarData statusBarData;
    bool showRenderDebugWindow = false;
    bool showCameraWindow = false;
    bool showCullingWindow = false;
    uint32_t cullStatInstances = 0;
    uint32_t cullStatCulled = 0;
    uint64_t cullTrianglesDrawn = 0;
    uint64_t cullTrianglesScene = 0;
    std::array<uint64_t, maxShadowCascades> cullTrianglesCascades = {};
    bool screenshotRequested = false;
    CameraWindowState cameraState;
    bool requestQuit = false;
    std::optional<FrameGraphDebugSnapshot> fgLastSnapshot;
    std::optional<uint32_t> fgSelectedPass;
    std::optional<uint32_t> fgSelectedResource;
    PerformanceWindowState performanceState;
    std::optional<RenderDebugSnapshot> renderDebugLast;
    uint32_t renderDebugSelectedPrim = 0; // a draw picked in the Render Debug window's list
    IntrospectionFlags introspectionFlags;
    MemoryWindowState memoryWindowState;
    CaptureWindowState captureWindowState;
    FrameDebuggerState frameDebuggerState;
    GpuSceneWindowState gpuSceneState;
    CountersWindowState countersState;
    DebugViewWindowState debugViewState;
    RenderDebugDrawState renderDebugDraws;
};
