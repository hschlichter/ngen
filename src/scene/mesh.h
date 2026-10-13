#pragma once

#include "assetlibrary.h"
#include "scenehandles.h"
#include "scenetypes.h"
#include "types.h"

#include <vector>

// A contiguous range of the mesh's index buffer that shares one material.
// Meshes with USD materialBind GeomSubsets produce one SubMesh per subset;
// meshes with a single (or no) binding produce a single SubMesh covering the
// whole index buffer. Always at least one entry when the mesh has geometry.
// The material is the draw's, not the mesh's: a mesh shared by several prims
// is drawn with each prim's bindings.
struct SubMesh {
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    MaterialHandle material;
};

struct MeshDesc {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SubMesh> submeshes;
    bool doubleSided = false; // USD doubleSided: draw without back-face culling
};

// The bounds of a mesh's vertices; invalid for a mesh without any.
inline auto meshBounds(const MeshDesc& mesh) -> AABB {
    if (mesh.vertices.empty()) {
        return {};
    }
    AABB b = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
    for (const auto& v : mesh.vertices) {
        auto p = glm::vec3(v.position[0], v.position[1], v.position[2]);
        b.min = glm::min(b.min, p);
        b.max = glm::max(b.max, p);
    }
    return b;
}

// Meshes by mesh asset id: one per mesh pack, shared by every prim that places it.
using MeshLibrary = AssetLibrary<MeshHandle, MeshDesc>;
