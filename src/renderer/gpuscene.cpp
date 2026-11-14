#include "gpuscene.h"

#include "renderconfig.h"

#include "deletionqueue.h"
#include "framegraph.h"
#include "framegraphdebug.h"
#include "gpuuploader.h"
#include "instanceuploadpass.h"
#include "jobsystem.h"
#include "mesh.h"
#include "profile.h"
#include "profilegpu.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "trace.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>

namespace {

constexpr uint64_t minimumBufferBytes = 64 * 1024;
constexpr uint32_t deadMesh = 0xFFFFFFFFu; // a dead slot's mesh: past the mesh table, so the culling passes never draw it

// The bytes of a mip level of an RGBA8 texture.
auto levelBytes(uint32_t width, uint32_t height, uint32_t level) -> uint64_t {
    uint64_t w = std::max(1u, width >> level);
    uint64_t h = std::max(1u, height >> level);
    return w * h * 4;
}

auto levelOffset(uint32_t width, uint32_t height, uint32_t level) -> uint64_t {
    uint64_t offset = 0;
    for (uint32_t l = 0; l < level; l++) {
        offset += levelBytes(width, height, l);
    }
    return offset;
}

} // namespace

auto PoolAllocator::allocate(uint64_t count) -> std::optional<uint64_t> {
    if (count == 0) {
        return 0;
    }
    for (auto it = freeRanges.begin(); it != freeRanges.end(); ++it) {
        if (it->second < count) {
            continue;
        }
        auto first = it->first;
        auto rest = it->second - count;
        freeRanges.erase(it);
        if (rest > 0) {
            freeRanges[first + count] = rest;
        }
        inUse += count;
        return first;
    }
    return std::nullopt;
}

auto PoolAllocator::free(uint64_t first, uint64_t count) -> void {
    if (count == 0) {
        return;
    }
    inUse -= count;
    auto next = freeRanges.lower_bound(first);
    // Merge with the free range after, then with the one before.
    if (next != freeRanges.end() && first + count == next->first) {
        count += next->second;
        next = freeRanges.erase(next);
    }
    if (next != freeRanges.begin()) {
        auto before = std::prev(next);
        if (before->first + before->second == first) {
            before->second += count;
            return;
        }
    }
    freeRanges[first] = count;
}

auto PoolAllocator::grow(uint64_t capacity) -> void {
    if (capacity <= total) {
        return;
    }
    auto added = capacity - total;
    auto first = total;
    total = capacity;
    inUse += added; // free() takes it back
    free(first, added);
}

auto GpuScene::init(RhiDevice* rhiDevice, uint32_t frameSlots, DeletionQueue* queue, RhiTexture* fallback, RhiTexture* pending) -> void {
    device = rhiDevice;
    deletionQueue = queue;
    fallbackTexture = fallback;
    pendingTexture = pending;
    slots = {fallback, pending};
    uploadRing.assign(frameSlots, {});
    instanceStaging.assign(frameSlots, {});
    // The mesh shaders pull vertices and meshlets from storage buffers.
    vertices = {.usage = RhiBufferUsage::Storage, .readState = RhiBufferState::StorageRead, .name = "gpuscene.pool.vertices"};
    positions = {.usage = RhiBufferUsage::Storage, .readState = RhiBufferState::StorageRead, .name = "gpuscene.pool.positions"};
    meshlets = {.usage = RhiBufferUsage::Storage, .readState = RhiBufferState::StorageRead, .name = "gpuscene.pool.meshlets"};
    meshletVertices = {.usage = RhiBufferUsage::Storage, .readState = RhiBufferState::StorageRead, .name = "gpuscene.pool.meshletvertices"};
    meshletTriangles = {.usage = RhiBufferUsage::Storage, .readState = RhiBufferState::StorageRead, .name = "gpuscene.pool.meshlettriangles"};
    meshTable = {.usage = RhiBufferUsage::Storage, .readState = RhiBufferState::StorageRead, .name = "gpuscene.meshtable", .zeroed = true};
    materialTable = {.usage = RhiBufferUsage::Storage, .readState = RhiBufferState::StorageRead, .name = "gpuscene.materials", .zeroed = true};
    // Never null, so every descriptor set has something to name.
    ensureTable(meshTable, meshTableGrowth, sizeof(GpuMeshEntry));
    ensureTable(materialTable, materialTableGrowth, sizeof(GpuMaterial));
    ensureInstanceCapacity(0, 0, 0);
}

auto GpuScene::destroy() -> void {
    for (auto* growth : {&instanceGrowth, &vertexGrowth, &meshletGrowth, &meshletVertexGrowth, &meshletTriangleGrowth, &meshTableGrowth, &materialTableGrowth}) {
        growth->discard(device);
    }
    for (auto* target : poolBuffers()) {
        // The pool's buffers exist once a mesh has been added.
        if (target->buffer != nullptr) {
            device->destroyBuffer(target->buffer);
        }
        if (target->moveFrom != nullptr) {
            device->destroyBuffer(target->moveFrom);
        }
        *target = {};
    }
    meshes.clear();
    meshData.clear();
    for (auto& [index, info] : gpuTextures) {
        // Made when its first level is recorded.
        if (info.texture != nullptr) {
            device->destroyTexture(info.texture);
        }
    }
    gpuTextures.clear();
    textureData.clear();
    slots.clear();
    queue.clear();

    device->destroyBuffer(instances);
    instances = nullptr;
    instanceStagingGrowth.discard(device);
    uploadRingGrowth.discard(device);
    for (auto& slot : instanceStaging) {
        if (slot.buffer != nullptr) {
            device->destroyBuffer(slot.buffer);
        }
        slot = {};
    }
    instanceStagingBytes = 0;
    for (auto& slot : uploadRing) {
        if (slot.buffer != nullptr) {
            device->destroyBuffer(slot.buffer);
        }
        slot = {};
    }
    uploadRingBytes = 0;
}

auto GpuScene::ensureTable(GrowableBuffer& target, BufferGrowth& growth, uint64_t bytes) -> void {
    auto grownCapacity = [](uint64_t needed) -> uint64_t {
        return std::max(needed, minimumBufferBytes);
    };
    auto describe = [&target](uint64_t capacity) -> std::vector<GrowthBufferDesc> {
        return {{.desc = {
                     .size = capacity,
                     // TransferSrc for the next growth's copy and for the capture service.
                     .usage = target.usage | RhiBufferUsage::TransferDst | RhiBufferUsage::TransferSrc,
                     .memory = RhiMemoryUsage::GpuOnly,
                     .debugName = target.name,
                 }}};
    };
    auto install = [&](uint64_t capacity, std::vector<GrownBuffer>& made) {
        swapIn(target, made[0].buffer, capacity);
    };
    if (target.buffer == nullptr) {
        auto capacity = grownCapacity(bytes);
        auto made = BufferGrowth::makeNow(device, describe(capacity));
        install(capacity, made);
        return;
    }
    growth.update(device, bytes, target.capacity, grownCapacity, describe, install);
}

auto GpuScene::swapIn(GrowableBuffer& target, RhiBuffer* buffer, uint64_t bytes) -> void {
    if (buffer == nullptr) {
        return;
    }
    if (target.buffer != nullptr && target.fresh) {
        // Nothing has written it since it was made (grown twice before a record, or made at init and never used): nothing to copy. A
        // copy still pending comes from the buffer before it.
        deletionQueue->deferBuffer(currentFrame, target.buffer);
    } else if (target.buffer != nullptr) {
        target.moveFrom = target.buffer;
        target.moveBytes = target.capacity;
    }
    target.buffer = buffer;
    target.capacity = bytes;
    target.fresh = true;
    descriptorsDirty = true;
}

auto GpuScene::poolLimitBytes() const -> uint64_t {
    // Whole MiBs, so the allocation the buffer needs (rounded up to its alignment) stays under the device's limit too.
    constexpr uint64_t granule = 1024ull * 1024;
    auto limit = device->limits().maxBufferBytes;
    if (limit == 0) {
        limit = UINT64_MAX;
    }
    if (geometryLimit > 0) {
        limit = std::min(limit, geometryLimit);
    }
    return limit / granule * granule;
}

auto GpuScene::poolBuffers() -> std::array<GrowableBuffer*, 7> {
    return {&vertices, &positions, &meshlets, &meshletVertices, &meshletTriangles, &meshTable, &materialTable};
}

auto GpuScene::poolReady() const -> bool {
    return vertices.buffer != nullptr && positions.buffer != nullptr && meshlets.buffer != nullptr && meshletVertices.buffer != nullptr && meshletTriangles.buffer != nullptr;
}

auto GpuScene::rangesOf(Pool pool) -> PoolAllocator& {
    switch (pool) {
        case Pool::Vertices:
            return vertexRanges;
        case Pool::Meshlets:
            return meshletRanges;
        case Pool::MeshletVertices:
            return meshletVertexRanges;
        case Pool::MeshletTriangles:
            return meshletTriangleRanges;
    }
    return vertexRanges;
}

auto GpuScene::growthOf(Pool pool) -> BufferGrowth& {
    switch (pool) {
        case Pool::Vertices:
            return vertexGrowth;
        case Pool::Meshlets:
            return meshletGrowth;
        case Pool::MeshletVertices:
            return meshletVertexGrowth;
        case Pool::MeshletTriangles:
            return meshletTriangleGrowth;
    }
    return vertexGrowth;
}

auto GpuScene::updatePool(Pool pool, uint64_t needed) -> bool {
    // The pool's buffers and their element sizes. The vertex stream reaches the limit first (a Vertex is larger than a position): it sets the
    // vertex capacity of both.
    std::vector<std::pair<GrowableBuffer*, uint64_t>> targets;
    switch (pool) {
        case Pool::Vertices:
            targets = {{&vertices, sizeof(Vertex)}, {&positions, sizeof(std::array<float, 3>)}};
            break;
        case Pool::Meshlets:
            targets = {{&meshlets, sizeof(GpuMeshlet)}};
            break;
        case Pool::MeshletVertices:
            targets = {{&meshletVertices, sizeof(uint32_t)}};
            break;
        case Pool::MeshletTriangles:
            targets = {{&meshletTriangles, sizeof(uint32_t)}};
            break;
    }
    auto maxElements = poolLimitBytes() / targets[0].second;
    auto& ranges = rangesOf(pool);
    auto minElements = minimumBufferBytes / targets[0].second;
    auto grownCapacity = [&](uint64_t wanted) -> uint64_t {
        return std::min(std::max(wanted, minElements), maxElements);
    };
    auto describe = [&](uint64_t elements) -> std::vector<GrowthBufferDesc> {
        std::vector<GrowthBufferDesc> descs;
        for (const auto& [target, elementBytes] : targets) {
            descs.push_back({.desc = {
                                 .size = std::max(elements * elementBytes, minimumBufferBytes),
                                 // TransferSrc for the next growth's copy and for the capture service.
                                 .usage = target->usage | RhiBufferUsage::TransferDst | RhiBufferUsage::TransferSrc,
                                 .memory = RhiMemoryUsage::GpuOnly,
                                 .debugName = target->name,
                             }});
        }
        return descs;
    };
    auto install = [&](uint64_t elements, std::vector<GrownBuffer>& made) {
        for (size_t i = 0; i < targets.size(); i++) {
            if (made[i].buffer == nullptr) {
                // The device couldn't make it: the pool keeps its size, the rest of what was made goes.
                for (auto& other : made) {
                    deletionQueue->deferBuffer(currentFrame, other.buffer);
                }
                return;
            }
        }
        for (size_t i = 0; i < targets.size(); i++) {
            swapIn(*targets[i].first, made[i].buffer, std::max(elements * targets[i].second, minimumBufferBytes));
        }
        ranges.grow(elements);
    };
    if (targets[0].first->buffer == nullptr) {
        auto elements = grownCapacity(needed);
        if (elements == 0) {
            return false;
        }
        auto made = BufferGrowth::makeNow(device, describe(elements));
        install(elements, made);
        return true;
    }
    return growthOf(pool).update(device, needed, ranges.capacity(), grownCapacity, describe, install);
}

auto GpuScene::allocateIn(Pool pool, uint64_t count) -> std::optional<uint64_t> {
    auto& ranges = rangesOf(pool);
    auto first = ranges.allocate(count);
    if (!first) {
        // No room (the ranges may be fragmented, so past the capacity, not the use): a growth that is ready is taken, else one is started
        // and the mesh waits for it.
        auto wanted = ranges.capacity() + count;
        updatePool(pool, wanted);
        first = ranges.allocate(count);
        if (!first) {
            auto& shortBy = poolShort[(size_t) pool];
            shortBy = std::max(shortBy, wanted);
        }
    } else {
        // Taken a growth that is ready, or started one once the use passes render.growth.ahead of the capacity; a mesh that didn't fit
        // still asks for its room.
        updatePool(pool, std::max(ranges.used(), poolShort[(size_t) pool]));
    }
    return first;
}

auto GpuScene::addMesh(MeshHandle handle, std::shared_ptr<const MeshDesc> mesh) -> void {
    if (!handle || mesh == nullptr || mesh->vertices.empty() || mesh->meshlets.empty()) {
        return;
    }
    // The table entry exists either way: a mesh that waits has an empty one, and its draws draw nothing.
    ensureTable(meshTable, meshTableGrowth, ((uint64_t) handle.index + 1) * sizeof(GpuMeshEntry));
    if (placeMesh(handle, mesh)) {
        return;
    }
    bool growing = std::ranges::any_of(std::array{Pool::Vertices, Pool::Meshlets, Pool::MeshletVertices, Pool::MeshletTriangles}, [&](Pool pool) {
        return growthOf(pool).isWaiting();
    });
    if (!poolFullReported && !growing) {
        poolFullReported = true;
        auto limit = poolLimitBytes();
        TRACE_WARNING("Render", "GeometryPoolFull", "gpuscene.pool")
            .text(std::format("the geometry pool is full ({} MiB per buffer): meshes that don't fit wait, undrawn, until removed meshes make room", limit / (1024 * 1024)))
            .field("limitBytes", (int64_t) limit)
            .field("vertexUsed", (int64_t) vertexRanges.used())
            .field("meshletUsed", (int64_t) meshletRanges.used());
    }
    waitingMeshes.push_back({.handle = handle, .mesh = std::move(mesh)});
}

auto GpuScene::placeMesh(MeshHandle handle, const std::shared_ptr<const MeshDesc>& mesh) -> bool {
    std::array<std::pair<Pool, uint64_t>, 4> wanted = {{
        {Pool::Vertices, mesh->vertices.size()},
        {Pool::Meshlets, mesh->meshlets.size()},
        {Pool::MeshletVertices, mesh->meshletVertices.size()},
        {Pool::MeshletTriangles, mesh->meshletTriangles.size()},
    }};
    std::array<uint64_t, 4> first = {};
    for (size_t p = 0; p < wanted.size(); p++) {
        auto range = allocateIn(wanted[p].first, wanted[p].second);
        if (!range) {
            // Doesn't fit: give back what it took.
            for (size_t taken = 0; taken < p; taken++) {
                rangesOf(wanted[taken].first).free(first[taken], wanted[taken].second);
            }
            return false;
        }
        first[p] = *range;
    }
    uint64_t vertexCount = mesh->vertices.size();
    meshes[handle.index] = {
        .vertexOffset = (int32_t) first[0],
        .vertexCount = (uint32_t) vertexCount,
        .meshletOffset = (uint32_t) first[1],
        .meshletCount = (uint32_t) mesh->meshlets.size(),
        .meshletVertexOffset = (uint32_t) first[2],
        .meshletVertexCount = (uint32_t) mesh->meshletVertices.size(),
        .meshletTriangleOffset = (uint32_t) first[3],
        .meshletTriangleCount = (uint32_t) mesh->meshletTriangles.size(),
        .vertexBytes = vertexCount * (sizeof(Vertex) + sizeof(std::array<float, 3>)),
        .meshletBytes = (mesh->meshlets.size() * sizeof(GpuMeshlet)) + ((mesh->meshletVertices.size() + mesh->meshletTriangles.size()) * sizeof(uint32_t)),
    };
    meshData[handle.index] = mesh;
    // The data before the entry: a mesh is drawn once its entry says so.
    queue.push_back({.kind = Upload::Kind::MeshVertices, .handle = handle.index, .bytes = vertexCount * sizeof(Vertex)});
    queue.push_back({.kind = Upload::Kind::MeshPositions, .handle = handle.index, .bytes = vertexCount * sizeof(std::array<float, 3>)});
    queue.push_back({.kind = Upload::Kind::MeshMeshlets, .handle = handle.index, .bytes = mesh->meshlets.size() * sizeof(GpuMeshlet)});
    queue.push_back({.kind = Upload::Kind::MeshMeshletVertices, .handle = handle.index, .bytes = mesh->meshletVertices.size() * sizeof(uint32_t)});
    queue.push_back({.kind = Upload::Kind::MeshMeshletTriangles, .handle = handle.index, .bytes = mesh->meshletTriangles.size() * sizeof(uint32_t)});
    queue.push_back({.kind = Upload::Kind::MeshEntry, .handle = handle.index, .bytes = sizeof(GpuMeshEntry)});
    return true;
}

auto GpuScene::placeWaitingMeshes() -> void {
    if (waitingMeshes.empty()) {
        return;
    }
    std::vector<WaitingMesh> stillWaiting;
    for (auto& waiting : waitingMeshes) {
        if (!placeMesh(waiting.handle, waiting.mesh)) {
            stillWaiting.push_back(std::move(waiting));
        }
    }
    bool emptied = stillWaiting.empty();
    waitingMeshes = std::move(stillWaiting);
    if (emptied && std::exchange(poolFullReported, false)) {
        TRACE_EVENT("Render", "GeometryPoolFits", "gpuscene.pool").text("every mesh that waited for room in the geometry pool is in it");
    }
}

auto GpuScene::uploadStats() const -> UploadStats {
    return {
        .queued = (uint32_t) queue.size(),
        .queuedBytes = pendingBytes(),
        .budget = uploadBudget,
        .lastUploads = lastUploads,
        .lastBytes = lastUploadBytes,
        .lastFrame = lastUploadFrame,
        .totalBytes = totalUploadBytes,
        .texturesWaitingSlot = (uint32_t) texturesCopied.size(),
        .instanceRecords = lastInstanceUploads,
        .instanceRegions = lastInstanceRegions,
    };
}

auto GpuScene::growthStatus() const -> std::vector<GrowthStatus> {
    return {
        instanceGrowth.statusOf(instanceCapacity, lastInstanceCount),
        vertexGrowth.statusOf(vertexRanges.capacity(), std::max(vertexRanges.used(), poolShort[(size_t) Pool::Vertices])),
        meshletGrowth.statusOf(meshletRanges.capacity(), std::max(meshletRanges.used(), poolShort[(size_t) Pool::Meshlets])),
        meshletVertexGrowth.statusOf(meshletVertexRanges.capacity(), std::max(meshletVertexRanges.used(), poolShort[(size_t) Pool::MeshletVertices])),
        meshletTriangleGrowth.statusOf(meshletTriangleRanges.capacity(), std::max(meshletTriangleRanges.used(), poolShort[(size_t) Pool::MeshletTriangles])),
        meshTableGrowth.statusOf(meshTable.capacity, meshTable.capacity),
        materialTableGrowth.statusOf(materialTable.capacity, materialTable.capacity),
        instanceStagingGrowth.statusOf(instanceStagingBytes, lastInstanceStagingNeed),
        uploadRingGrowth.statusOf(uploadRingBytes, std::max(uploadBudget, queue.empty() ? 0 : queue.front().bytes)),
    };
}

auto GpuScene::geometryPoolStats() const -> GeometryPoolStats {
    auto limit = poolLimitBytes();
    GeometryPoolStats stats = {
        .limitBytes = limit,
        .vertices = {.limit = limit / sizeof(Vertex), .capacity = vertexRanges.capacity(), .used = vertexRanges.used()},
        .meshlets = {.limit = limit / sizeof(GpuMeshlet), .capacity = meshletRanges.capacity(), .used = meshletRanges.used()},
        .meshletVertices = {.limit = limit / sizeof(uint32_t), .capacity = meshletVertexRanges.capacity(), .used = meshletVertexRanges.used()},
        .meshletTriangles = {.limit = limit / sizeof(uint32_t), .capacity = meshletTriangleRanges.capacity(), .used = meshletTriangleRanges.used()},
        .waitingMeshes = (uint32_t) waitingMeshes.size(),
    };
    for (const auto& waiting : waitingMeshes) {
        const auto& mesh = *waiting.mesh;
        stats.waitingBytes += (mesh.vertices.size() * (sizeof(Vertex) + sizeof(std::array<float, 3>))) + (mesh.meshlets.size() * sizeof(GpuMeshlet)) +
                              ((mesh.meshletVertices.size() + mesh.meshletTriangles.size()) * sizeof(uint32_t));
    }
    return stats;
}

auto GpuScene::removeMesh(MeshHandle handle) -> void {
    if (std::erase_if(waitingMeshes, [&](const WaitingMesh& waiting) { return waiting.handle.index == handle.index; }) > 0) {
        return;
    }
    auto it = meshes.find(handle.index);
    if (it == meshes.end()) {
        return;
    }
    std::erase_if(queue, [&](const Upload& upload) {
        return upload.handle == handle.index && upload.kind != Upload::Kind::MaterialEntry && upload.kind != Upload::Kind::TextureLevel;
    });
    // Its ranges can be written at once: the next copy's barrier orders it after every earlier frame's reads (one queue).
    vertexRanges.free((uint64_t) it->second.vertexOffset, it->second.vertexCount);
    meshletRanges.free(it->second.meshletOffset, it->second.meshletCount);
    meshletVertexRanges.free(it->second.meshletVertexOffset, it->second.meshletVertexCount);
    meshletTriangleRanges.free(it->second.meshletTriangleOffset, it->second.meshletTriangleCount);
    poolRoomFreed = true;
    meshes.erase(it);
    meshData.erase(handle.index);
    queue.push_back({.kind = Upload::Kind::MeshEntry, .handle = handle.index, .bytes = sizeof(GpuMeshEntry)});
}

auto GpuScene::addTexture(TextureHandle handle, std::shared_ptr<const TextureDesc> texture, uint64_t frame) -> void {
    if (!handle || texture == nullptr || texture->width == 0 || texture->height == 0) {
        return;
    }
    currentFrame = frame;
    // The GPU texture is made when its first level is recorded, so making textures (slow for large ones) follows the upload budget.
    auto mipLevels = std::max(1u, texture->mipLevels);
    gpuTextures[handle.index] = {
        .width = texture->width,
        .height = texture->height,
        .mipLevels = mipLevels,
        .bytes = texture->levels.size(),
        .format = texture->format == TextureFormat::Rgb9e5 ? RhiFormat::E5B9G9R9_UFLOAT : RhiFormat::R8G8B8A8_SRGB,
    };
    textureData[handle.index] = std::move(texture);
    levelsLeft[handle.index] = mipLevels;
    for (uint32_t level = 0; level < mipLevels; level++) {
        queue.push_back({.kind = Upload::Kind::TextureLevel, .handle = handle.index, .level = level, .bytes = levelBytes(gpuTextures[handle.index].width, gpuTextures[handle.index].height, level)});
    }
}

auto GpuScene::removeTexture(TextureHandle handle, uint64_t frame) -> void {
    auto it = gpuTextures.find(handle.index);
    if (it == gpuTextures.end()) {
        return;
    }
    std::erase_if(queue, [&](const Upload& upload) {
        return upload.kind == Upload::Kind::TextureLevel && upload.handle == handle.index;
    });
    std::erase(texturesCopied, handle.index);
    if (it->second.slot > pendingSlot) {
        slots[it->second.slot] = fallbackTexture;
        freeTextureSlots.insert(it->second.slot);
        descriptorsDirty = true;
    }
    // Frames in flight may still sample it; one never recorded was never made.
    if (it->second.texture != nullptr) {
        deletionQueue->deferTexture(frame, it->second.texture);
    }
    gpuTextures.erase(it);
    textureData.erase(handle.index);
    levelsLeft.erase(handle.index);
    materialsUsing.erase(handle.index);
}

auto GpuScene::setMaterial(MaterialHandle handle, const MaterialDesc& material) -> void {
    if (!handle) {
        return;
    }
    auto [it, added] = entryOf.try_emplace(handle.index, 0);
    if (added) {
        if (!freeEntries.empty()) {
            it->second = *freeEntries.begin();
            freeEntries.erase(freeEntries.begin());
        } else {
            it->second = entryCount++;
        }
        ensureTable(materialTable, materialTableGrowth, (uint64_t) entryCount * sizeof(GpuMaterial));
    }
    materialEntries[handle.index] = material;
    for (auto texture : {material.baseColorTexture, material.opacityTexture}) {
        if (texture) {
            materialsUsing[texture.index].insert(handle.index);
        }
    }
    queueMaterialEntry(handle.index);
}

auto GpuScene::removeMaterial(MaterialHandle handle) -> void {
    auto it = entryOf.find(handle.index);
    if (it == entryOf.end()) {
        return;
    }
    freeEntries.insert(it->second);
    entryOf.erase(it);
    if (auto material = materialEntries.find(handle.index); material != materialEntries.end()) {
        for (auto texture : {material->second.baseColorTexture, material->second.opacityTexture}) {
            if (auto using_ = materialsUsing.find(texture.index); using_ != materialsUsing.end()) {
                using_->second.erase(handle.index);
            }
        }
        materialEntries.erase(material);
    }
    std::erase_if(queue, [&](const Upload& upload) {
        return upload.kind == Upload::Kind::MaterialEntry && upload.handle == handle.index;
    });
}

auto GpuScene::queueMaterialEntry(uint32_t materialIndex) -> void {
    queue.push_back({.kind = Upload::Kind::MaterialEntry, .handle = materialIndex, .bytes = sizeof(GpuMaterial)});
}

auto GpuScene::materialsUsingTexture(uint32_t textureIndex) const -> std::vector<uint32_t> {
    auto it = materialsUsing.find(textureIndex);
    if (it == materialsUsing.end()) {
        return {};
    }
    return {it->second.begin(), it->second.end()};
}

auto GpuScene::texturedMaterialCount() const -> uint32_t {
    uint32_t count = 0;
    for (const auto& [index, material] : materialEntries) {
        auto texture = gpuTextures.find(material.baseColorTexture.index);
        if (texture != gpuTextures.end() && (texture->second.width > 1 || texture->second.height > 1)) {
            count++;
        }
    }
    return count;
}

auto GpuScene::cutoutMaterialCount() const -> uint32_t {
    uint32_t count = 0;
    for (const auto& [index, material] : materialEntries) {
        if (material.cutout()) {
            count++;
        }
    }
    return count;
}

auto GpuScene::materialEntry(MaterialHandle handle) const -> uint32_t {
    auto it = entryOf.find(handle.index);
    if (it == entryOf.end()) {
        return 0;
    }
    return it->second;
}

// The table entry for a material from what is on the GPU now: a texture still uploading samples the white pending texture, and an
// opacity texture still uploading draws the material opaque.
auto GpuScene::materialRecord(MaterialHandle handle) const -> GpuMaterial {
    auto it = materialEntries.find(handle.index);
    if (it == materialEntries.end()) {
        return {};
    }
    const auto& material = it->second;
    auto slotOf = [this](TextureHandle texture) -> std::optional<uint32_t> {
        auto found = gpuTextures.find(texture.index);
        if (!texture || found == gpuTextures.end()) {
            return std::nullopt;
        }
        if (found->second.slot == fallbackSlot) {
            return pendingSlot;
        }
        return found->second.slot;
    };
    GpuMaterial record;
    record.baseColorTexture = slotOf(material.baseColorTexture).value_or(fallbackSlot);
    if (!material.cutout()) {
        return record;
    }
    record.opacityInfo = (material.opacityChannel & 3u) | (material.opacityRaw ? 4u : 0u);
    if (material.opacityFromBaseColor) {
        record.opacityTexture = record.baseColorTexture;
        record.opacityThreshold = record.baseColorTexture == pendingSlot ? 0.0f : material.opacityThreshold;
        return record;
    }
    if (material.opacityTexture) {
        auto slot = slotOf(material.opacityTexture);
        if (slot && *slot != pendingSlot) {
            record.opacityTexture = *slot;
            record.opacityThreshold = material.opacityThreshold;
        }
        return record;
    }
    // A constant: at the threshold or above draws everywhere (opaque); below it, cut away everywhere (opacity texture 0).
    if (material.opacity < material.opacityThreshold) {
        record.opacityThreshold = material.opacityThreshold;
    }
    return record;
}

auto GpuScene::beginFrame(uint64_t frame) -> bool {
    currentFrame = frame;
    // The pools' growths: one that is ready is taken, and its waiting meshes placed.
    for (auto pool : {Pool::Vertices, Pool::Meshlets, Pool::MeshletVertices, Pool::MeshletTriangles}) {
        auto& shortBy = poolShort[(size_t) pool];
        if (rangesOf(pool).capacity() == 0) {
            continue;
        }
        if (updatePool(pool, std::max(rangesOf(pool).used(), shortBy))) {
            shortBy = 0;
            poolRoomFreed = true;
        }
    }
    // Meshes that waited for room, once a frame after removals or a growth made some.
    if (std::exchange(poolRoomFreed, false)) {
        placeWaitingMeshes();
    }
    for (auto index : texturesCopied) {
        auto it = gpuTextures.find(index);
        if (it == gpuTextures.end()) {
            continue;
        }
        uint32_t slot = 0;
        if (!freeTextureSlots.empty()) {
            slot = *freeTextureSlots.begin();
            freeTextureSlots.erase(freeTextureSlots.begin());
        } else if (slots.size() < maxTextures) {
            slot = (uint32_t) slots.size();
            slots.push_back(fallbackTexture);
        } else {
            slotOverflow++;
            TRACE_WARNING("Render", "TextureSlotsFull", "materials").text(std::format("texture {}: no slot left of {}; its materials use the fallback", index, maxTextures));
            continue;
        }
        slots[slot] = it->second.texture;
        it->second.slot = slot;
        descriptorsDirty = true;
        if (auto using_ = materialsUsing.find(index); using_ != materialsUsing.end()) {
            for (auto material : using_->second) {
                queueMaterialEntry(material);
            }
        }
    }
    texturesCopied.clear();
    return std::exchange(descriptorsDirty, false);
}

auto GpuScene::idle() const -> bool {
    auto buffers = std::array{&vertices, &positions, &meshlets, &meshletVertices, &meshletTriangles, &meshTable, &materialTable};
    bool fresh = std::ranges::any_of(buffers, [](const GrowableBuffer* target) { return target->fresh; });
    return queue.empty() && texturesCopied.empty() && !fresh;
}

auto GpuScene::pendingBytes() const -> uint64_t {
    uint64_t bytes = 0;
    for (const auto& upload : queue) {
        bytes += upload.bytes;
    }
    return bytes;
}

template <typename Acquire, typename Retire>
auto GpuScene::record(RhiCommandBuffer* cmd, uint64_t budget, uint64_t stagingCapacity, const Acquire& acquire, const Retire& retire) -> void {
    // The buffers there are: the pool's exist once a mesh has been added.
    std::vector<GrowableBuffer*> targets;
    for (auto* target : poolBuffers()) {
        if (target->buffer != nullptr) {
            targets.push_back(target);
        }
    }

    // What this record takes: at most `budget` bytes, at least one upload that fits the staging (a larger one waits for it to grow).
    size_t taken = 0;
    uint64_t takenBytes = 0;
    while (taken < queue.size() && ((taken == 0 && queue[0].bytes <= stagingCapacity) || takenBytes + queue[taken].bytes <= budget)) {
        takenBytes += queue[taken].bytes;
        taken++;
    }
    lastUploads = (uint32_t) taken;
    lastUploadBytes = takenBytes;
    lastUploadFrame = currentFrame;
    totalUploadBytes += takenBytes;
    // One staging buffer for the batch, and the zeros a grown table's new part needs.
    uint64_t zeroBytes = 0;
    for (auto* target : targets) {
        if (target->fresh && target->zeroed) {
            zeroBytes = std::max(zeroBytes, target->capacity - target->moveBytes);
        }
    }
    auto stagingBytes = takenBytes + zeroBytes;
    Staging staging;
    RhiBuffer* batch = nullptr;
    std::byte* mapped = nullptr;
    if (stagingBytes > 0) {
        PROFILE_ZONE("UploadStaging", Upload);
        PROFILE_ZONE_VALUE(stagingBytes);
        staging = acquire(stagingBytes);
        batch = staging.buffer;
        mapped = staging.mapped;
        PROFILE_ZONE_DETAIL("ZeroGrownTables", Upload);
        PROFILE_ZONE_VALUE(zeroBytes);
        std::memset(mapped + takenBytes, 0, zeroBytes);
    }

    // Into the transfer state: after every earlier read of the tables and the pool (on one queue, a barrier covers earlier submissions).
    std::vector<RhiBufferBarrierDesc> before;
    for (auto* target : targets) {
        before.push_back({.buffer = target->buffer, .oldState = target->fresh ? RhiBufferState::Undefined : target->readState, .newState = RhiBufferState::TransferDst});
        if (target->moveFrom != nullptr) {
            before.push_back({.buffer = target->moveFrom, .oldState = target->readState, .newState = RhiBufferState::TransferSrc});
        }
    }
    cmd->bufferBarrier(before);

    // Grown buffers: the old contents first, then zeros past them for the tables.
    PROFILE_GPU_ZONE_DETAIL(cmd, "UploadBatch", Upload);
    for (auto* target : targets) {
        if (target->moveFrom != nullptr) {
            cmd->copyBuffer(target->moveFrom, target->buffer, {.size = target->moveBytes});
            deletionQueue->deferBuffer(currentFrame, target->moveFrom);
            target->moveFrom = nullptr;
        }
        if (target->fresh && target->zeroed && target->capacity > target->moveBytes) {
            cmd->copyBuffer(batch, target->buffer, {.srcOffset = takenBytes, .dstOffset = target->moveBytes, .size = target->capacity - target->moveBytes});
        }
        target->moveBytes = 0;
        target->fresh = false;
    }
    // Copies in one command buffer may run in any order: the moves and zeros finish before the uploads overwrite parts of them.
    std::vector<RhiBufferBarrierDesc> afterMoves;
    for (auto* target : targets) {
        afterMoves.push_back({.buffer = target->buffer, .oldState = RhiBufferState::TransferDst, .newState = RhiBufferState::TransferDst});
    }
    cmd->bufferBarrier(afterMoves);

    // The uploads, packed into the batch in order.
    uint64_t offset = 0;
    std::vector<RhiTextureBarrierDesc> toTransfer;
    std::vector<RhiTextureBarrierDesc> toShader;
    for (size_t i = 0; i < taken; i++) {
        const auto& upload = queue[i];
        if (upload.kind == Upload::Kind::TextureLevel && upload.level == 0) {
            PROFILE_ZONE("CreateTexture", Upload);
            auto& info = gpuTextures.at(upload.handle);
            auto name = std::format("texture.{}", upload.handle);
            info.texture = device->createTexture({
                .width = info.width,
                .height = info.height,
                .format = info.format,
                // TransferSrc for the texture inspector's blits and level dumps.
                .usage = RhiTextureUsage::Sampled | RhiTextureUsage::TransferDst | RhiTextureUsage::TransferSrc,
                .mipLevels = info.mipLevels,
                .debugName = name.c_str(),
            });
            toTransfer.push_back({.texture = info.texture, .oldState = RhiTextureState::Undefined, .newState = RhiTextureState::TransferDst});
        }
    }
    if (!toTransfer.empty()) {
        cmd->pipelineBarrier(toTransfer);
    }
    {
        // The uploads' bytes packed into the batch (CPU) and their copies (GPU, with profile.gpu.detail).
        PROFILE_ZONE_DETAIL("UploadCopies", Upload);
        PROFILE_ZONE_VALUE(takenBytes);
        PROFILE_GPU_ZONE_DETAIL(cmd, "UploadCopies", Upload);
        for (size_t i = 0; i < taken; i++) {
            const auto& upload = queue[i];
            auto* dst = mapped + offset;
            switch (upload.kind) {
                case Upload::Kind::MeshVertices: {
                    const auto& mesh = *meshData.at(upload.handle);
                    std::memcpy(dst, mesh.vertices.data(), upload.bytes);
                    cmd->copyBuffer(batch, vertices.buffer, {.srcOffset = offset, .dstOffset = (uint64_t) meshes.at(upload.handle).vertexOffset * sizeof(Vertex), .size = upload.bytes});
                    break;
                }
                case Upload::Kind::MeshPositions: {
                    // Depth-only passes read the position stream alone: 12 bytes against the 44-byte Vertex.
                    const auto& mesh = *meshData.at(upload.handle);
                    auto* positionsOut = reinterpret_cast<std::array<float, 3>*>(dst);
                    for (size_t v = 0; v < mesh.vertices.size(); v++) {
                        positionsOut[v] = mesh.vertices[v].position;
                    }
                    cmd->copyBuffer(batch, positions.buffer, {.srcOffset = offset, .dstOffset = (uint64_t) meshes.at(upload.handle).vertexOffset * sizeof(std::array<float, 3>), .size = upload.bytes});
                    break;
                }
                case Upload::Kind::MeshMeshlets: {
                    // Offsets made pool positions, so a shader reads a meshlet without its mesh's ranges.
                    const auto& mesh = *meshData.at(upload.handle);
                    const auto& range = meshes.at(upload.handle);
                    auto* out = reinterpret_cast<GpuMeshlet*>(dst);
                    for (size_t i = 0; i < mesh.meshlets.size(); i++) {
                        const auto& meshlet = mesh.meshlets[i];
                        uint32_t cone = 0;
                        for (uint32_t c = 0; c < 4; c++) {
                            cone |= (uint32_t) (uint8_t) meshlet.cone[c] << (8 * c);
                        }
                        out[i] = {
                            .vertexOffset = range.meshletVertexOffset + meshlet.vertexOffset,
                            .triangleOffset = range.meshletTriangleOffset + meshlet.triangleOffset,
                            .counts = meshlet.vertexCount | (meshlet.triangleCount << 8),
                            .cone = cone,
                            .center = glm::vec3(meshlet.center[0], meshlet.center[1], meshlet.center[2]),
                            .radius = meshlet.radius,
                        };
                    }
                    cmd->copyBuffer(batch, meshlets.buffer, {.srcOffset = offset, .dstOffset = (uint64_t) range.meshletOffset * sizeof(GpuMeshlet), .size = upload.bytes});
                    break;
                }
                case Upload::Kind::MeshMeshletVertices: {
                    const auto& mesh = *meshData.at(upload.handle);
                    std::memcpy(dst, mesh.meshletVertices.data(), upload.bytes);
                    cmd->copyBuffer(batch, meshletVertices.buffer, {.srcOffset = offset, .dstOffset = (uint64_t) meshes.at(upload.handle).meshletVertexOffset * sizeof(uint32_t), .size = upload.bytes});
                    break;
                }
                case Upload::Kind::MeshMeshletTriangles: {
                    const auto& mesh = *meshData.at(upload.handle);
                    std::memcpy(dst, mesh.meshletTriangles.data(), upload.bytes);
                    cmd->copyBuffer(batch, meshletTriangles.buffer, {.srcOffset = offset, .dstOffset = (uint64_t) meshes.at(upload.handle).meshletTriangleOffset * sizeof(uint32_t), .size = upload.bytes});
                    break;
                }
                case Upload::Kind::MeshEntry: {
                    GpuMeshEntry entry;
                    if (auto it = meshes.find(upload.handle); it != meshes.end()) {
                        entry = {
                            .vertexOffset = it->second.vertexOffset,
                            .meshletOffset = it->second.meshletOffset,
                            .meshletCount = it->second.meshletCount,
                            .triangleCount = it->second.meshletTriangleCount,
                        };
                        // Every upload of the mesh is recorded by now: its data can go.
                        meshData.erase(upload.handle);
                    }
                    std::memcpy(dst, &entry, sizeof(entry));
                    cmd->copyBuffer(batch, meshTable.buffer, {.srcOffset = offset, .dstOffset = (uint64_t) upload.handle * sizeof(GpuMeshEntry), .size = sizeof(entry)});
                    break;
                }
                case Upload::Kind::MaterialEntry: {
                    auto record = materialRecord(MaterialHandle{upload.handle});
                    std::memcpy(dst, &record, sizeof(record));
                    cmd->copyBuffer(batch, materialTable.buffer, {.srcOffset = offset, .dstOffset = (uint64_t) entryOf.at(upload.handle) * sizeof(GpuMaterial), .size = sizeof(record)});
                    break;
                }
                case Upload::Kind::TextureLevel: {
                    const auto& data = *textureData.at(upload.handle);
                    const auto& info = gpuTextures.at(upload.handle);
                    std::memcpy(dst, data.levels.data() + levelOffset(info.width, info.height, upload.level), upload.bytes);
                    cmd->copyBufferToTexture(batch, info.texture, {.bufferOffset = offset, .width = std::max(1u, info.width >> upload.level), .height = std::max(1u, info.height >> upload.level), .mipLevel = upload.level});
                    if (--levelsLeft.at(upload.handle) == 0) {
                        toShader.push_back({.texture = info.texture, .oldState = RhiTextureState::TransferDst, .newState = RhiTextureState::ShaderReadOnly});
                        texturesCopied.push_back(upload.handle);
                        textureData.erase(upload.handle);
                        levelsLeft.erase(upload.handle);
                    }
                    break;
                }
            }
            offset += upload.bytes;
        }
    }
    queue.erase(queue.begin(), queue.begin() + (std::ptrdiff_t) taken);
    if (batch != nullptr && staging.own) {
        device->unmapBuffer(batch);
        retire(batch);
    }

    // Back to what the frame reads them as.
    if (!toShader.empty()) {
        cmd->pipelineBarrier(toShader);
    }
    std::vector<RhiBufferBarrierDesc> after;
    for (auto* target : targets) {
        after.push_back({.buffer = target->buffer, .oldState = RhiBufferState::TransferDst, .newState = target->readState});
    }
    cmd->bufferBarrier(after);
}

auto GpuScene::updateUploadRing() -> void {
    // Room for the budget, and for the next upload when it alone is larger (a texture level over the budget): it waits for the growth.
    auto needed = std::max(uploadBudget, queue.empty() ? 0 : queue.front().bytes);
    auto describe = [this](uint64_t bytes) -> std::vector<GrowthBufferDesc> {
        std::vector<GrowthBufferDesc> descs;
        for (size_t s = 0; s < uploadRing.size(); s++) {
            descs.push_back({.desc = {.size = bytes, .usage = RhiBufferUsage::TransferSrc, .memory = RhiMemoryUsage::CpuToGpu, .debugName = "gpuscene.upload.staging"}, .mapped = true});
        }
        return descs;
    };
    auto install = [this](uint64_t bytes, std::vector<GrownBuffer>& made) {
        for (size_t s = 0; s < uploadRing.size(); s++) {
            // Frames in flight may still copy from the old slot.
            deletionQueue->deferBuffer(currentFrame, uploadRing[s].buffer);
            uploadRing[s] = made[s];
        }
        uploadRingBytes = bytes;
    };
    auto grownCapacity = [](uint64_t wanted) -> uint64_t {
        return wanted;
    };
    if (uploadRingBytes == 0) {
        auto made = BufferGrowth::makeNow(device, describe(needed));
        install(needed, made);
        return;
    }
    uploadRingGrowth.update(device, needed, uploadRingBytes, grownCapacity, describe, install);
}

auto GpuScene::updateInstanceStaging(uint64_t bytes) -> void {
    lastInstanceStagingNeed = bytes;
    auto describe = [this](uint64_t capacity) -> std::vector<GrowthBufferDesc> {
        std::vector<GrowthBufferDesc> descs;
        for (size_t s = 0; s < instanceStaging.size(); s++) {
            descs.push_back(
                {.desc = {.size = capacity, .usage = RhiBufferUsage::TransferSrc, .memory = RhiMemoryUsage::CpuToGpu, .debugName = "gpuscene.instances.staging"}, .mapped = true});
        }
        return descs;
    };
    auto install = [this](uint64_t capacity, std::vector<GrownBuffer>& made) {
        for (size_t s = 0; s < instanceStaging.size(); s++) {
            // Frames in flight may still copy from the old slot.
            deletionQueue->deferBuffer(currentFrame, instanceStaging[s].buffer);
            instanceStaging[s] = made[s];
        }
        instanceStagingBytes = capacity;
    };
    auto grownCapacity = [](uint64_t wanted) -> uint64_t {
        return std::min(std::max(wanted, instanceStagingMinimum), instanceStagingLimit);
    };
    if (instanceStagingBytes == 0) {
        auto made = BufferGrowth::makeNow(device, describe(instanceStagingMinimum));
        install(instanceStagingMinimum, made);
    }
    instanceStagingGrowth.update(device, bytes, instanceStagingBytes, grownCapacity, describe, install);
}

auto GpuScene::addUploadPass(FrameGraph& fg, uint64_t frame, uint32_t frameSlot) -> void {
    currentFrame = frame;
    updateUploadRing();
    bool moves = std::ranges::any_of(poolBuffers(), [](const GrowableBuffer* target) { return target->fresh; });
    if (queue.empty() && !moves) {
        return;
    }
    struct SceneUploadData {};
    fg.addPass<SceneUploadData>(
        "SceneUpload",
        profile::Group::Upload,
        [](FrameGraphBuilder& builder, SceneUploadData&) { builder.setSideEffects(true); },
        [this, frame, frameSlot](FrameGraphContext& ctx, const SceneUploadData&) {
            PROFILE_ZONE("SceneUpload", Upload);
            auto acquire = [this, frameSlot](uint64_t bytes) -> Staging {
                if (frameSlot < uploadRing.size() && bytes <= uploadRingBytes && uploadRing[frameSlot].buffer != nullptr) {
                    return {.buffer = uploadRing[frameSlot].buffer, .mapped = static_cast<std::byte*>(uploadRing[frameSlot].mapped)};
                }
                // Larger than the ring (a grown table's zeros on top of the uploads): a buffer of its own, made here.
                auto* buffer = device->createBuffer({.size = bytes, .usage = RhiBufferUsage::TransferSrc, .memory = RhiMemoryUsage::CpuToGpu, .debugName = "gpuscene.upload.staging.large"});
                return {.buffer = buffer, .mapped = static_cast<std::byte*>(device->mapBuffer(buffer)), .own = true};
            };
            record(ctx.cmd(), std::min(uploadBudget, uploadRingBytes), uploadRingBytes, acquire, [this, frame](RhiBuffer* staging) { deletionQueue->deferBuffer(frame, staging); });
        });
}

auto GpuScene::flushUploads(GpuUploader& uploader, uint64_t frame) -> void {
    PROFILE_ZONE("SceneUploadFlush", Upload);
    currentFrame = frame;
    uploader.begin();
    // Textures finished here take their slots, which queues their materials' entries: record again until nothing is left.
    do {
        auto acquire = [this](uint64_t bytes) -> Staging {
            auto* buffer = device->createBuffer({.size = bytes, .usage = RhiBufferUsage::TransferSrc, .memory = RhiMemoryUsage::CpuToGpu, .debugName = "gpuscene.upload.staging"});
            return {.buffer = buffer, .mapped = static_cast<std::byte*>(device->mapBuffer(buffer)), .own = true};
        };
        record(uploader.commandBuffer(), UINT64_MAX, UINT64_MAX, acquire, [&uploader](RhiBuffer* staging) { uploader.adoptStaging(staging); });
        beginFrame(frame);
        descriptorsDirty = true;
    } while (!queue.empty());
    uploader.end();
}

auto GpuScene::meshRange(uint32_t meshIndex) const -> const GpuMeshRange* {
    auto it = meshes.find(meshIndex);
    if (it == meshes.end()) {
        return nullptr;
    }
    return &it->second;
}

auto GpuScene::updateInstances(const ChunkedList<GpuInstance>& sceneInstances, std::span<const uint32_t> changedSlots, uint64_t frame) -> void {
    auto count = (uint32_t) sceneInstances.size();
    ensureInstanceCapacity(count, count, frame);
    if (!allDirty) {
        for (auto slot : changedSlots) {
            markDirty(slot);
        }
    }
}

auto GpuScene::markDirty(uint32_t slot) -> void {
    auto word = slot / 64;
    if (word >= dirtyBits.size()) {
        dirtyBits.resize((size_t) word + 1, 0);
    }
    auto bit = uint64_t{1} << (slot % 64);
    if ((dirtyBits[word] & bit) == 0) {
        dirtyBits[word] |= bit;
        dirtyMarked++;
    }
}

auto GpuScene::ensureInstanceCapacity(uint32_t count, uint32_t liveCount, uint64_t frame) -> void {
    lastInstanceCount = count;
    // A multiple of 256 slots, so the culling records after the draw records start on a 32 KiB boundary: bound at that offset, they meet
    // any device's storage buffer offset alignment (at most 256 bytes).
    constexpr uint64_t slotGranule = 256;
    auto grownCapacity = [slotGranule](uint64_t needed) -> uint64_t {
        auto wanted = std::max(needed, slotGranule);
        return std::min<uint64_t>((wanted + slotGranule - 1) / slotGranule * slotGranule, UINT32_MAX / slotGranule * slotGranule);
    };
    auto describe = [](uint64_t capacity) -> std::vector<GrowthBufferDesc> {
        return {{.desc = {
                     .size = instanceBufferBytesFor(capacity),
                     .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferDst | RhiBufferUsage::TransferSrc,
                     .memory = RhiMemoryUsage::GpuOnly,
                     .debugName = "gpuscene.instances",
                 }}};
    };
    if (instances == nullptr) {
        auto capacity = grownCapacity(count);
        auto made = BufferGrowth::makeNow(device, describe(capacity));
        installInstances((uint32_t) capacity, made[0].buffer, count, liveCount, frame);
        return;
    }
    instanceGrowth.update(device, count, instanceCapacity, grownCapacity, describe, [&](uint64_t capacity, std::vector<GrownBuffer>& buffers) {
        installInstances((uint32_t) capacity, buffers[0].buffer, count, liveCount, frame);
    });
}

auto GpuScene::installInstances(uint32_t capacity, RhiBuffer* buffer, uint32_t count, uint32_t liveCount, uint64_t frame) -> void {
    // What the old buffer holds is kept: copied into the new one on the GPU before the next upload, with the slots changed since still dirty.
    // A buffer with nothing to keep yet (made at init, or replaced before its first full upload) goes, and every live slot is uploaded.
    bool keep = instances != nullptr && !allDirty;
    if (keep && instanceMoveFrom == nullptr) {
        instanceMoveFrom = instances;
        instanceMoveSlots = instanceCapacity;
        instanceMoveAccess = instanceAccess;
    } else if (instances != nullptr) {
        // Frames in flight may still read the old buffer. Grown twice before an upload: the first replacement was never written, and the
        // move still comes from the buffer before it.
        deletionQueue->deferBuffer(frame, instances);
    }
    instanceCapacity = capacity;
    instances = buffer;
    // A new buffer: no earlier access to sync with.
    instanceAccess = FgAccessFlags::None;
    generation++;
    if (!keep) {
        allDirty = liveCount > 0;
        std::ranges::fill(dirtyBits, 0);
        dirtyMarked = 0;
    }
    if (keep) {
        TRACE_EVENT("Render", "InstanceBufferGrown", "gpuscene.instances")
            .text(std::format("instance buffer grown to {} slots for {}; the {} slots before are copied on the GPU", instanceCapacity, count, instanceMoveSlots))
            .field("capacity", (int64_t) instanceCapacity)
            .field("instances", (int64_t) count)
            .field("copied", (int64_t) instanceMoveSlots);
    }
}

auto GpuScene::addInstanceUploadPasses(FrameGraph& fg, const ChunkedList<GpuInstance>& sceneInstances, uint32_t frameSlot) -> FgBufferHandle {
    // Imported with the access the previous frame left it in, so this frame's upload waits for last frame's reads.
    FgBufferDesc desc = {
        .size = instanceBufferBytes(),
        .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferDst,
    };
    instanceImport = fg.importBuffer("instances", instances, desc, instanceAccess);
    instanceImported = true;
    auto handle = instanceImport;
    if (instanceMoveFrom != nullptr) {
        // The buffer a growth replaced: its draw records to the start, its culling records to the new culling offset, before any upload.
        auto oldDrawBytes = (uint64_t) instanceMoveSlots * sizeof(GpuInstanceRecord);
        auto oldCullBytes = (uint64_t) instanceMoveSlots * sizeof(GpuCullRecord);
        auto from = fg.importBuffer("instancesBefore", instanceMoveFrom, {.size = oldDrawBytes + oldCullBytes, .usage = RhiBufferUsage::TransferSrc}, instanceMoveAccess);
        std::vector<RhiBufferCopy> moves = {
            {.srcOffset = 0, .dstOffset = 0, .size = oldDrawBytes},
            {.srcOffset = oldDrawBytes, .dstOffset = cullRecordsOffset(), .size = oldCullBytes},
        };
        handle = addInstanceUploadPass(fg, from, handle, std::move(moves), "InstanceMove");
        deletionQueue->deferBuffer(currentFrame, instanceMoveFrom);
        instanceMoveFrom = nullptr;
        instanceMoveSlots = 0;
    }
    // The slots to upload, in order, each once.
    std::vector<uint32_t> upload;
    auto count = (uint32_t) sceneInstances.size();
    {
        PROFILE_ZONE_DETAIL("DirtySlots", Upload);
        PROFILE_ZONE_VALUE(allDirty ? count : dirtyMarked);
        // Slots past the buffer's capacity wait for its growth, dirty: they aren't drawn until then.
        auto fits = std::min(count, instanceCapacity);
        if (allDirty) {
            upload.resize(fits);
            for (uint32_t m = 0; m < fits; m++) {
                upload[m] = m;
            }
            for (uint32_t m = fits; m < count; m++) {
                markDirty(m);
            }
        } else if (dirtyMarked > 0) {
            upload.reserve(dirtyMarked);
            for (size_t word = 0; word < dirtyBits.size(); word++) {
                auto bits = dirtyBits[word];
                while (bits != 0) {
                    auto slot = (uint32_t) ((word * 64) + (size_t) std::countr_zero(bits));
                    auto bit = bits & (~bits + 1);
                    bits &= bits - 1;
                    if (slot < fits) {
                        upload.push_back(slot);
                    } else if (slot < count) {
                        // Waits for the growth: stays dirty.
                        continue;
                    }
                    // Uploaded now, or past the slot count (removed): no longer dirty.
                    dirtyBits[word] &= ~bit;
                    dirtyMarked--;
                }
            }
        }
        allDirty = false;
    }
    if (upload.empty()) {
        return handle;
    }
    // Only the changed records, packed (the draw records, then the culling records): the slot's kept staging when they fit in it, else a
    // buffer of their own for this frame, freed once the frame is done.
    auto recordBytes = (uint64_t) upload.size() * sizeof(GpuInstanceRecord);
    auto bytes = recordBytes + ((uint64_t) upload.size() * sizeof(GpuCullRecord));
    RhiBuffer* buffer = nullptr;
    void* mapped = nullptr;
    {
        PROFILE_ZONE_DETAIL("InstanceStaging", Upload);
        updateInstanceStaging(bytes);
        if (bytes <= instanceStagingBytes && frameSlot < instanceStaging.size() && instanceStaging[frameSlot].buffer != nullptr) {
            buffer = instanceStaging[frameSlot].buffer;
            mapped = instanceStaging[frameSlot].mapped;
        } else {
            buffer = device->createBuffer({.size = bytes, .usage = RhiBufferUsage::TransferSrc, .memory = RhiMemoryUsage::CpuToGpu, .debugName = "gpuscene.instances.staging.large"});
            mapped = device->mapBuffer(buffer);
            deletionQueue->deferBuffer(currentFrame, buffer);
        }
    }
    auto* dst = static_cast<GpuInstanceRecord*>(mapped);
    auto* cullDst = reinterpret_cast<GpuCullRecord*>(static_cast<std::byte*>(mapped) + recordBytes);
    // A run of consecutive slots is one region.
    auto addRun = [](std::vector<RhiBufferCopy>& runs, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) {
        if (!runs.empty() && runs.back().srcOffset + runs.back().size == srcOffset && runs.back().dstOffset + runs.back().size == dstOffset) {
            runs.back().size += size;
        } else {
            runs.push_back({.srcOffset = srcOffset, .dstOffset = dstOffset, .size = size});
        }
    };
    // On the workers: each range of the list writes its own records (record i to staging index i) and its own regions; the ranges' regions
    // are joined in order after, a run crossing two ranges made one again.
    constexpr size_t packGrain = 16 * 1024;
    auto ranges = (upload.size() + packGrain - 1) / packGrain;
    std::vector<std::vector<RhiBufferCopy>> rangeRegions(ranges);
    std::vector<std::vector<RhiBufferCopy>> rangeCullRegions(ranges);
    {
        PROFILE_ZONE_DETAIL("PackInstanceRecords", Upload);
        PROFILE_ZONE_VALUE(upload.size());
        JobSystem::parallelFor(
            upload.size(),
            packGrain,
            [&](size_t begin, size_t end) {
                auto& runs = rangeRegions[begin / packGrain];
                auto& cullRuns = rangeCullRegions[begin / packGrain];
                for (size_t i = begin; i < end; i++) {
                    auto m = upload[i];
                    const auto& inst = sceneInstances[m];
                    uint32_t flags = 0;
                    if (inst.doubleSided) {
                        flags |= gpuInstanceDoubleSided;
                    }
                    if (inst.worldBounds.valid()) {
                        flags |= gpuInstanceBoundsValid;
                    }
                    if (inst.cutout) {
                        flags |= gpuInstanceCutout;
                    }
                    // Built here and written once: the staging buffer is CpuToGpu memory, not CPU-cached, so reading it back is slow.
                    GpuInstanceRecord record;
                    if (!inst.alive) {
                        record = {.model = glm::mat4(1.0f), .mesh = deadMesh};
                    } else {
                        record = {
                            .model = inst.transform,
                            .material = materialEntry(inst.material),
                            .mesh = inst.mesh.index,
                            .meshletOffset = inst.meshletOffset,
                            .meshletCount = inst.meshletCount,
                            .boundsMin = inst.worldBounds.min,
                            .flags = flags,
                            .boundsMax = inst.worldBounds.max,
                            .lodLevel = inst.lodLevel,
                            .lodGroup = inst.lodGroup,
                            .triangleCount = inst.indexCount / 3,
                            .streamRoot = inst.streamRoot,
                            .lodDepth = inst.lodDepth,
                        };
                    }
                    dst[i] = record;
                    cullDst[i] = {
                        .boundsMin = record.boundsMin,
                        .flags = record.flags,
                        .boundsMax = record.boundsMax,
                        .lod = (record.lodLevel & 0xFFu) | (record.lodGroup << 8),
                        .mesh = record.mesh,
                        .meshletOffset = record.meshletOffset,
                        .meshletCount = record.meshletCount,
                        .triangleCount = record.triangleCount,
                    };
                    addRun(runs, (uint64_t) i * sizeof(GpuInstanceRecord), (uint64_t) m * sizeof(GpuInstanceRecord), sizeof(GpuInstanceRecord));
                    addRun(cullRuns, recordBytes + ((uint64_t) i * sizeof(GpuCullRecord)), cullRecordsOffset() + ((uint64_t) m * sizeof(GpuCullRecord)), sizeof(GpuCullRecord));
                }
            },
            "PackInstanceRange");
    }
    std::vector<RhiBufferCopy> regions;
    std::vector<RhiBufferCopy> cullRegions;
    for (size_t r = 0; r < ranges; r++) {
        for (const auto& run : rangeRegions[r]) {
            addRun(regions, run.srcOffset, run.dstOffset, run.size);
        }
        for (const auto& run : rangeCullRegions[r]) {
            addRun(cullRegions, run.srcOffset, run.dstOffset, run.size);
        }
    }
    lastInstanceRegions = (uint32_t) regions.size();
    regions.insert(regions.end(), cullRegions.begin(), cullRegions.end());
    auto stagingHandle = fg.importBuffer("instanceStaging", buffer, {.size = bytes, .usage = RhiBufferUsage::TransferSrc});
    lastInstanceUploads = (uint32_t) upload.size();
    return addInstanceUploadPass(fg, stagingHandle, handle, std::move(regions), "InstanceUpload");
}

auto GpuScene::afterExecute(const FrameGraph& fg) -> void {
    if (!instanceImported) {
        return;
    }
    instanceAccess = fg.finalAccess(instanceImport);
    instanceImported = false;
}

namespace renderconfig {

config::Int uploadBudgetMiB{"render.upload.budgetMiB",
                            (int64_t) (GpuScene::defaultUploadBudget / (1024 * 1024)),
                            "MiB the scene's uploads may copy per frame (the SceneUpload pass); the rest waits for the next frame.",
                            {.min = 1}};
config::Int geometryLimitMiB{"render.geometry.limitMiB",
                             0,
                             "MiB each geometry pool buffer may grow to; 0 for the device's limit. Lowering it stops growth, it doesn't shrink the pool.",
                             {.min = 0}};

} // namespace renderconfig
