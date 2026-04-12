#include "editoruipass.h"
#include "rhicommandbuffer.h"
#include "rhieditorui.h"

auto EditorUIPass::addPass(FrameGraph& fg,
                           FgTextureHandle colorHandle,
                           RhiExtent2D extent,
                           RhiEditorUI* editorUI,
                           ImGuiFrameSnapshot& imguiSnapshot) -> void {
    struct EditorUIPassData {
        FgTextureHandle color;
    };

    fg.addPass<EditorUIPassData>(
        "EditorUIPass",
        [&](FrameGraphBuilder& builder, EditorUIPassData& data) {
            data.color = builder.write(colorHandle, FgAccessFlags::ColorAttachment);
            builder.setSideEffects(true);
        },
        [editorUI, extent, &imguiSnapshot](FrameGraphContext& ctx, const EditorUIPassData& data) {
            auto* cmd = ctx.cmd();

            RhiRenderingAttachmentInfo colorAtt = {
                .texture = ctx.texture(data.color),
                .layout = RhiImageLayout::ColorAttachment,
                .clear = false,
            };
            RhiRenderingInfo renderInfo = {
                .extent = extent,
                .colorAttachments = {&colorAtt, 1},
            };
            cmd->beginRendering(renderInfo);
            editorUI->renderDrawData(cmd, imguiSnapshot);
            cmd->endRendering();
        });
}
