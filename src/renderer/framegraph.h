#pragma once

#include "framegraphbuilder.h"
#include "framegraphcontext.h"
#include "framegraphresource.h"
#include "passnode.h"
#include "profile.h"
#include "rhitypes.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class RhiCommandBuffer;
class ResourcePool;
struct FrameGraphDebugSnapshot;

struct FgCapturedResource {
    const char* name = "";
    RhiTexture* physical = nullptr;
    FgTextureDesc desc;
    FgAccessFlags currentAccess = FgAccessFlags::None;
    bool external = false;
};

using FgDebugCaptureFn = std::function<void(RhiCommandBuffer*, const FgCapturedResource&)>;

// A resource as it is at a capture point: right after a pass, or after the last pass. The
// state is what the graph tracks at that moment; a capture must leave the resource in it.
struct FgCaptureSource {
    const char* pass = "";     // the pass that just ran; "" after the last pass
    const char* resource = ""; // resource name
    FgResourceKind kind = FgResourceKind::Texture;
    RhiTexture* texture = nullptr;
    RhiBuffer* buffer = nullptr;
    FgTextureDesc textureDesc;
    FgBufferDesc bufferDesc;
    RhiTextureState textureState = RhiTextureState::Undefined;
    RhiBufferState bufferState = RhiBufferState::Undefined;
};

// Runs `record` right after pass `pass` executes (before transients are released), or after
// the last pass when `pass` is empty, if resource `resource` is alive then. `record` gets the
// frame's command buffer and records whatever copies it needs.
struct FgCaptureRequest {
    std::string pass;
    std::string resource;
    std::function<void(RhiCommandBuffer*, const FgCaptureSource&)> record;
};

class FrameGraph {
    friend class FrameGraphBuilder;
    friend class FrameGraphContext;

public:
    auto setResourcePool(ResourcePool* pool) -> void { resourcePool = pool; }
    auto reset() -> void;
    auto importTexture(const char* name, RhiTexture* texture, const FgTextureDesc& desc) -> FgTextureHandle;
    // A buffer that outlives the frame. initialAccess is the access it was left in by the
    // previous frame (finalAccess of that frame), so the first barrier this frame syncs
    // against the earlier reads or writes; None for a new buffer or unknown contents.
    auto importBuffer(const char* name, RhiBuffer* buffer, const FgBufferDesc& desc, FgAccessFlags initialAccess = FgAccessFlags::None) -> FgBufferHandle;
    // Access the imported buffer is left in after execute(); initialAccess if no pass used it.
    auto finalAccess(FgBufferHandle handle) const -> FgAccessFlags { return resources[handle.index].currentAccess; }

    template <typename DataT>
    auto addPass(const char* name, std::function<void(FrameGraphBuilder&, DataT&)> setup, std::function<void(FrameGraphContext&, const DataT&)> exec)
        -> const DataT&;

    auto compile() -> void;
    auto execute(RhiCommandBuffer* cmd) -> void;

    auto passCount() const -> size_t { return passes.size(); }
    auto culledCount() const -> size_t {
        return std::count_if(passes.begin(), passes.end(), [](const PassNode& p) { return p.culled; });
    }

    auto buildDebugSnapshot() const -> FrameGraphDebugSnapshot;
    // Command stats of the named pass from the last execute; nullptr if absent or culled.
    auto passStats(std::string_view name) const -> const RhiCommandStats*;

    // Draw log for the render debugger: enabled per frame, records cleared by reset().
    auto setDrawLogEnabled(bool enabled) -> void { drawLogEnabled = enabled; }
    // Keep each pass's slice of the command buffer's command log (the log itself must be on).
    auto setCommandLogEnabled(bool enabled) -> void { commandLogEnabled = enabled; }
    // While on, each pass's execute is one pipeline statistics query named after the pass.
    auto setPipelineStatsEnabled(bool enabled) -> void { pipelineStatsEnabled = enabled; }
    auto drawLog() const -> const std::vector<FgDrawRecord>& { return draws; }

    auto setDebugCaptureHook(FgDebugCaptureFn fn) -> void { debugCaptureHook = std::move(fn); }
    // Capture points for this frame; cleared by reset().
    auto setCaptureRequests(std::vector<FgCaptureRequest> requests) -> void { captureRequests = std::move(requests); }
    static auto textureStateFor(FgAccessFlags access) -> RhiTextureState;
    static auto bufferStateFor(FgAccessFlags access) -> RhiBufferState;

private:
    std::vector<PassNode> passes;
    std::vector<FgResource> resources;
    std::vector<uint32_t> passOrder;

    struct PassDataEntry {
        void* data = nullptr;
        void (*deleter)(void*) = nullptr;
        ~PassDataEntry() {
            if (deleter != nullptr) {
                deleter(data);
            }
        }
        PassDataEntry() = default;
        PassDataEntry(const PassDataEntry&) = delete;
        PassDataEntry& operator=(const PassDataEntry&) = delete;
        PassDataEntry(PassDataEntry&& o) noexcept : data(o.data), deleter(o.deleter) {
            o.data = nullptr;
            o.deleter = nullptr;
        }
        PassDataEntry& operator=(PassDataEntry&& o) noexcept {
            if (this != &o) {
                if (deleter != nullptr) {
                    deleter(data);
                }
                data = o.data;
                deleter = o.deleter;
                o.data = nullptr;
                o.deleter = nullptr;
            }
            return *this;
        }
    };
    std::vector<PassDataEntry> passData;

    ResourcePool* resourcePool = nullptr;
    FgDebugCaptureFn debugCaptureHook;
    std::vector<FgCaptureRequest> captureRequests;
    auto runCaptures(RhiCommandBuffer* cmd, const char* passName, const std::vector<FgAccessFlags>& access) -> void;

    bool drawLogEnabled = false;
    bool commandLogEnabled = false;
    bool pipelineStatsEnabled = false;
    std::vector<FgDrawRecord> draws;
    const char* executingPass = "";
    uint32_t executingPassDraws = 0;
    uint32_t executingIndirectDraws = 0; // FrameGraphContext::addIndirectStats
    uint64_t executingIndirectPrimitives = 0;
};

template <typename DataT>
auto FrameGraph::addPass(const char* name, std::function<void(FrameGraphBuilder&, DataT&)> setup, std::function<void(FrameGraphContext&, const DataT&)> exec)
    -> const DataT& {
    auto passIndex = (uint32_t) passes.size();

    auto* data = new DataT{};
    PassDataEntry entry;
    entry.data = data;
    entry.deleter = [](void* p) {
        delete static_cast<DataT*>(p);
    };
    passData.push_back(std::move(entry));

    passes.push_back({});
    passes.back().name = name;
    passes.back().profileNameId = profile::registerName(name);

    FrameGraphBuilder builder(this, passIndex);
    setup(builder, *data);

    passes[passIndex].execute = [data, exec](FrameGraphContext& ctx) {
        exec(ctx, *data);
    };

    return *data;
}
