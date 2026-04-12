#include "framegraph.h"
#include "resourcepool.h"
#include "rhicommandbuffer.h"

#include <print>
#include <queue>
#include <utility>

auto FrameGraph::reset() -> void {
    passes.clear();
    resources.clear();
    passOrder.clear();
    passData.clear();
}

auto FrameGraph::importTexture(RhiTexture* texture, const FgTextureDesc& desc) -> FgTextureHandle {
    FgResource res;
    res.desc = desc;
    res.physical = texture;
    res.external = true;

    auto index = (uint32_t) resources.size();
    resources.push_back(res);
    return {index};
}

auto FrameGraphBuilder::createTexture(const FgTextureDesc& desc) -> FgTextureHandle {
    FgResource res;
    res.desc = desc;
    res.external = false;

    auto index = (uint32_t) graph->resources.size();
    graph->resources.push_back(res);
    return {index};
}

auto FrameGraphBuilder::read(FgTextureHandle handle, FgAccessFlags access) -> FgTextureHandle {
    auto& pass = graph->passes[passIndex];
    pass.reads.push_back({.resourceIndex = handle.index, .access = access});
    return handle;
}

auto FrameGraphBuilder::write(FgTextureHandle handle, FgAccessFlags access) -> FgTextureHandle {
    auto& pass = graph->passes[passIndex];
    pass.writes.push_back({.resourceIndex = handle.index, .access = access});
    return handle;
}

auto FrameGraphBuilder::setSideEffects(bool value) -> void {
    graph->passes[passIndex].hasSideEffects = value;
}

auto FrameGraphContext::texture(FgTextureHandle handle) -> RhiTexture* {
    return graph->resources[handle.index].physical;
}

// --- Helpers ---

static auto accessToLayout(FgAccessFlags access) -> RhiImageLayout {
    if (access & FgAccessFlags::ColorAttachment) {
        return RhiImageLayout::ColorAttachment;
    }
    if (access & FgAccessFlags::DepthAttachment) {
        return RhiImageLayout::DepthStencilAttachment;
    }
    if (access & FgAccessFlags::ShaderRead) {
        return RhiImageLayout::ShaderReadOnly;
    }
    if (access & FgAccessFlags::TransferSrc) {
        return RhiImageLayout::TransferSrc;
    }
    if (access & FgAccessFlags::TransferDst) {
        return RhiImageLayout::TransferDst;
    }
    if (access & FgAccessFlags::Present) {
        return RhiImageLayout::PresentSrc;
    }
    return RhiImageLayout::Undefined;
}

// --- Compilation: topo sort + culling ---

auto FrameGraph::compile() -> void {
    auto passCount = (uint32_t) passes.size();

    // 1. Build adjacency from resource usage
    std::vector<std::vector<uint32_t>> adj(passCount);
    std::vector<uint32_t> inDegree(passCount, 0);

    for (uint32_t resIdx = 0; resIdx < (uint32_t) resources.size(); resIdx++) {
        // Collect all writers and readers for this resource
        std::vector<uint32_t> writers;
        for (uint32_t p = 0; p < passCount; p++) {
            for (const auto& w : passes[p].writes) {
                if (w.resourceIndex == resIdx) {
                    writers.push_back(p);
                    break;
                }
            }
        }

        // Chain consecutive writers: write-after-write ordering
        for (size_t i = 1; i < writers.size(); i++) {
            adj[writers[i - 1]].push_back(writers[i]);
            inDegree[writers[i]]++;
        }

        // For each reader, connect to the latest writer declared before it
        for (uint32_t p = 0; p < passCount; p++) {
            for (const auto& r : passes[p].reads) {
                if (r.resourceIndex == resIdx) {
                    // Find the latest writer with index < p
                    uint32_t bestWriter = UINT32_MAX;
                    for (auto w : writers) {
                        if (w < p) {
                            bestWriter = w;
                        }
                    }
                    if (bestWriter != UINT32_MAX) {
                        adj[bestWriter].push_back(p);
                        inDegree[p]++;
                    }
                    break;
                }
            }
        }
    }

    // 2. Topological sort (Kahn's algorithm)
    passOrder.clear();
    std::queue<uint32_t> q;
    for (uint32_t i = 0; i < passCount; i++) {
        if (inDegree[i] == 0) {
            q.push(i);
        }
    }
    while (!q.empty()) {
        auto cur = q.front();
        q.pop();
        passOrder.push_back(cur);
        for (auto next : adj[cur]) {
            if (--inDegree[next] == 0) {
                q.push(next);
            }
        }
    }

    // 3. Pass culling — backward walk from side-effect passes
    std::vector<bool> alive(passCount, false);
    std::vector<std::vector<uint32_t>> reverseAdj(passCount);
    for (uint32_t i = 0; i < passCount; i++) {
        for (auto next : adj[i]) {
            reverseAdj[next].push_back(i);
        }
    }

    std::queue<uint32_t> aliveQ;
    for (uint32_t i = 0; i < passCount; i++) {
        if (passes[i].hasSideEffects) {
            alive[i] = true;
            aliveQ.push(i);
        }
    }
    while (!aliveQ.empty()) {
        auto cur = aliveQ.front();
        aliveQ.pop();
        for (auto dep : reverseAdj[cur]) {
            if (!alive[dep]) {
                alive[dep] = true;
                aliveQ.push(dep);
            }
        }
    }

    for (uint32_t i = 0; i < passCount; i++) {
        passes[i].culled = !alive[i];
        if (passes[i].culled) {
            std::println("Frame graph: culled pass '{}'", passes[i].name);
        }
    }

    // 4. Compute transient resource lifetimes
    for (auto& res : resources) {
        res.firstUseOrder = UINT32_MAX;
        res.lastUseOrder = 0;
    }

    for (uint32_t orderIdx = 0; orderIdx < (uint32_t) passOrder.size(); orderIdx++) {
        auto passIdx = passOrder[orderIdx];
        if (passes[passIdx].culled) {
            continue;
        }

        auto updateLifetime = [&](uint32_t resIdx) {
            auto& res = resources[resIdx];
            if (res.external) {
                return;
            }
            if (orderIdx < res.firstUseOrder) {
                res.firstUseOrder = orderIdx;
            }
            if (orderIdx > res.lastUseOrder) {
                res.lastUseOrder = orderIdx;
            }
        };

        for (const auto& r : passes[passIdx].reads) {
            updateLifetime(r.resourceIndex);
        }
        for (const auto& w : passes[passIdx].writes) {
            updateLifetime(w.resourceIndex);
        }
    }
}

// --- Execution: allocate transients, compute barriers, run passes, release ---

static auto toRhiTextureDesc(const FgTextureDesc& desc) -> RhiTextureDesc {
    return {
        .width = desc.width,
        .height = desc.height,
        .format = desc.format,
        .usage = desc.usage,
    };
}

auto FrameGraph::execute(RhiCommandBuffer* cmd) -> void {
    FrameGraphContext ctx(this, cmd);
    std::vector<FgAccessFlags> resourceAccess(resources.size(), FgAccessFlags::None);

    for (uint32_t orderIdx = 0; orderIdx < (uint32_t) passOrder.size(); orderIdx++) {
        auto passIdx = passOrder[orderIdx];
        if (passes[passIdx].culled) {
            continue;
        }

        // Allocate transient resources whose lifetime starts at this pass
        if (resourcePool != nullptr) {
            for (uint32_t resIdx = 0; resIdx < (uint32_t) resources.size(); resIdx++) {
                auto& res = resources[resIdx];
                if (!res.external && res.physical == nullptr && res.firstUseOrder == orderIdx) {
                    res.physical = resourcePool->acquireTexture(toRhiTextureDesc(res.desc));
                }
            }
        }

        // Compute barriers for this pass
        std::vector<RhiBarrierDesc> barriers;
        auto checkTransition = [&](uint32_t resIdx, FgAccessFlags newAccess) {
            auto oldAccess = resourceAccess[resIdx];
            if (std::to_underlying(oldAccess) != std::to_underlying(newAccess)) {
                auto oldLayout = accessToLayout(oldAccess);
                auto newLayout = accessToLayout(newAccess);
                if (oldLayout != newLayout) {
                    barriers.push_back({
                        .texture = resources[resIdx].physical,
                        .oldLayout = oldLayout,
                        .newLayout = newLayout,
                    });
                }
                resourceAccess[resIdx] = newAccess;
            }
        };

        for (const auto& r : passes[passIdx].reads) {
            checkTransition(r.resourceIndex, r.access);
        }
        for (const auto& w : passes[passIdx].writes) {
            checkTransition(w.resourceIndex, w.access);
        }

        if (!barriers.empty()) {
            cmd->pipelineBarrier(barriers);
        }

        // Execute pass
        passes[passIdx].execute(ctx);

        // Release transient resources whose lifetime ends at this pass
        if (resourcePool != nullptr) {
            for (uint32_t resIdx = 0; resIdx < (uint32_t) resources.size(); resIdx++) {
                auto& res = resources[resIdx];
                if (!res.external && res.physical != nullptr && res.lastUseOrder == orderIdx) {
                    resourcePool->releaseTexture(toRhiTextureDesc(res.desc), res.physical);
                    res.physical = nullptr;
                }
            }
        }
    }
}
