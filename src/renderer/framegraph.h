#pragma once

#include "framegraphbuilder.h"
#include "framegraphcontext.h"
#include "framegraphresource.h"
#include "passnode.h"
#include "rhitypes.h"

#include <functional>
#include <memory>
#include <vector>

class RhiCommandBuffer;
class ResourcePool;

class FrameGraph {
    friend class FrameGraphBuilder;
    friend class FrameGraphContext;

public:
    auto setResourcePool(ResourcePool* pool) -> void { resourcePool = pool; }
    auto reset() -> void;
    auto importTexture(RhiTexture* texture, const FgTextureDesc& desc) -> FgTextureHandle;

    template <typename DataT>
    auto addPass(const char* name, std::function<void(FrameGraphBuilder&, DataT&)> setup, std::function<void(FrameGraphContext&, const DataT&)> exec)
        -> const DataT&;

    auto compile() -> void;
    auto execute(RhiCommandBuffer* cmd) -> void;

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

    struct BarrierBatch {
        std::vector<RhiBarrierDesc> barriers;
    };
    std::vector<BarrierBatch> passBarriers;

    ResourcePool* resourcePool = nullptr;
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

    FrameGraphBuilder builder(this, passIndex);
    setup(builder, *data);

    passes[passIndex].execute = [data, exec](FrameGraphContext& ctx) {
        exec(ctx, *data);
    };

    return *data;
}
