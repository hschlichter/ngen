#pragma once

#include "framegraph.h"
#include "rhitypes.h"

class ImGuiBackend;
struct ImGuiFrameSnapshot;

class EditorUIPass {
public:
    auto addPass(FrameGraph& fg, FgTextureHandle colorHandle, RhiExtent2D extent, ImGuiBackend* editorUI, ImGuiFrameSnapshot& imguiSnapshot) -> void;
};
