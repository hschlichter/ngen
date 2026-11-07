#include "meshletbuild.h"

#include "meshoptimizer.h"

#include <span>

auto weldMesh(MeshPack& mesh) -> void {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return;
    }
    std::vector<uint32_t> remap(mesh.vertices.size());
    auto uniqueCount = meshopt_generateVertexRemap(remap.data(), mesh.indices.data(), mesh.indices.size(), mesh.vertices.data(), mesh.vertices.size(), sizeof(PackVertex));
    std::vector<PackVertex> welded(uniqueCount);
    meshopt_remapVertexBuffer(welded.data(), mesh.vertices.data(), mesh.vertices.size(), sizeof(PackVertex), remap.data());
    meshopt_remapIndexBuffer(mesh.indices.data(), mesh.indices.data(), mesh.indices.size(), remap.data());
    mesh.vertices = std::move(welded);
}

auto buildMeshlets(MeshPack& mesh) -> void {
    mesh.meshlets.clear();
    mesh.meshletVertices.clear();
    mesh.meshletTriangles.clear();
    const auto* positions = mesh.vertices.empty() ? nullptr : mesh.vertices.front().position.data();
    // meshopt_buildMeshlets's cone weight: how much it favours meshlets whose triangles face alike, so the normal cone culls more.
    constexpr float coneWeight = 0.25f;

    for (auto& submesh : mesh.submeshes) {
        submesh.meshletOffset = (uint32_t) mesh.meshlets.size();
        submesh.meshletCount = 0;
        if (submesh.indexCount < 3) {
            continue;
        }
        std::span<uint32_t> indices(mesh.indices.data() + submesh.indexOffset, submesh.indexCount);
        meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), mesh.vertices.size());

        auto maxMeshlets = meshopt_buildMeshletsBound(indices.size(), meshletMaxVertices, meshletMaxTriangles);
        std::vector<meshopt_Meshlet> built(maxMeshlets);
        std::vector<uint32_t> vertices(indices.size());
        std::vector<unsigned char> triangles(indices.size());
        auto count = meshopt_buildMeshlets(
            built.data(), vertices.data(), triangles.data(), indices.data(), indices.size(), positions, mesh.vertices.size(), sizeof(PackVertex), meshletMaxVertices, meshletMaxTriangles, coneWeight);

        for (size_t i = 0; i < count; i++) {
            const auto& source = built[i];
            meshopt_optimizeMeshlet(&vertices[source.vertex_offset], &triangles[source.triangle_offset], source.triangle_count, source.vertex_count);
            auto bounds = meshopt_computeMeshletBounds(
                &vertices[source.vertex_offset], &triangles[source.triangle_offset], source.triangle_count, positions, mesh.vertices.size(), sizeof(PackVertex));

            PackMeshlet meshlet;
            meshlet.vertexOffset = (uint32_t) mesh.meshletVertices.size();
            meshlet.triangleOffset = (uint32_t) mesh.meshletTriangles.size();
            meshlet.vertexCount = source.vertex_count;
            meshlet.triangleCount = source.triangle_count;
            meshlet.center = {bounds.center[0], bounds.center[1], bounds.center[2]};
            meshlet.radius = bounds.radius;
            meshlet.cone = {bounds.cone_axis_s8[0], bounds.cone_axis_s8[1], bounds.cone_axis_s8[2], bounds.cone_cutoff_s8};
            for (uint32_t v = 0; v < source.vertex_count; v++) {
                mesh.meshletVertices.push_back(vertices[source.vertex_offset + v]);
            }
            for (uint32_t t = 0; t < source.triangle_count; t++) {
                const auto* corner = &triangles[source.triangle_offset + (t * 3)];
                mesh.meshletTriangles.push_back((uint32_t) corner[0] | ((uint32_t) corner[1] << 8) | ((uint32_t) corner[2] << 16));
            }
            mesh.meshlets.push_back(meshlet);
        }
        submesh.meshletCount = (uint32_t) mesh.meshlets.size() - submesh.meshletOffset;
    }
}
