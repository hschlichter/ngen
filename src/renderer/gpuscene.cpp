#include "gpuscene.h"

#include "deletionqueue.h"
#include "framegraph.h"
#include "framegraphdebug.h"
#include "gpuuploader.h"
#include "instanceuploadpass.h"
#include "mesh.h"
#include "observationmacros.h"
#include "profile.h"
#include "rhidevice.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <print>

auto GpuScene::init(RhiDevice* rhiDevice, uint32_t frameSlots, DeletionQueue* queue) -> void {
    device = rhiDevice;
    deletionQueue = queue;
    staging.assign(frameSlots, nullptr);
    stagingMapped.assign(frameSlots, nullptr);
    ensureInstanceCapacity(0, 0, 0);
}

auto GpuScene::destroy() -> void {
    device->destroyBuffer(poolVertices);
    device->destroyBuffer(poolPositions);
    device->destroyBuffer(poolIndices);
    poolVertices = nullptr;
    poolPositions = nullptr;
    poolIndices = nullptr;
    meshes.clear();
    device->destroyBuffer(meshTable);
    meshTable = nullptr;

    device->destroyBuffer(materialTable);
    materialTable = nullptr;
    slots.clear();

    device->destroyBuffer(instances);
    instances = nullptr;
    for (auto*& buffer : staging) {
        device->destroyBuffer(buffer);
        buffer = nullptr;
    }
}

auto GpuScene::rebuildGeometry(std::span<const GpuInstance> sceneInstances, const MeshLibrary& meshLib, GpuUploader& uploader, uint64_t frame) -> void {
    PROFILE_ZONE("RebuildGeometryPool");
    auto start = std::chrono::steady_clock::now();

    // Frames in flight may still draw from the old pool.
    deletionQueue->deferBuffer(frame, poolVertices);
    deletionQueue->deferBuffer(frame, poolPositions);
    deletionQueue->deferBuffer(frame, poolIndices);
    deletionQueue->deferBuffer(frame, meshTable);
    meshTable = nullptr;
    poolVertices = nullptr;
    poolPositions = nullptr;
    poolIndices = nullptr;
    poolBytes = 0;
    meshes.clear();

    // Concatenate every referenced mesh, in instance order. Depth-only passes read the
    // position stream alone: 12 bytes against the 44-byte Vertex.
    std::vector<Vertex> vertices;
    std::vector<std::array<float, 3>> positions;
    std::vector<uint32_t> indices;
    for (const auto& inst : sceneInstances) {
        if (!inst.mesh || meshes.contains(inst.mesh.index)) {
            continue;
        }
        const auto* meshData = meshLib.get(inst.mesh);
        if (meshData == nullptr || meshData->vertices.empty()) {
            continue;
        }
        GpuMeshRange range = {
            .firstIndex = (uint32_t) indices.size(),
            .vertexOffset = (int32_t) vertices.size(),
            .indexCount = (uint32_t) meshData->indices.size(),
            .vertexCount = (uint32_t) meshData->vertices.size(),
            .vertexBytes = meshData->vertices.size() * (sizeof(Vertex) + sizeof(std::array<float, 3>)),
            .indexBytes = meshData->indices.size() * sizeof(uint32_t),
        };
        vertices.insert(vertices.end(), meshData->vertices.begin(), meshData->vertices.end());
        for (const auto& v : meshData->vertices) {
            positions.push_back(v.position);
        }
        indices.insert(indices.end(), meshData->indices.begin(), meshData->indices.end());
        meshes[inst.mesh.index] = range;

        OBS_EVENT("Render", "MeshUploaded", "Mesh").field("vertex_count", (int64_t) range.vertexCount).field("index_count", (int64_t) range.indexCount);
    }

    // Mesh table for the culling passes: dense by mesh index; entry 0 and meshes outside the
    // pool stay empty. Always at least one entry so the binding is never null.
    uint32_t tableSize = 1;
    for (const auto& [index, range] : meshes) {
        tableSize = std::max(tableSize, index + 1);
    }
    std::vector<GpuMeshEntry> table(tableSize);
    for (const auto& [index, range] : meshes) {
        table[index] = {.firstIndex = range.firstIndex, .vertexOffset = range.vertexOffset, .indexCount = range.indexCount};
    }

    uploader.begin();
    if (!vertices.empty()) {
        poolVertices = uploader.uploadBuffer(std::as_bytes(std::span(vertices)), RhiBufferUsage::Vertex, "gpuscene.pool.vertices");
        poolPositions = uploader.uploadBuffer(std::as_bytes(std::span(positions)), RhiBufferUsage::Vertex, "gpuscene.pool.positions");
        poolIndices = uploader.uploadBuffer(std::as_bytes(std::span(indices)), RhiBufferUsage::Index, "gpuscene.pool.indices");
    }
    meshTable = uploader.uploadBuffer(std::as_bytes(std::span(table)), RhiBufferUsage::Storage, "gpuscene.meshtable");
    meshTableSize = table.size() * sizeof(GpuMeshEntry);
    uploader.end();

    auto vertexBytes = vertices.size() * sizeof(Vertex);
    auto positionBytes = positions.size() * sizeof(positions[0]);
    auto indexBytes = indices.size() * sizeof(uint32_t);
    poolBytes = vertexBytes + positionBytes + indexBytes;
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    OBS_EVENT("Render", "GeometryPoolBuilt", "geometry")
        .field("meshes", (int64_t) meshes.size())
        .field("vertex_bytes", (int64_t) vertexBytes)
        .field("position_bytes", (int64_t) positionBytes)
        .field("index_bytes", (int64_t) indexBytes)
        .field("ms", ms);
}

auto GpuScene::rebuildMaterials(std::span<const GpuInstance> sceneInstances,
                                const std::unordered_map<uint32_t, CachedTexture>& textures,
                                RhiTexture* fallback,
                                GpuUploader& uploader,
                                uint64_t frame) -> void {
    deletionQueue->deferBuffer(frame, materialTable);
    materialTable = nullptr;
    slots.assign(1, fallback);
    materialIndexOf.clear();
    instanceMaterial.assign(sceneInstances.size(), 0);

    // Entry 0 is "no material": samples the fallback, like an instance without a texture.
    std::vector<GpuMaterial> table(1);
    uint32_t overflow = 0;
    for (size_t m = 0; m < sceneInstances.size(); m++) {
        const auto& inst = sceneInstances[m];
        if (!inst.material) {
            continue;
        }
        auto [it, inserted] = materialIndexOf.try_emplace(inst.material.index, (uint32_t) table.size());
        if (inserted) {
            GpuMaterial material;
            auto tex = textures.find(inst.material.index);
            if (tex != textures.end()) {
                if (slots.size() < maxTextures) {
                    material.baseColorTexture = (uint32_t) slots.size();
                    slots.push_back(tex->second.texture);
                } else {
                    overflow++;
                }
            }
            table.push_back(material);
        }
        instanceMaterial[m] = it->second;
    }
    if (overflow > 0) {
        std::println(stderr, "GpuScene: {} material textures past the {}-slot limit use the fallback", overflow, maxTextures);
    }

    uploader.begin();
    materialTable = uploader.uploadBuffer(std::as_bytes(std::span(table)), RhiBufferUsage::Storage, "gpuscene.materials");
    materialTableSize = table.size() * sizeof(GpuMaterial);
    uploader.end();

    // Instance records carry the material index; rewrite them all.
    markDirty(0, (uint32_t) sceneInstances.size());
    OBS_EVENT("Render", "MaterialTableBuilt", "materials")
        .field("materials", (int64_t) materialIndexOf.size())
        .field("texture_slots", (int64_t) slots.size())
        .field("max_textures", (int64_t) maxTextures);
}

auto GpuScene::meshRange(uint32_t meshIndex) const -> const GpuMeshRange* {
    auto it = meshes.find(meshIndex);
    if (it == meshes.end()) {
        return nullptr;
    }
    return &it->second;
}

auto GpuScene::updateInstances(std::span<const GpuInstance> sceneInstances, uint32_t first, uint32_t end, uint64_t frame) -> void {
    auto count = (uint32_t) sceneInstances.size();
    ensureInstanceCapacity(count, count, frame);
    markDirty(first, end);
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
    OBS_EVENT("Render", "InstanceBufferCreated", "instances").field("capacity", (int64_t) instanceCapacity).field("bytes", (int64_t) bytes);
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

auto GpuScene::addUploadPasses(FrameGraph& fg, std::span<const GpuInstance> sceneInstances, uint32_t frameSlot) -> FgBufferHandle {
    // Imported with the access the previous frame left it in, so this frame's upload waits
    // for last frame's reads. Only the dirty span is copied.
    FgBufferDesc desc = {
        .size = instanceBufferBytes(),
        .usage = RhiBufferUsage::Storage | RhiBufferUsage::TransferDst,
    };
    instanceImport = fg.importBuffer("instances", instances, desc, instanceAccess);
    auto handle = instanceImport;
    carriedAccess = instanceAccess;
    uploadFirst = dirtyFirst;
    uploadBytes = 0;
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
            dst[m] = {
                .model = inst.transform,
                .material = m < instanceMaterial.size() ? instanceMaterial[m] : 0,
                .mesh = inst.mesh.index,
                .indexOffset = inst.indexOffset,
                .indexCount = inst.indexCount,
                .boundsMin = inst.worldBounds.min,
                .flags = flags,
                .boundsMax = inst.worldBounds.max,
            };
        }
        RhiBufferCopy region = {
            .srcOffset = (uint64_t) dirtyFirst * sizeof(GpuInstanceRecord),
            .dstOffset = (uint64_t) dirtyFirst * sizeof(GpuInstanceRecord),
            .size = (uint64_t) (dirtyEnd - dirtyFirst) * sizeof(GpuInstanceRecord),
        };
        auto stagingHandle = fg.importBuffer("instanceStaging", staging[frameSlot], {.size = desc.size, .usage = RhiBufferUsage::TransferSrc});
        handle = addInstanceUploadPass(fg, stagingHandle, instanceImport, region);
        uploadBytes = region.size;
    }
    dirtyFirst = 0;
    dirtyEnd = 0;
    return handle;
}

auto GpuScene::afterExecute(const FrameGraph& fg, uint64_t frame) -> void {
    instanceAccess = fg.finalAccess(instanceImport);
    if (uploadBytes == 0) {
        return;
    }
    // carried_access is what the previous frame left the buffer in: StorageRead on any
    // upload after the first, so the upload's barrier waits for those reads.
    const auto* upload = fg.passStats("InstanceUpload");
    const auto* shadow = fg.passStats("ShadowPass");
    OBS_EVENT("Render", "InstanceUpload", "instances")
        .field("frame", (int64_t) frame)
        .field("first", (int64_t) uploadFirst)
        .field("count", (int64_t) (uploadBytes / sizeof(GpuInstanceRecord)))
        .field("bytes", (int64_t) uploadBytes)
        .field("carried_access", toString(carriedAccess))
        .field("upload_barriers", upload != nullptr ? (int64_t) upload->barriers : -1)
        .field("shadow_barriers", shadow != nullptr ? (int64_t) shadow->barriers : -1);
}
