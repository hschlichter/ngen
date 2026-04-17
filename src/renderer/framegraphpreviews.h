#pragma once

#include "framegraph.h"
#include "framegraphdebug.h"
#include "rhitypes.h"

#include <cstdint>
#include <string>
#include <unordered_map>

class RhiDevice;
class RhiEditorUI;
class RhiCommandBuffer;

class FrameGraphPreviews {
public:
    auto init(RhiDevice* device, RhiEditorUI* editorUI, RhiSampler* sampler) -> void;
    auto shutdown() -> void;

    // Hook body: transitions source to TransferSrc, blits into preview, restores source, leaves preview in ShaderReadOnly.
    auto capture(RhiCommandBuffer* cmd, const FgCapturedResource& view) -> void;

    // Fill in previewTextureId / previewWidth / previewHeight for each named resource in the snapshot.
    auto annotate(FrameGraphDebugSnapshot& snap) const -> void;

private:
    struct Entry {
        RhiTexture* texture = nullptr;
        uint64_t imguiId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        RhiFormat format = RhiFormat::Undefined;
        bool everCaptured = false;
    };

    auto destroyEntry(Entry& e) -> void;
    auto entryFor(const FgCapturedResource& view) -> Entry*;
    static auto previewExtent(uint32_t srcW, uint32_t srcH) -> RhiExtent2D;
    static auto isBlittableColorFormat(RhiFormat f) -> bool;

    RhiDevice* device = nullptr;
    RhiEditorUI* editorUI = nullptr;
    RhiSampler* sampler = nullptr;
    std::unordered_map<std::string, Entry> entries;
};
