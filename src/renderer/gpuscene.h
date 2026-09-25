#pragma once

#include "framegraphresource.h"
#include "rhitypes.h"
#include "scenehandles.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <unordered_map>
#include <vector>

class DeletionQueue;
class FrameGraph;
class GpuUploader;
class MeshLibrary;
class RhiDevice;

struct GpuInstance {
    MeshHandle mesh;
    MaterialHandle material;
    uint32_t prim = 0; // PrimHandle::index of the source prim, for the render debugger
    glm::mat4 transform;
    // Submesh index range within the mesh; primFirst marks the first submesh
    // instance of a prim (the shadow pass draws the whole mesh once, there).
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    bool primFirst = true;
    bool doubleSided = false; // drawn with the cull-none pipeline
};

// Where one mesh lives in the geometry pool. Indices are mesh-local; vertexOffset rebases them.
struct GpuMeshRange {
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    uint64_t vertexBytes = 0; // Vertex plus position stream, for the render debugger
    uint64_t indexBytes = 0;
};

// The scene's GPU tables (docs/plan_gpu_driven.md): the geometry pool and the instance
// buffer, later the material table. Two upload paths, split by the data:
//   bulk  - large and rare, blocking through GpuUploader (rebuildGeometry)
//   delta - small and per frame, through per-slot staging and a graph copy pass
//           (updateInstances, then addUploadPasses)
class GpuScene {
public:
    auto init(RhiDevice* device, uint32_t frameSlots, DeletionQueue* deletionQueue) -> void;
    auto destroy() -> void;

    // Bulk: rebuilds the geometry pool from every mesh the instances reference, in
    // instance order (docs/plan_geometry_pool.md). The old pool is freed through the
    // deletion queue at `frame`.
    auto rebuildGeometry(std::span<const GpuInstance> instances, const MeshLibrary& meshLib, GpuUploader& uploader, uint64_t frame) -> void;
    auto meshRange(uint32_t meshIndex) const -> const GpuMeshRange*;
    auto meshRanges() const -> const std::unordered_map<uint32_t, GpuMeshRange>& { return meshes; }
    auto vertexBuffer() const -> RhiBuffer* { return poolVertices; }
    auto positionBuffer() const -> RhiBuffer* { return poolPositions; }
    auto indexBuffer() const -> RhiBuffer* { return poolIndices; }
    auto geometryPoolBytes() const -> uint64_t { return poolBytes; }

    // Delta: grows the instance buffer to fit `instances` and marks [dirtyFirst, dirtyEnd)
    // for upload. A grown buffer is a new buffer: instanceGeneration() changes, and every
    // instance is marked dirty.
    auto updateInstances(std::span<const GpuInstance> instances, uint32_t dirtyFirst, uint32_t dirtyEnd, uint64_t frame) -> void;
    // Imports the instance buffer and, if anything is dirty, adds the InstanceUpload pass.
    // Returns the handle the draw passes read.
    auto addUploadPasses(FrameGraph& fg, std::span<const GpuInstance> instances, uint32_t frameSlot) -> FgBufferHandle;
    // Stores the access the instance buffer is left in for the next frame's import.
    auto afterExecute(const FrameGraph& fg, uint64_t frame) -> void;
    auto instanceBuffer() const -> RhiBuffer* { return instances; }
    auto instanceGeneration() const -> uint32_t { return generation; }
    auto instanceBufferBytes() const -> uint64_t { return (uint64_t) instanceCapacity * sizeof(glm::mat4); }
    auto lastInstanceUploadBytes() const -> uint64_t { return uploadBytes; }

private:
    auto ensureInstanceCapacity(uint32_t count, uint32_t liveCount, uint64_t frame) -> void;
    auto markDirty(uint32_t first, uint32_t end) -> void;

    RhiDevice* device = nullptr;
    DeletionQueue* deletionQueue = nullptr;

    // Geometry pool
    RhiBuffer* poolVertices = nullptr;
    RhiBuffer* poolPositions = nullptr;
    RhiBuffer* poolIndices = nullptr;
    uint64_t poolBytes = 0;
    std::unordered_map<uint32_t, GpuMeshRange> meshes;

    // Instance buffer (docs/plan_frame_graph_buffers.md): one mat4 per GpuInstance, read at
    // gl_InstanceIndex. The access it was left in carries into the next frame's graph so the
    // upload syncs with the previous frame's reads.
    RhiBuffer* instances = nullptr;
    FgAccessFlags instanceAccess = FgAccessFlags::None;
    std::vector<RhiBuffer*> staging; // per frame slot, CpuToGpu, mapped
    std::vector<void*> stagingMapped;
    uint32_t instanceCapacity = 0;
    uint32_t generation = 0;
    // Dirty span [dirtyFirst, dirtyEnd); empty when equal. Accumulates until a frame uploads it.
    uint32_t dirtyFirst = 0;
    uint32_t dirtyEnd = 0;

    // This frame's upload, for RenderStats and the InstanceUpload observation.
    FgBufferHandle instanceImport;
    FgAccessFlags carriedAccess = FgAccessFlags::None;
    uint32_t uploadFirst = 0;
    uint64_t uploadBytes = 0;
};
