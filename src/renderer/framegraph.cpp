#include "framegraph.h"
#include "framegraphdebug.h"
#include "observationmacros.h"
#include "profilegpu.h"
#include "resourcepool.h"
#include "rhicommandbuffer.h"

#include <format>
#include <queue>
#include <utility>

auto FrameGraphContext::logDraw(const FgDrawRecord& record) -> void {
    if (!graph->drawLogEnabled) {
        return;
    }
    auto entry = record;
    entry.pass = graph->executingPass;
    entry.drawIndex = graph->executingPassDraws++;
    graph->draws.push_back(entry);
}

auto FrameGraphContext::addIndirectStats(uint32_t draws, uint64_t primitives) -> void {
    graph->executingIndirectDraws += draws;
    graph->executingIndirectPrimitives += primitives;
}

auto FrameGraph::reset() -> void {
    captureRequests.clear();
    draws.clear();
    passes.clear();
    resources.clear();
    passOrder.clear();
    passData.clear();
}

auto FrameGraph::importTexture(const char* name, RhiTexture* texture, const FgTextureDesc& desc) -> FgTextureHandle {
    FgResource res;
    res.name = name;
    res.desc = desc;
    res.physical = texture;
    res.external = true;

    auto index = (uint32_t) resources.size();
    resources.push_back(res);
    return {index};
}

auto FrameGraph::importBuffer(const char* name, RhiBuffer* buffer, const FgBufferDesc& desc, FgAccessFlags initialAccess) -> FgBufferHandle {
    FgResource res;
    res.name = name;
    res.kind = FgResourceKind::Buffer;
    res.bufferDesc = desc;
    res.currentAccess = initialAccess;
    res.physicalBuffer = buffer;
    res.external = true;

    auto index = (uint32_t) resources.size();
    resources.push_back(res);
    return {index};
}

auto FrameGraphBuilder::createTexture(const char* name, const FgTextureDesc& desc) -> FgTextureHandle {
    FgResource res;
    res.name = name;
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

auto FrameGraphBuilder::read(FgBufferHandle handle, FgAccessFlags access) -> FgBufferHandle {
    auto& pass = graph->passes[passIndex];
    pass.reads.push_back({.resourceIndex = handle.index, .access = access});
    return handle;
}

auto FrameGraphBuilder::write(FgBufferHandle handle, FgAccessFlags access) -> FgBufferHandle {
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

auto FrameGraphContext::buffer(FgBufferHandle handle) -> RhiBuffer* {
    return graph->resources[handle.index].physicalBuffer;
}

// --- Helpers ---

static auto accessToLayout(FgAccessFlags access) -> RhiTextureState {
    if (access & FgAccessFlags::ColorAttachment) {
        return RhiTextureState::ColorAttachment;
    }
    if (access & FgAccessFlags::DepthAttachment) {
        return RhiTextureState::DepthStencilAttachment;
    }
    if (access & FgAccessFlags::ShaderRead) {
        return RhiTextureState::ShaderReadOnly;
    }
    if (access & FgAccessFlags::TransferSrc) {
        return RhiTextureState::TransferSrc;
    }
    if (access & FgAccessFlags::TransferDst) {
        return RhiTextureState::TransferDst;
    }
    if (access & FgAccessFlags::Present) {
        return RhiTextureState::PresentSrc;
    }
    if ((access & FgAccessFlags::StorageRead) || (access & FgAccessFlags::StorageWrite)) {
        return RhiTextureState::General;
    }
    return RhiTextureState::Undefined;
}

static auto accessToBufferState(FgAccessFlags access) -> RhiBufferState {
    if (access & FgAccessFlags::StorageWrite) {
        return RhiBufferState::StorageWrite;
    }
    if (access & FgAccessFlags::StorageRead) {
        return RhiBufferState::StorageRead;
    }
    if (access & FgAccessFlags::TransferSrc) {
        return RhiBufferState::TransferSrc;
    }
    if (access & FgAccessFlags::TransferDst) {
        return RhiBufferState::TransferDst;
    }
    if (access & FgAccessFlags::IndirectRead) {
        return RhiBufferState::IndirectRead;
    }
    return RhiBufferState::Undefined;
}

auto FrameGraph::textureStateFor(FgAccessFlags access) -> RhiTextureState {
    return accessToLayout(access);
}

auto FrameGraph::bufferStateFor(FgAccessFlags access) -> RhiBufferState {
    return accessToBufferState(access);
}

auto FrameGraph::runCaptures(RhiCommandBuffer* cmd, const char* passName, const std::vector<FgAccessFlags>& access) -> void {
    for (const auto& request : captureRequests) {
        if (request.pass != passName) {
            continue;
        }
        for (uint32_t resIdx = 0; resIdx < (uint32_t) resources.size(); resIdx++) {
            const auto& res = resources[resIdx];
            if (request.resource != res.name) {
                continue;
            }
            FgCaptureSource source = {
                .pass = passName,
                .resource = res.name,
                .kind = res.kind,
                .texture = res.physical,
                .buffer = res.physicalBuffer,
                .textureDesc = res.desc,
                .bufferDesc = res.bufferDesc,
                .textureState = accessToLayout(access[resIdx]),
                .bufferState = accessToBufferState(access[resIdx]),
            };
            bool alive = res.kind == FgResourceKind::Texture ? res.physical != nullptr : res.physicalBuffer != nullptr;
            if (alive) {
                request.record(cmd, source);
            }
            break;
        }
    }
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
            // Only one cull reason exists today: the pass has no downstream reads
            // and no side effects, so its outputs are dead. Keep `reason` as a
            // field so future reasons extend the vocabulary without breaking the
            // observation shape.
            OBS_EVENT("Render", "PassCulled", passes[i].name != nullptr ? passes[i].name : "(unnamed)").field("reason", "no_downstream_reads");
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
    // Always request TransferSrc/TransferDst on pooled transients so any pass can blit
    // to or from them (debug-preview capture, dummy AA, compositing, etc.) without the
    // pass author having to remember to request those usage bits.
    return {
        .width = desc.width,
        .height = desc.height,
        .format = desc.format,
        .usage = desc.usage | RhiTextureUsage::TransferSrc | RhiTextureUsage::TransferDst,
    };
}

auto FrameGraph::execute(RhiCommandBuffer* cmd) -> void {
    FrameGraphContext ctx(this, cmd);
    // Imported buffers start from the access carried over from the previous frame; everything
    // else starts undefined each frame.
    std::vector<FgAccessFlags> resourceAccess(resources.size(), FgAccessFlags::None);
    for (uint32_t resIdx = 0; resIdx < (uint32_t) resources.size(); resIdx++) {
        resourceAccess[resIdx] = resources[resIdx].currentAccess;
    }

    auto invokeCapture = [&](uint32_t resIdx) {
        if (!debugCaptureHook) {
            return;
        }
        const auto& res = resources[resIdx];
        if (res.kind != FgResourceKind::Texture || res.physical == nullptr) {
            return;
        }
        FgCapturedResource view = {
            .name = res.name,
            .physical = res.physical,
            .desc = res.desc,
            .currentAccess = resourceAccess[resIdx],
            .external = res.external,
        };
        debugCaptureHook(cmd, view);
        // Hook restores the source's previous layout, so resourceAccess stays in sync.
    };

    for (uint32_t orderIdx = 0; orderIdx < (uint32_t) passOrder.size(); orderIdx++) {
        auto passIdx = passOrder[orderIdx];
        if (passes[passIdx].culled) {
            continue;
        }

        // Allocate transient resources whose lifetime starts at this pass
        if (resourcePool != nullptr) {
            for (uint32_t resIdx = 0; resIdx < (uint32_t) resources.size(); resIdx++) {
                auto& res = resources[resIdx];
                if (res.kind == FgResourceKind::Texture && !res.external && res.physical == nullptr && res.firstUseOrder == orderIdx) {
                    auto desc = toRhiTextureDesc(res.desc);
                    auto name = std::format("fg.{}", res.name);
                    desc.debugName = name.c_str();
                    res.physical = resourcePool->acquireTexture(desc);
                }
            }
        }

        // Compute barriers for this pass
        std::vector<RhiTextureBarrierDesc> barriers;
        std::vector<RhiBufferBarrierDesc> bufferBarriers;
        auto checkTransition = [&](uint32_t resIdx, FgAccessFlags newAccess) {
            auto oldAccess = resourceAccess[resIdx];
            if (resources[resIdx].kind == FgResourceKind::Buffer) {
                // Buffers have no layout: read-after-read needs no barrier, anything
                // involving a write (or a different kind of read after one) does.
                auto oldState = accessToBufferState(oldAccess);
                auto newState = accessToBufferState(newAccess);
                bool readAfterRead = oldState == newState && oldState != RhiBufferState::StorageWrite && oldState != RhiBufferState::TransferDst;
                if (!readAfterRead) {
                    bufferBarriers.push_back({
                        .buffer = resources[resIdx].physicalBuffer,
                        .oldState = oldState,
                        .newState = newState,
                    });
                    passes[passIdx].barriers.push_back({.resourceIndex = resIdx, .buffer = true, .oldAccess = oldAccess, .newAccess = newAccess, .oldBufferState = oldState, .newBufferState = newState});
                }
                resourceAccess[resIdx] = newAccess;
                return;
            }
            if (std::to_underlying(oldAccess) != std::to_underlying(newAccess)) {
                auto oldState = accessToLayout(oldAccess);
                auto newState = accessToLayout(newAccess);
                if (oldState != newState) {
                    barriers.push_back({
                        .texture = resources[resIdx].physical,
                        .oldState = oldState,
                        .newState = newState,
                    });
                    passes[passIdx].barriers.push_back({.resourceIndex = resIdx, .oldAccess = oldAccess, .newAccess = newAccess, .oldTextureState = oldState, .newTextureState = newState});
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

        // Stats window opens before the graph's barriers, so a pass's barrier count includes
        // the transitions the graph issued for it, not only the ones it records itself.
        auto before = cmd->stats();
        auto logStart = cmd->commandLog().size();
        if (!barriers.empty() || !bufferBarriers.empty()) {
            cmd->pipelineBarrier(barriers, bufferBarriers);
        }

        // Execute pass. Single emission site covers all passes automatically;
        // new passes added later get narrated without per-file edits.
        const auto* passName = passes[passIdx].name != nullptr ? passes[passIdx].name : "(unnamed)";
        OBS_EVENT("Render", "PassExecuted", passName);
        cmd->beginLabel(passName);
        if (pipelineStatsEnabled) {
            cmd->beginPipelineStats(passName);
        }
        {
            // Every pass is a GPU zone and a CPU record zone; passes may nest their own inside.
            PROFILE_GPU_ZONE(cmd, passName);
            profile::ScopedZone recordZone(passes[passIdx].profileNameId);
            executingPass = passName;
            executingPassDraws = 0;
            executingIndirectDraws = 0;
            executingIndirectPrimitives = 0;
            passes[passIdx].execute(ctx);
            const auto& after = cmd->stats();
            passes[passIdx].stats = {
                .draws = after.draws - before.draws + executingIndirectDraws,
                .dispatches = after.dispatches - before.dispatches,
                .barriers = after.barriers - before.barriers,
                .pipelineBinds = after.pipelineBinds - before.pipelineBinds,
                .descriptorBinds = after.descriptorBinds - before.descriptorBinds,
                .bufferBinds = after.bufferBinds - before.bufferBinds,
                .indirectDraws = after.indirectDraws - before.indirectDraws,
                .copies = after.copies - before.copies,
                .primitives = after.primitives - before.primitives + executingIndirectPrimitives,
            };
        }
        if (pipelineStatsEnabled) {
            cmd->endPipelineStats();
        }
        cmd->endLabel();

        if (!captureRequests.empty()) {
            runCaptures(cmd, passName, resourceAccess);
        }
        if (commandLogEnabled) {
            auto log = cmd->commandLog();
            passes[passIdx].commands.assign(log.begin() + (std::ptrdiff_t) logStart, log.end());
        }

        // Release transient resources whose lifetime ends at this pass (after capturing)
        if (resourcePool != nullptr) {
            for (uint32_t resIdx = 0; resIdx < (uint32_t) resources.size(); resIdx++) {
                auto& res = resources[resIdx];
                if (res.kind == FgResourceKind::Texture && !res.external && res.physical != nullptr && res.lastUseOrder == orderIdx) {
                    invokeCapture(resIdx);
                    resourcePool->releaseTexture(toRhiTextureDesc(res.desc), res.physical);
                    res.physical = nullptr;
                }
            }
        }
    }

    if (!captureRequests.empty()) {
        runCaptures(cmd, "", resourceAccess);
    }

    // Carry imported buffers' final access into the next frame (finalAccess).
    for (uint32_t resIdx = 0; resIdx < (uint32_t) resources.size(); resIdx++) {
        auto& res = resources[resIdx];
        if (res.kind == FgResourceKind::Buffer && res.external) {
            res.currentAccess = resourceAccess[resIdx];
        }
    }

    // Capture external resources at end-of-execute (they're not released)
    if (debugCaptureHook) {
        for (uint32_t resIdx = 0; resIdx < (uint32_t) resources.size(); resIdx++) {
            if (resources[resIdx].external) {
                invokeCapture(resIdx);
            }
        }
    }
}

auto FrameGraph::passStats(std::string_view name) const -> const RhiCommandStats* {
    for (const auto& pass : passes) {
        if (!pass.culled && pass.name != nullptr && name == pass.name) {
            return &pass.stats;
        }
    }
    return nullptr;
}

// --- Debug snapshot ---

auto FrameGraph::buildDebugSnapshot() const -> FrameGraphDebugSnapshot {
    FrameGraphDebugSnapshot snap;
    auto passCount = (uint32_t) passes.size();
    auto resCount = (uint32_t) resources.size();

    std::vector<uint32_t> passExecIdx(passCount, UINT32_MAX);
    for (uint32_t i = 0; i < (uint32_t) passOrder.size(); i++) {
        passExecIdx[passOrder[i]] = i;
    }

    snap.passes.reserve(passCount);
    for (uint32_t p = 0; p < passCount; p++) {
        const auto& src = passes[p];
        FgPassDebug dbg;
        dbg.name = src.name != nullptr ? src.name : "(unnamed)";
        dbg.executionIndex = passExecIdx[p];
        dbg.culled = src.culled;
        dbg.hasSideEffects = src.hasSideEffects;
        dbg.stats = src.stats;
        dbg.reads.reserve(src.reads.size());
        for (const auto& r : src.reads) {
            dbg.reads.push_back({.resourceIndex = r.resourceIndex, .access = r.access});
        }
        for (const auto& b : src.barriers) {
            dbg.barriers.push_back({
                .resourceIndex = b.resourceIndex,
                .buffer = b.buffer,
                .oldAccess = b.oldAccess,
                .newAccess = b.newAccess,
                .oldState = b.buffer ? rhiStateName(b.oldBufferState) : rhiStateName(b.oldTextureState),
                .newState = b.buffer ? rhiStateName(b.newBufferState) : rhiStateName(b.newTextureState),
                .oldTextureState = b.oldTextureState,
                .newTextureState = b.newTextureState,
                .oldBufferState = b.oldBufferState,
                .newBufferState = b.newBufferState,
            });
        }
        dbg.commands = src.commands;
        dbg.writes.reserve(src.writes.size());
        for (const auto& w : src.writes) {
            dbg.writes.push_back({.resourceIndex = w.resourceIndex, .access = w.access});
        }
        snap.passes.push_back(std::move(dbg));
    }

    snap.executionOrder = passOrder;

    snap.resources.reserve(resCount);
    for (uint32_t r = 0; r < resCount; r++) {
        const auto& src = resources[r];
        FgResourceDebug dbg;
        dbg.index = r;
        dbg.external = src.external;
        if (src.kind == FgResourceKind::Buffer) {
            dbg.buffer = true;
            dbg.sizeBytes = src.bufferDesc.size;
            dbg.formatName = "buffer";
            dbg.usageName = toString(src.bufferDesc.usage);
        } else {
            dbg.width = src.desc.width;
            dbg.height = src.desc.height;
            dbg.formatName = toString(src.desc.format);
            dbg.usageName = toString(src.desc.usage);
        }
        dbg.firstUseOrder = src.firstUseOrder;
        dbg.lastUseOrder = src.lastUseOrder;

        for (uint32_t p = 0; p < passCount; p++) {
            for (const auto& w : passes[p].writes) {
                if (w.resourceIndex == r) {
                    if (dbg.producerPass == UINT32_MAX) {
                        dbg.producerPass = p;
                    }
                    break;
                }
            }
            for (const auto& rd : passes[p].reads) {
                if (rd.resourceIndex == r) {
                    dbg.consumerPasses.push_back(p);
                    break;
                }
            }
        }

        const char* nm = (src.name != nullptr && src.name[0] != '\0') ? src.name : "(unnamed)";
        dbg.name = nm;
        if (src.kind == FgResourceKind::Buffer) {
            dbg.label = std::format("{} ({}buffer, {} bytes)", nm, src.external ? "imported, " : "", dbg.sizeBytes);
        } else if (src.external) {
            dbg.label = std::format("{} (imported, {}x{} {})", nm, dbg.width, dbg.height, dbg.formatName);
        } else {
            dbg.label = std::format("{} ({}x{} {})", nm, dbg.width, dbg.height, dbg.formatName);
        }
        snap.resources.push_back(std::move(dbg));
    }

    return snap;
}
