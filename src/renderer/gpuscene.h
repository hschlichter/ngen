#pragma once

#include "framegraphresource.h"
#include "material.h"
#include "mesh.h"
#include "rhitypes.h"
#include "scenehandles.h"
#include "scenetypes.h"

#include <cstdint>
#include <deque>
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <vector>

class DeletionQueue;
class FrameGraph;
class GpuUploader;
class RhiCommandBuffer;
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
    bool doubleSided = false;   // drawn with the cull-none pipeline
    bool cutout = false;        // its material is cutout: drawn by the cutout pipelines
    bool primHasCutout = false; // some submesh of its prim is cutout: the shadow pass draws the prim per submesh
    uint32_t lodLevel = 0;      // the drawn level + 1 of the LOD root above it, 0 under none (RenderMeshInstance::lodLevel)
    AABB worldBounds;           // culling input; invalid bounds are always visible
    bool alive = false;         // a dead slot is never drawn
};

// Flags in GpuInstanceRecord::flags.
inline constexpr uint32_t gpuInstancePrimFirst = 1u << 0;     // drawn by the shadow pass (whole mesh)
inline constexpr uint32_t gpuInstanceDoubleSided = 1u << 1;   // cull-none pipeline bucket
inline constexpr uint32_t gpuInstanceBoundsValid = 1u << 2;   // otherwise never culled
inline constexpr uint32_t gpuInstanceCutout = 1u << 3;        // cutout pipeline bucket
inline constexpr uint32_t gpuInstancePrimHasCutout = 1u << 4; // shadows per submesh, not whole mesh

// One entry of the GPU instance buffer (std430, stride 112), read by the vertex shaders at
// gl_InstanceIndex and by the culling passes. material indexes
// the material table, mesh the mesh table; indexOffset/indexCount are the submesh range.
struct GpuInstanceRecord {
    glm::mat4 model;
    uint32_t material = 0;
    uint32_t mesh = 0;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    glm::vec3 boundsMin = glm::vec3(0.0f);
    uint32_t flags = 0;
    glm::vec3 boundsMax = glm::vec3(0.0f);
    uint32_t lodLevel = 0; // GpuInstance::lodLevel, for the LOD level view
};
static_assert(sizeof(GpuInstanceRecord) == 112);

// One entry of the GPU mesh table (std430, stride 16), indexed by MeshHandle::index. A mesh
// outside the pool has indexCount 0 and is never drawn.
struct GpuMeshEntry {
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
    uint32_t indexCount = 0;
    uint32_t pad = 0;
};
static_assert(sizeof(GpuMeshEntry) == 16);

// One entry of the GPU material table (std430, stride 16). baseColorTexture is a slot in
// the geometry descriptor set's texture array; slot 0 is the fallback texture. A cutout
// material (opacityThreshold > 0) is drawn where its opacity reaches the threshold
// (shaders/cutout.glsl): the opacity texture's channel (opacityInfo bits 0-1; bit 2 when the
// texture is raw), or with opacity texture 0 cut away entirely.
struct GpuMaterial {
    uint32_t baseColorTexture = 0;
    uint32_t opacityTexture = 0;
    uint32_t opacityInfo = 0;
    float opacityThreshold = 0.0f;
};
static_assert(sizeof(GpuMaterial) == 16);

// A texture on the GPU, keyed by TextureHandle::index.
struct GpuTextureInfo {
    RhiTexture* texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    uint64_t bytes = 0; // all levels
    RhiFormat format = RhiFormat::Undefined;
    uint32_t slot = 0; // in the texture array once every level has been copied; 0 until then
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

// Ranges in a buffer measured in elements: first fit from a free list, neighbouring free ranges merged.
class PoolAllocator {
public:
    // The first element of a range of `count`; nullopt when the capacity has no room.
    auto allocate(uint64_t count) -> std::optional<uint64_t>;
    auto free(uint64_t first, uint64_t count) -> void;
    // Adds [old capacity, capacity) to the free list.
    auto grow(uint64_t capacity) -> void;
    auto capacity() const -> uint64_t { return total; }
    auto used() const -> uint64_t { return inUse; }

private:
    std::map<uint64_t, uint64_t> freeRanges; // first element to count
    uint64_t total = 0;
    uint64_t inUse = 0;
};

// The scene's GPU tables (src/renderer/README.md), changed incrementally:
//   - the geometry pool: a range per mesh, allocated and freed as meshes come and go, grown by copy;
//   - the mesh table, indexed by MeshHandle::index, for the culling passes;
//   - textures, one per TextureHandle, with a slot in the texture array once uploaded;
//   - the material table, an entry per MaterialHandle;
//   - the instance buffer, a record per draw slot.
// Changes queue uploads; recordUploads copies them in the frame's command buffer, within a byte budget, with barriers against the
// frames before (on one queue, a barrier covers earlier submissions), so nothing waits. flushUploads does the same on the uploader's
// command buffer and waits, for start-up.
class GpuScene {
public:
    static constexpr uint32_t maxTextures = 1024;
    // Slot 0 is the fallback texture (no material, a slot past the limit); slot 1 a white 1x1, sampled while a texture is still uploading.
    static constexpr uint32_t fallbackSlot = 0;
    static constexpr uint32_t pendingSlot = 1;
    static constexpr uint64_t defaultUploadBudget = 64ull * 1024 * 1024;

    auto init(RhiDevice* device, uint32_t frameSlots, DeletionQueue* deletionQueue, RhiTexture* fallback, RhiTexture* pending) -> void;
    auto destroy() -> void;

    auto addMesh(MeshHandle handle, std::shared_ptr<const MeshDesc> mesh) -> void;
    auto removeMesh(MeshHandle handle) -> void;
    auto addTexture(TextureHandle handle, std::shared_ptr<const TextureDesc> texture, uint64_t frame) -> void;
    auto removeTexture(TextureHandle handle, uint64_t frame) -> void;
    auto setMaterial(MaterialHandle handle, const MaterialDesc& material) -> void;
    auto removeMaterial(MaterialHandle handle) -> void;

    // Once per frame, before the frame graph is built: textures whose last level the previous frames copied take their slots, and the
    // materials using them are patched. True when the texture slots or any table buffer changed, so the descriptor sets naming them are
    // rewritten.
    auto beginFrame(uint64_t frame) -> bool;
    // Adds the SceneUpload pass when uploads are queued.
    auto addUploadPass(FrameGraph& fg, uint64_t frame) -> void;
    // Records every queued upload on the uploader's command buffer and waits. Start-up only.
    auto flushUploads(GpuUploader& uploader, uint64_t frame) -> void;
    auto pendingUploads() const -> size_t { return queue.size(); }
    // Nothing queued, no texture waiting for its slot, no grown buffer waiting for its copy: what was applied is on the GPU.
    auto idle() const -> bool;
    auto pendingBytes() const -> uint64_t;
    auto setUploadBudget(uint64_t bytes) -> void { uploadBudget = bytes; }

    auto meshRange(uint32_t meshIndex) const -> const GpuMeshRange*;
    auto meshRanges() const -> const std::unordered_map<uint32_t, GpuMeshRange>& { return meshes; }
    auto vertexBuffer() const -> RhiBuffer* { return vertices.buffer; }
    auto positionBuffer() const -> RhiBuffer* { return positions.buffer; }
    auto indexBuffer() const -> RhiBuffer* { return indices.buffer; }
    auto meshTableBuffer() const -> RhiBuffer* { return meshTable.buffer; }
    auto meshTableBytes() const -> uint64_t { return meshTable.bytes(); }
    auto poolVertexCapacity() const -> uint64_t { return vertexRanges.capacity(); }
    auto poolVertexUsed() const -> uint64_t { return vertexRanges.used(); }

    auto textures() const -> const std::unordered_map<uint32_t, GpuTextureInfo>& { return gpuTextures; }
    auto textureSlots() const -> std::span<RhiTexture* const> { return slots; }
    auto materialBuffer() const -> RhiBuffer* { return materialTable.buffer; }
    auto materialTableBytes() const -> uint64_t { return materialTable.bytes(); }
    auto materialCount() const -> uint32_t { return (uint32_t) materialEntries.size(); }
    // The material table entry of a material; 0 (no material) when it has none.
    auto materialEntry(MaterialHandle handle) const -> uint32_t;
    // For the render debugger: the materials naming a texture, and counts over the materials.
    auto materialsUsingTexture(uint32_t textureIndex) const -> std::vector<uint32_t>;
    auto texturedMaterialCount() const -> uint32_t; // base colour from a texture bigger than 1x1
    auto cutoutMaterialCount() const -> uint32_t;

    // The instance buffer: a record per draw slot. updateInstances grows it to `slotCount` and marks the changed slots for upload; a grown
    // buffer is a new one (instanceGeneration() changes) and every slot is uploaded.
    auto updateInstances(std::span<const GpuInstance> instances, std::span<const uint32_t> changedSlots, uint64_t frame) -> void;
    // Imports the instance buffer and, if anything is dirty, adds the InstanceUpload pass. Returns the handle the draw passes read.
    auto addInstanceUploadPasses(FrameGraph& fg, std::span<const GpuInstance> instances, uint32_t frameSlot) -> FgBufferHandle;
    // Stores the access the instance buffer is left in for the next frame's import.
    auto afterExecute(const FrameGraph& fg) -> void;
    auto instanceBuffer() const -> RhiBuffer* { return instances; }
    auto instanceGeneration() const -> uint32_t { return generation; }
    auto instanceBufferBytes() const -> uint64_t { return (uint64_t) instanceCapacity * sizeof(GpuInstanceRecord); }

private:
    // A GPU buffer that grows by copy: a bigger one replaces it, and the copy from the old one is recorded before this frame's uploads.
    struct GrowableBuffer {
        RhiBuffer* buffer = nullptr;
        uint64_t capacity = 0; // bytes
        RhiBufferUsageFlags usage = {};
        RhiBufferState readState = RhiBufferState::StorageRead;
        const char* name = "";
        bool zeroed = false; // a table: the part past the old contents is zeroed when it grows
        // Waiting to be copied from, at the next record; the deletion queue takes it after. moveBytes is the old contents' size.
        RhiBuffer* moveFrom = nullptr;
        uint64_t moveBytes = 0;
        bool fresh = false; // created since the last record: nothing has written it yet

        auto bytes() const -> uint64_t { return capacity; }
    };
    struct Upload {
        enum class Kind : uint8_t {
            MeshVertices,
            MeshPositions,
            MeshIndices,
            MeshEntry,
            MaterialEntry,
            TextureLevel,
        };
        Kind kind = Kind::MeshVertices;
        uint32_t handle = 0; // the mesh, material or texture
        uint32_t level = 0;
        uint64_t bytes = 0;
    };

    auto ensureCapacity(GrowableBuffer& target, uint64_t bytes) -> bool;
    auto growPool(uint64_t vertexCount, uint64_t indexCount) -> void;
    auto materialRecord(MaterialHandle handle) const -> GpuMaterial;
    auto queueMaterialEntry(uint32_t materialIndex) -> void;
    // Records queued uploads, at most `budget` bytes (and at least one upload); staging goes to `retire`.
    template <typename Retire>
    auto record(RhiCommandBuffer* cmd, uint64_t budget, const Retire& retire) -> void;
    auto ensureInstanceCapacity(uint32_t count, uint32_t liveCount, uint64_t frame) -> void;
    auto markDirty(uint32_t first, uint32_t end) -> void;

    RhiDevice* device = nullptr;
    DeletionQueue* deletionQueue = nullptr;
    uint64_t currentFrame = 0;
    uint64_t uploadBudget = defaultUploadBudget;
    bool descriptorsDirty = true;

    // Geometry pool and mesh table
    GrowableBuffer vertices;
    GrowableBuffer positions;
    GrowableBuffer indices;
    GrowableBuffer meshTable;
    PoolAllocator vertexRanges;
    PoolAllocator indexRanges;
    std::unordered_map<uint32_t, GpuMeshRange> meshes;
    std::unordered_map<uint32_t, std::shared_ptr<const MeshDesc>> meshData; // kept while its uploads are queued

    // Textures and slots
    RhiTexture* fallbackTexture = nullptr;
    RhiTexture* pendingTexture = nullptr;
    std::unordered_map<uint32_t, GpuTextureInfo> gpuTextures;
    std::unordered_map<uint32_t, std::shared_ptr<const TextureDesc>> textureData; // kept while its levels are queued
    std::unordered_map<uint32_t, uint32_t> levelsLeft;                            // by texture: levels not copied yet
    std::vector<uint32_t> texturesCopied;                                         // last level recorded; slots assigned at beginFrame
    std::vector<RhiTexture*> slots;
    std::set<uint32_t> freeTextureSlots;
    uint32_t slotOverflow = 0;

    // Material table
    GrowableBuffer materialTable;
    std::unordered_map<uint32_t, MaterialDesc> materialEntries; // by material handle
    std::unordered_map<uint32_t, uint32_t> entryOf;             // material handle to its table entry
    std::set<uint32_t> freeEntries;
    uint32_t entryCount = 1;                                         // entry 0 is "no material"
    std::unordered_map<uint32_t, std::set<uint32_t>> materialsUsing; // texture handle to the materials naming it

    std::deque<Upload> queue;

    // Instance buffer: one GpuInstanceRecord per draw slot, read at gl_InstanceIndex. The access it was left in carries into the next
    // frame's graph so the upload syncs with the previous frame's reads.
    RhiBuffer* instances = nullptr;
    FgAccessFlags instanceAccess = FgAccessFlags::None;
    std::vector<RhiBuffer*> staging; // per frame slot, CpuToGpu, mapped
    std::vector<void*> stagingMapped;
    uint32_t instanceCapacity = 0;
    uint32_t generation = 0;
    // Dirty span [dirtyFirst, dirtyEnd); empty when equal. Accumulates until a frame uploads it.
    uint32_t dirtyFirst = 0;
    uint32_t dirtyEnd = 0;

    // This frame's import of the instance buffer, whose final access afterExecute keeps; none in a frame without the lit path.
    FgBufferHandle instanceImport;
    bool instanceImported = false;
};
