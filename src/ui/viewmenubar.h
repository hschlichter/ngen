#pragma once

#include "introspectionflags.h"

// ngen-view's menu bar: File (Quit), View (purposes, overlays, buffer and debug views) and Windows (the introspection windows). The
// flags are ViewUI's; the menus edit them in place.
struct ViewMenuBarState {
    bool& showGrid;
    bool& showOrigin;
    bool& showGizmo;
    bool& showAABBs;
    bool& showLightGizmos;
    bool& showGuides;
    bool& showProxies;
    bool& depthPrepass;
    int& gbufferView;
    int& debugView; // DebugView
    bool& showBufferOverlay;
    bool& showShadowOverlay;
    bool& antiAliasing;
    bool& showFrameGraph;
    bool& showPerformance;
    bool& showStatusBar;
    bool& showRenderDebug;
    bool& showCamera;
    bool& showCulling;
    IntrospectionFlags& introspection;
    bool& requestQuit;
};

auto drawViewMenuBar(ViewMenuBarState& state) -> void;
