#pragma once

#include "assetlibrary.h"
#include "scenehandles.h"
#include "scenetypes.h"
#include "types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// A contiguous range of the mesh's index buffer that shares one material, and the meshlets built from it.
// Meshes with USD materialBind GeomSubsets produce one SubMesh per subset;
// meshes with a single (or no) binding produce a single SubMesh covering the
// whole index buffer. Always at least one entry when the mesh has geometry.
// The material is the draw's, not the mesh's: a mesh shared by several prims
// is drawn with each prim's bindings.
struct SubMesh {
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    uint32_t meshletOffset = 0;
    uint32_t meshletCount = 0;
    MaterialHandle material;
    AABB bounds; // its triangles' vertices, in the mesh's space (subsetBounds)
};

// A piece of a submesh drawn and culled as one (at most 64 vertices and 124 triangles, the mesh pack's meshlet): its vertices are
// meshletVertices[vertexOffset ..) (mesh vertex indices), its triangles meshletTriangles[triangleOffset ..) (three meshlet-local indices in
// the low three bytes of each word); a bounding sphere and a normal cone (axis and cos(half angle), as value * 127) in the mesh's space.
struct MeshletDesc {
    uint32_t vertexOffset = 0;
    uint32_t triangleOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
    std::array<float, 3> center = {0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    std::array<int8_t, 4> cone = {0, 0, 0, 0};
};

struct MeshDesc {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SubMesh> submeshes;
    std::vector<MeshletDesc> meshlets;
    std::vector<uint32_t> meshletVertices;
    std::vector<uint32_t> meshletTriangles;
    bool doubleSided = false; // USD doubleSided: draw without back-face culling
};

// The bounds of the vertices a submesh's triangles use, in the mesh's space: each draw of the mesh is one submesh, culled by this box. A
// submesh without triangles gets the empty box at the origin (it draws nothing).
inline auto subsetBounds(const MeshDesc& mesh, const SubMesh& submesh) -> AABB {
    AABB b = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
    auto end = std::min<size_t>((size_t) submesh.indexOffset + submesh.indexCount, mesh.indices.size());
    for (size_t i = submesh.indexOffset; i < end; i++) {
        auto index = mesh.indices[i];
        if (index >= mesh.vertices.size()) {
            continue;
        }
        const auto& v = mesh.vertices[index];
        auto p = glm::vec3(v.position[0], v.position[1], v.position[2]);
        b.min = glm::min(b.min, p);
        b.max = glm::max(b.max, p);
    }
    return b.valid() ? b : AABB{};
}

// Meshes by mesh asset id: one per mesh pack, shared by every prim that places it.
using MeshLibrary = AssetLibrary<MeshHandle, MeshDesc>;
