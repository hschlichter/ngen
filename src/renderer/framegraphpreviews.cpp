#include "framegraphpreviews.h"

#include "deletionqueue.h"
#include "imguibackend.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <algorithm>
#include <array>

auto FrameGraphPreviews::init(RhiDevice* d, ImGuiBackend* ui, RhiSampler* s, DeletionQueue* dq) -> void {
    deletionQueue = dq;
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

// Immediate destruction; only valid when no frame can still reference the entry
// (shutdown after waitIdle).
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

// Deferred variant for replacing a live preview mid-frame: the previous frame's
// command buffer may still sample it.
auto FrameGraphPreviews::releaseEntry(Entry& e) -> void {
    if (deletionQueue == nullptr) {
        destroyEntry(e);
        return;
    }
    if (e.imguiId != 0 && editorUI != nullptr) {
        deletionQueue->defer(currentFrame, [ui = editorUI, id = e.imguiId] { ui->unregisterTexture(id); });
        e.imguiId = 0;
    }
    if (e.texture != nullptr) {
        deletionQueue->deferTexture(currentFrame, e.texture);
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
        case RhiFormat::R16G16B16A16_SFLOAT:
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
    auto maxSrc = std::max(srcW, srcH);
    if (maxSrc <= maxDim) {
        return {srcW, srcH};
    }
    // Integer math so repeat calls for the same source are bit-identical.
    auto w = std::max(1u, (srcW * maxDim + maxSrc / 2) / maxSrc);
    auto h = std::max(1u, (srcH * maxDim + maxSrc / 2) / maxSrc);
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
        releaseEntry(e);
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

static auto accessToLayoutPreview(FgAccessFlags a) -> RhiTextureState {
    if (a & FgAccessFlags::ColorAttachment) {
        return RhiTextureState::ColorAttachment;
    }
    if (a & FgAccessFlags::DepthAttachment) {
        return RhiTextureState::DepthStencilAttachment;
    }
    if (a & FgAccessFlags::ShaderRead) {
        return RhiTextureState::ShaderReadOnly;
    }
    if (a & FgAccessFlags::TransferSrc) {
        return RhiTextureState::TransferSrc;
    }
    if (a & FgAccessFlags::TransferDst) {
        return RhiTextureState::TransferDst;
    }
    if (a & FgAccessFlags::Present) {
        return RhiTextureState::PresentSrc;
    }
    if ((a & FgAccessFlags::StorageRead) || (a & FgAccessFlags::StorageWrite)) {
        return RhiTextureState::General;
    }
    return RhiTextureState::Undefined;
}

auto FrameGraphPreviews::capture(RhiCommandBuffer* cmd, const FgCapturedResource& view) -> void {
    auto* e = entryFor(view);
    if (e == nullptr) {
        return;
    }

    auto srcLayout = accessToLayoutPreview(view.currentAccess);
    auto dstStartLayout = e->everCaptured ? RhiTextureState::ShaderReadOnly : RhiTextureState::Undefined;

    std::array<RhiTextureBarrierDesc, 2> preBarriers = {{
        {.texture = view.physical, .oldState = srcLayout, .newState = RhiTextureState::TransferSrc},
        {.texture = e->texture, .oldState = dstStartLayout, .newState = RhiTextureState::TransferDst},
    }};
    cmd->pipelineBarrier(preBarriers);

    cmd->blitTexture(view.physical, e->texture, {view.desc.width, view.desc.height}, {e->width, e->height});

    std::array<RhiTextureBarrierDesc, 2> postBarriers = {{
        {.texture = view.physical, .oldState = RhiTextureState::TransferSrc, .newState = srcLayout},
        {.texture = e->texture, .oldState = RhiTextureState::TransferDst, .newState = RhiTextureState::ShaderReadOnly},
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
