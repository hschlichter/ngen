#pragma once

#include "framegraph.h"
#include "framegraphdebug.h"
#include "rhitypes.h"

#include <cstdint>
#include <string>
#include <unordered_map>

class RhiDevice;
class PreviewTextures;
class RhiCommandBuffer;
class DeletionQueue;

class FrameGraphPreviews {
public:
    auto init(RhiDevice* device, PreviewTextures* previews, RhiSampler* sampler, DeletionQueue* deletionQueue) -> void;
    auto shutdown() -> void;

    // Frame number currently being recorded; tags deferred destruction of replaced previews.
    auto setFrame(uint64_t frame) -> void { currentFrame = frame; }

    // Hook body: transitions source to TransferSrc, blits into preview, restores source, leaves preview in ShaderReadOnly.
    auto capture(RhiCommandBuffer* cmd, const FgCapturedResource& view) -> void;

    // Fill in previewTextureId / previewWidth / previewHeight for each named resource in the snapshot.
    auto annotate(FrameGraphDebugSnapshot& snap) const -> void;

private:
    struct Entry {
        RhiTexture* texture = nullptr;
        uint64_t previewId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        RhiFormat format = RhiFormat::Undefined;
        bool everCaptured = false;
    };

    auto destroyEntry(Entry& e) -> void;
    auto releaseEntry(Entry& e) -> void;
    auto entryFor(const FgCapturedResource& view) -> Entry*;
    static auto previewExtent(uint32_t srcW, uint32_t srcH) -> RhiExtent2D;
    static auto isBlittableColorFormat(RhiFormat f) -> bool;

    RhiDevice* device = nullptr;
    PreviewTextures* previews = nullptr; // none: no previews
    RhiSampler* sampler = nullptr;
    DeletionQueue* deletionQueue = nullptr;
    uint64_t currentFrame = 0;
    std::unordered_map<std::string, Entry> entries;
};
