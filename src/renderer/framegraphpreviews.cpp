#include "framegraphpreviews.h"

#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "rhieditorui.h"

#include <algorithm>
#include <array>

auto FrameGraphPreviews::init(RhiDevice* d, RhiEditorUI* ui, RhiSampler* s) -> void {
    device = d;
    editorUI = ui;
    sampler = s;
}

auto FrameGraphPreviews::shutdown() -> void {
    for (auto& [name, e] : entries) {
        destroyEntry(e);
    }
    entries.clear();
}

auto FrameGraphPreviews::destroyEntry(Entry& e) -> void {
    if (e.imguiId != 0 && editorUI != nullptr) {
        editorUI->unregisterTexture(e.imguiId);
        e.imguiId = 0;
    }
    if (e.texture != nullptr && device != nullptr) {
        device->destroyTexture(e.texture);
        e.texture = nullptr;
    }
}

auto FrameGraphPreviews::isBlittableColorFormat(RhiFormat f) -> bool {
    switch (f) {
        case RhiFormat::R8G8B8A8_SRGB:
        case RhiFormat::R8G8B8A8_UNORM:
        case RhiFormat::B8G8R8A8_SRGB:
        case RhiFormat::B8G8R8A8_UNORM:
        case RhiFormat::R32G32B32A32_SFLOAT:
            return true;
        default:
            return false;
    }
}

auto FrameGraphPreviews::previewExtent(uint32_t srcW, uint32_t srcH) -> RhiExtent2D {
    constexpr uint32_t maxDim = 256;
    if (srcW == 0 || srcH == 0) {
        return {maxDim, maxDim};
    }
    auto scale = std::min(1.0f, (float) maxDim / (float) std::max(srcW, srcH));
    auto w = std::max(1u, (uint32_t) (srcW * scale));
    auto h = std::max(1u, (uint32_t) (srcH * scale));
    return {w, h};
}

auto FrameGraphPreviews::entryFor(const FgCapturedResource& view) -> Entry* {
    if (view.name == nullptr || view.name[0] == '\0') {
        return nullptr;
    }
    if (!isBlittableColorFormat(view.desc.format)) {
        return nullptr;
    }
    auto ext = previewExtent(view.desc.width, view.desc.height);

    auto [it, inserted] = entries.try_emplace(view.name);
    auto& e = it->second;

    bool needRecreate = inserted || e.texture == nullptr || e.width != ext.width || e.height != ext.height || e.format != view.desc.format;

    if (needRecreate) {
        destroyEntry(e);
        RhiTextureDesc desc = {
            .width = ext.width,
            .height = ext.height,
            .format = view.desc.format,
            .usage = RhiTextureUsage::Sampled | RhiTextureUsage::TransferDst,
        };
        e.texture = device->createTexture(desc);
        e.width = ext.width;
        e.height = ext.height;
        e.format = view.desc.format;
        e.everCaptured = false;
        if (e.texture != nullptr) {
            e.imguiId = editorUI->registerTexture(e.texture, sampler);
        }
    }
    return e.texture != nullptr ? &e : nullptr;
}

static auto accessToLayoutPreview(FgAccessFlags a) -> RhiImageLayout {
    if (a & FgAccessFlags::ColorAttachment) {
        return RhiImageLayout::ColorAttachment;
    }
    if (a & FgAccessFlags::DepthAttachment) {
        return RhiImageLayout::DepthStencilAttachment;
    }
    if (a & FgAccessFlags::ShaderRead) {
        return RhiImageLayout::ShaderReadOnly;
    }
    if (a & FgAccessFlags::TransferSrc) {
        return RhiImageLayout::TransferSrc;
    }
    if (a & FgAccessFlags::TransferDst) {
        return RhiImageLayout::TransferDst;
    }
    if (a & FgAccessFlags::Present) {
        return RhiImageLayout::PresentSrc;
    }
    return RhiImageLayout::Undefined;
}

auto FrameGraphPreviews::capture(RhiCommandBuffer* cmd, const FgCapturedResource& view) -> void {
    auto* e = entryFor(view);
    if (e == nullptr) {
        return;
    }

    auto srcLayout = accessToLayoutPreview(view.currentAccess);
    auto dstStartLayout = e->everCaptured ? RhiImageLayout::ShaderReadOnly : RhiImageLayout::Undefined;

    std::array<RhiBarrierDesc, 2> preBarriers = {{
        {.texture = view.physical, .oldLayout = srcLayout, .newLayout = RhiImageLayout::TransferSrc},
        {.texture = e->texture, .oldLayout = dstStartLayout, .newLayout = RhiImageLayout::TransferDst},
    }};
    cmd->pipelineBarrier(preBarriers);

    cmd->blitTexture(view.physical, e->texture, {view.desc.width, view.desc.height}, {e->width, e->height});

    std::array<RhiBarrierDesc, 2> postBarriers = {{
        {.texture = view.physical, .oldLayout = RhiImageLayout::TransferSrc, .newLayout = srcLayout},
        {.texture = e->texture, .oldLayout = RhiImageLayout::TransferDst, .newLayout = RhiImageLayout::ShaderReadOnly},
    }};
    cmd->pipelineBarrier(postBarriers);

    e->everCaptured = true;
}

auto FrameGraphPreviews::annotate(FrameGraphDebugSnapshot& snap) const -> void {
    for (auto& r : snap.resources) {
        if (r.name.empty()) {
            continue;
        }
        auto it = entries.find(r.name);
        if (it == entries.end() || !it->second.everCaptured) {
            continue;
        }
        r.previewTextureId = it->second.imguiId;
        r.previewWidth = it->second.width;
        r.previewHeight = it->second.height;
    }
}
