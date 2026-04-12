#pragma once

#include "framegraph.h"
#include "rhitypes.h"

class RhiEditorUI;
struct ImGuiFrameSnapshot;

class EditorUIPass {
public:
    auto addPass(FrameGraph& fg,
                 FgTextureHandle colorHandle,
                 RhiExtent2D extent,
                 RhiEditorUI* editorUI,
                 ImGuiFrameSnapshot& imguiSnapshot) -> void;
};
