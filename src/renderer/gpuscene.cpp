#include "gpuscene.h"

#include "deletionqueue.h"
#include "framegraph.h"
#include "framegraphdebug.h"
#include "gpuuploader.h"
#include "instanceuploadpass.h"
#include "mesh.h"
#include "profile.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "trace.h"

#include <algorithm>
#include <array>
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
    staging.assign(frameSlots, nullptr);
    stagingMapped.assign(frameSlots, nullptr);
    vertices = {.usage = RhiBufferUsage::Vertex, .readState = RhiBufferState::VertexRead, .name = "gpuscene.pool.vertices"};
    positions = {.usage = RhiBufferUsage::Vertex, .readState = RhiBufferState::VertexRead, .name = "gpuscene.pool.positions"};
    indices = {.usage = RhiBufferUsage::Index, .readState = RhiBufferState::IndexRead, .name = "gpuscene.pool.indices"};
    meshTable = {.usage = RhiBufferUsage::Storage, .readState = RhiBufferState::StorageRead, .name = "gpuscene.meshtable", .zeroed = true};
    materialTable = {.usage = RhiBufferUsage::Storage, .readState = RhiBufferState::StorageRead, .name = "gpuscene.materials", .zeroed = true};
    // Never null, so every descriptor set has something to name.
    ensureCapacity(meshTable, sizeof(GpuMeshEntry));
    ensureCapacity(materialTable, sizeof(GpuMaterial));
    ensureInstanceCapacity(0, 0, 0);
}

auto GpuScene::destroy() -> void {
    for (auto* target : {&vertices, &positions, &indices, &meshTable, &materialTable}) {
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
    for (auto*& buffer : staging) {
        device->destroyBuffer(buffer);
        buffer = nullptr;
    }
}

auto GpuScene::ensureCapacity(GrowableBuffer& target, uint64_t bytes, uint64_t maxBytes) -> bool {
    if (target.buffer != nullptr && bytes <= target.capacity) {
        return false;
    }
    if (bytes > maxBytes) {
        return false;
    }
    PROFILE_ZONE("GrowBuffer");
    PROFILE_ZONE_VALUE(bytes);
    auto capacity = std::min(std::max({bytes, target.capacity * 2, minimumBufferBytes}), maxBytes);
    auto* grown = device->createBuffer({
        .size = capacity,
        // TransferSrc for the next growth's copy and for the capture service.
        .usage = target.usage | RhiBufferUsage::TransferDst | RhiBufferUsage::TransferSrc,
        .memory = RhiMemoryUsage::GpuOnly,
        .debugName = target.name,
    });
    if (grown == nullptr) {
        return false;
    }
    if (target.buffer != nullptr && target.fresh) {
        // Nothing has written it since it was made (grown twice before a record, or made at init and never used): nothing to copy. A
        // copy still pending comes from the buffer before it.
        deletionQueue->deferBuffer(currentFrame, target.buffer);
    } else if (target.buffer != nullptr) {
        target.moveFrom = target.buffer;
        target.moveBytes = target.capacity;
    }
    target.buffer = grown;
    target.capacity = capacity;
    target.fresh = true;
    descriptorsDirty = true;
    return true;
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

auto GpuScene::growPool(uint64_t vertexCount, uint64_t indexCount) -> void {
    auto limit = poolLimitBytes();
    if (vertexCount > 0) {
        // The vertex stream reaches the limit first (a Vertex is larger than a position): it sets the vertex capacity of both.
        auto maxVertices = limit / sizeof(Vertex);
        auto capacity = std::min(std::max(vertexRanges.capacity() * 2, vertexRanges.capacity() + vertexCount), maxVertices);
        ensureCapacity(vertices, capacity * sizeof(Vertex), maxVertices * sizeof(Vertex));
        ensureCapacity(positions, capacity * sizeof(std::array<float, 3>), maxVertices * sizeof(std::array<float, 3>));
        vertexRanges.grow(std::min(vertices.capacity / sizeof(Vertex), positions.capacity / sizeof(std::array<float, 3>)));
    }
    if (indexCount > 0) {
        auto maxIndices = limit / sizeof(uint32_t);
        auto capacity = std::min(std::max(indexRanges.capacity() * 2, indexRanges.capacity() + indexCount), maxIndices);
        ensureCapacity(indices, capacity * sizeof(uint32_t), maxIndices * sizeof(uint32_t));
        indexRanges.grow(indices.capacity / sizeof(uint32_t));
    }
}

auto GpuScene::addMesh(MeshHandle handle, std::shared_ptr<const MeshDesc> mesh) -> void {
    if (!handle || mesh == nullptr || mesh->vertices.empty() || mesh->indices.empty()) {
        return;
    }
    // The table entry exists either way: a mesh that waits has an empty one, and its draws draw nothing.
    ensureCapacity(meshTable, ((uint64_t) handle.index + 1) * sizeof(GpuMeshEntry));
    if (placeMesh(handle, mesh)) {
        return;
    }
    if (waitingMeshes.empty()) {
        auto limit = poolLimitBytes();
        TRACE_WARNING("Render", "GeometryPoolFull", "gpuscene.pool")
            .text(std::format("the geometry pool is full ({} MiB per buffer): meshes that don't fit wait, undrawn, until removed meshes make room", limit / (1024 * 1024)))
            .field("limitBytes", (int64_t) limit)
            .field("vertexUsed", (int64_t) vertexRanges.used())
            .field("indexUsed", (int64_t) indexRanges.used());
    }
    waitingMeshes.push_back({.handle = handle, .mesh = std::move(mesh)});
}

auto GpuScene::placeMesh(MeshHandle handle, const std::shared_ptr<const MeshDesc>& mesh) -> bool {
    uint64_t vertexCount = mesh->vertices.size();
    uint64_t indexCount = mesh->indices.size();
    auto vertexFirst = vertexRanges.allocate(vertexCount);
    if (!vertexFirst) {
        growPool(vertexCount, 0);
        vertexFirst = vertexRanges.allocate(vertexCount);
    }
    if (!vertexFirst) {
        return false;
    }
    auto indexFirst = indexRanges.allocate(indexCount);
    if (!indexFirst) {
        growPool(0, indexCount);
        indexFirst = indexRanges.allocate(indexCount);
    }
    if (!indexFirst) {
        vertexRanges.free(*vertexFirst, vertexCount);
        return false;
    }
    meshes[handle.index] = {
        .firstIndex = (uint32_t) *indexFirst,
        .vertexOffset = (int32_t) *vertexFirst,
        .indexCount = (uint32_t) indexCount,
        .vertexCount = (uint32_t) vertexCount,
        .vertexBytes = vertexCount * (sizeof(Vertex) + sizeof(std::array<float, 3>)),
        .indexBytes = indexCount * sizeof(uint32_t),
    };
    meshData[handle.index] = mesh;
    // The data before the entry: a mesh is drawn once its entry says so.
    queue.push_back({.kind = Upload::Kind::MeshVertices, .handle = handle.index, .bytes = vertexCount * sizeof(Vertex)});
    queue.push_back({.kind = Upload::Kind::MeshPositions, .handle = handle.index, .bytes = vertexCount * sizeof(std::array<float, 3>)});
    queue.push_back({.kind = Upload::Kind::MeshIndices, .handle = handle.index, .bytes = indexCount * sizeof(uint32_t)});
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
    if (emptied) {
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
    };
}

auto GpuScene::geometryPoolStats() const -> GeometryPoolStats {
    GeometryPoolStats stats = {
        .limitBytes = poolLimitBytes(),
        .vertexLimit = poolLimitBytes() / sizeof(Vertex),
        .indexLimit = poolLimitBytes() / sizeof(uint32_t),
        .vertexCapacity = vertexRanges.capacity(),
        .vertexUsed = vertexRanges.used(),
        .indexCapacity = indexRanges.capacity(),
        .indexUsed = indexRanges.used(),
        .waitingMeshes = (uint32_t) waitingMeshes.size(),
    };
    for (const auto& waiting : waitingMeshes) {
        stats.waitingBytes += waiting.mesh->vertices.size() * (sizeof(Vertex) + sizeof(std::array<float, 3>)) + waiting.mesh->indices.size() * sizeof(uint32_t);
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
    indexRanges.free(it->second.firstIndex, it->second.indexCount);
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
        .format = RhiFormat::R8G8B8A8_SRGB,
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
        ensureCapacity(materialTable, (uint64_t) entryCount * sizeof(GpuMaterial));
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
    // Meshes that waited for room, once a frame after removals freed some.
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
    bool fresh = std::ranges::any_of(std::array{&vertices, &positions, &indices, &meshTable, &materialTable}, [](const GrowableBuffer* target) { return target->fresh; });
    return queue.empty() && texturesCopied.empty() && !fresh;
}

auto GpuScene::pendingBytes() const -> uint64_t {
    uint64_t bytes = 0;
    for (const auto& upload : queue) {
        bytes += upload.bytes;
    }
    return bytes;
}

template <typename Retire>
auto GpuScene::record(RhiCommandBuffer* cmd, uint64_t budget, const Retire& retire) -> void {
    // The buffers there are: the pool's exist once a mesh has been added.
    std::vector<GrowableBuffer*> targets;
    for (auto* target : {&vertices, &positions, &indices, &meshTable, &materialTable}) {
        if (target->buffer != nullptr) {
            targets.push_back(target);
        }
    }

    // What this record takes: at most `budget` bytes, at least one upload.
    size_t taken = 0;
    uint64_t takenBytes = 0;
    while (taken < queue.size() && (taken == 0 || takenBytes + queue[taken].bytes <= budget)) {
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
    RhiBuffer* batch = nullptr;
    std::byte* mapped = nullptr;
    if (stagingBytes > 0) {
        PROFILE_ZONE("UploadStaging");
        batch = device->createBuffer({.size = stagingBytes, .usage = RhiBufferUsage::TransferSrc, .memory = RhiMemoryUsage::CpuToGpu, .debugName = "gpuscene.upload.staging"});
        mapped = static_cast<std::byte*>(device->mapBuffer(batch));
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
            PROFILE_ZONE("CreateTexture");
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
            case Upload::Kind::MeshIndices: {
                const auto& mesh = *meshData.at(upload.handle);
                std::memcpy(dst, mesh.indices.data(), upload.bytes);
                cmd->copyBuffer(batch, indices.buffer, {.srcOffset = offset, .dstOffset = (uint64_t) meshes.at(upload.handle).firstIndex * sizeof(uint32_t), .size = upload.bytes});
                break;
            }
            case Upload::Kind::MeshEntry: {
                GpuMeshEntry entry;
                if (auto it = meshes.find(upload.handle); it != meshes.end()) {
                    entry = {.firstIndex = it->second.firstIndex, .vertexOffset = it->second.vertexOffset, .indexCount = it->second.indexCount};
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
    queue.erase(queue.begin(), queue.begin() + (std::ptrdiff_t) taken);
    if (batch != nullptr) {
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

auto GpuScene::addUploadPass(FrameGraph& fg, uint64_t frame) -> void {
    currentFrame = frame;
    bool moves = std::ranges::any_of(std::array{&vertices, &positions, &indices, &meshTable, &materialTable}, [](const GrowableBuffer* target) { return target->fresh; });
    if (queue.empty() && !moves) {
        return;
    }
    struct SceneUploadData {};
    fg.addPass<SceneUploadData>(
        "SceneUpload",
        [](FrameGraphBuilder& builder, SceneUploadData&) { builder.setSideEffects(true); },
        [this, frame](FrameGraphContext& ctx, const SceneUploadData&) {
            PROFILE_ZONE("SceneUpload");
            record(ctx.cmd(), uploadBudget, [this, frame](RhiBuffer* staging) { deletionQueue->deferBuffer(frame, staging); });
        });
}

auto GpuScene::flushUploads(GpuUploader& uploader, uint64_t frame) -> void {
    PROFILE_ZONE("SceneUploadFlush");
    currentFrame = frame;
    uploader.begin();
    // Textures finished here take their slots, which queues their materials' entries: record again until nothing is left.
    do {
        record(uploader.commandBuffer(), UINT64_MAX, [&uploader](RhiBuffer* staging) { uploader.adoptStaging(staging); });
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

auto GpuScene::updateInstances(std::span<const GpuInstance> sceneInstances, std::span<const uint32_t> changedSlots, uint64_t frame) -> void {
    auto count = (uint32_t) sceneInstances.size();
    ensureInstanceCapacity(count, count, frame);
    for (auto slot : changedSlots) {
        markDirty(slot, slot + 1);
    }
}

auto GpuScene::ensureInstanceCapacity(uint32_t count, uint32_t liveCount, uint64_t frame) -> void {
    if (instances != nullptr && count <= instanceCapacity) {
        return;
    }
    // Frames in flight may still read the old buffers.
    deletionQueue->deferBuffer(frame, instances);
    for (auto*& buffer : staging) {
        deletionQueue->deferBuffer(frame, buffer);
        buffer = nullptr;
    }

    constexpr uint32_t minCapacity = 256;
    instanceCapacity = std::max({count, instanceCapacity * 2, minCapacity});
    auto bytes = instanceBufferBytes();
    instances = device->createBuffer({
        .size = bytes,
        .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferDst | RhiBufferUsage::TransferSrc,
        .memory = RhiMemoryUsage::GpuOnly,
        .debugName = "gpuscene.instances",
    });
    for (size_t i = 0; i < staging.size(); i++) {
        staging[i] = device->createBuffer({
            .size = bytes,
            .usage = RhiBufferUsage::TransferSrc,
            .memory = RhiMemoryUsage::CpuToGpu,
            .debugName = "gpuscene.instances.staging",
        });
        stagingMapped[i] = device->mapBuffer(staging[i]);
    }
    // New contents: no earlier access to sync with, and every instance needs uploading.
    instanceAccess = FgAccessFlags::None;
    generation++;
    markDirty(0, liveCount);
}

auto GpuScene::markDirty(uint32_t first, uint32_t end) -> void {
    if (end <= first) {
        return;
    }
    if (dirtyEnd <= dirtyFirst) {
        dirtyFirst = first;
        dirtyEnd = end;
        return;
    }
    dirtyFirst = std::min(dirtyFirst, first);
    dirtyEnd = std::max(dirtyEnd, end);
}

auto GpuScene::addInstanceUploadPasses(FrameGraph& fg, std::span<const GpuInstance> sceneInstances, uint32_t frameSlot) -> FgBufferHandle {
    // Imported with the access the previous frame left it in, so this frame's upload waits
    // for last frame's reads. Only the dirty span is copied.
    FgBufferDesc desc = {
        .size = instanceBufferBytes(),
        .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferDst,
    };
    instanceImport = fg.importBuffer("instances", instances, desc, instanceAccess);
    instanceImported = true;
    auto handle = instanceImport;
    dirtyEnd = std::min(dirtyEnd, (uint32_t) sceneInstances.size());
    if (dirtyEnd > dirtyFirst) {
        auto* dst = static_cast<GpuInstanceRecord*>(stagingMapped[frameSlot]);
        for (uint32_t m = dirtyFirst; m < dirtyEnd; m++) {
            const auto& inst = sceneInstances[m];
            uint32_t flags = 0;
            if (inst.primFirst) {
                flags |= gpuInstancePrimFirst;
            }
            if (inst.doubleSided) {
                flags |= gpuInstanceDoubleSided;
            }
            if (inst.worldBounds.valid()) {
                flags |= gpuInstanceBoundsValid;
            }
            if (inst.cutout) {
                flags |= gpuInstanceCutout;
            }
            if (inst.primHasCutout) {
                flags |= gpuInstancePrimHasCutout;
            }
            if (!inst.alive) {
                dst[m] = {.model = glm::mat4(1.0f), .mesh = deadMesh};
                continue;
            }
            dst[m] = {
                .model = inst.transform,
                .material = materialEntry(inst.material),
                .mesh = inst.mesh.index,
                .indexOffset = inst.indexOffset,
                .indexCount = inst.indexCount,
                .boundsMin = inst.worldBounds.min,
                .flags = flags,
                .boundsMax = inst.worldBounds.max,
                .lodLevel = inst.lodLevel,
            };
        }
        RhiBufferCopy region = {
            .srcOffset = (uint64_t) dirtyFirst * sizeof(GpuInstanceRecord),
            .dstOffset = (uint64_t) dirtyFirst * sizeof(GpuInstanceRecord),
            .size = (uint64_t) (dirtyEnd - dirtyFirst) * sizeof(GpuInstanceRecord),
        };
        auto stagingHandle = fg.importBuffer("instanceStaging", staging[frameSlot], {.size = desc.size, .usage = RhiBufferUsage::TransferSrc});
        handle = addInstanceUploadPass(fg, stagingHandle, instanceImport, region);
    }
    dirtyFirst = 0;
    dirtyEnd = 0;
    return handle;
}

auto GpuScene::afterExecute(const FrameGraph& fg) -> void {
    if (!instanceImported) {
        return;
    }
    instanceAccess = fg.finalAccess(instanceImport);
    instanceImported = false;
}
