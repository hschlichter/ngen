#include "ground.h"

#include "camerawindow.h"
#include "debugdraw.h"

#include <cmath>

auto GroundMeshes::apply(const SceneChanges& changes) -> void {
    for (const auto& [handle, desc] : changes.meshesAdded) {
        if (handle.index >= meshes.size()) {
            meshes.resize(handle.index + 1);
        }
        meshes[handle.index] = desc;
    }
    for (auto handle : changes.meshesRemoved) {
        if (handle.index < meshes.size()) {
            meshes[handle.index].reset();
        }
    }
}

auto GroundMeshes::get(MeshHandle handle) const -> const MeshDesc* {
    if (handle.index >= meshes.size()) {
        return nullptr;
    }
    return meshes[handle.index].get();
}

namespace {

// Möller–Trumbore: the distance along `dir` from `origin` to the triangle, either face; none when the ray misses it or it lies behind.
auto rayTriangle(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) -> std::optional<float> {
    auto ab = b - a;
    auto ac = c - a;
    auto p = glm::cross(dir, ac);
    auto det = glm::dot(ab, p);
    if (std::abs(det) < 1e-12f) {
        return std::nullopt;
    }
    auto inverse = 1.0f / det;
    auto toOrigin = origin - a;
    auto u = glm::dot(toOrigin, p) * inverse;
    if (u < 0.0f || u > 1.0f) {
        return std::nullopt;
    }
    auto q = glm::cross(toOrigin, ab);
    auto v = glm::dot(dir, q) * inverse;
    if (v < 0.0f || u + v > 1.0f) {
        return std::nullopt;
    }
    auto t = glm::dot(ac, q) * inverse;
    if (t < 0.0f) {
        return std::nullopt;
    }
    return t;
}

auto position(const Vertex& vertex) -> glm::vec3 {
    return {vertex.position[0], vertex.position[1], vertex.position[2]};
}

} // namespace

auto groundBelow(const RenderWorld& world, const SceneBoundsCache& chunkBounds, const GroundMeshes& meshes, const glm::vec3& origin, const glm::vec3& up, GroundTestCounts& counts)
    -> std::optional<GroundHit> {
    counts = {};
    // The stage's up is an axis: the bounds test is on the other two.
    int upAxis = 0;
    for (int axis = 1; axis < 3; axis++) {
        if (std::abs(up[axis]) > std::abs(up[upAxis])) {
            upAxis = axis;
        }
    }
    int across0 = (upAxis + 1) % 3;
    int across1 = (upAxis + 2) % 3;
    auto holds = [&](const AABB& bounds) {
        return bounds.valid() && origin[across0] >= bounds.min[across0] && origin[across0] <= bounds.max[across0] && origin[across1] >= bounds.min[across1] &&
               origin[across1] <= bounds.max[across1];
    };
    std::optional<GroundHit> best;
    float bestHeight = 0.0f;
    auto chunkCount = std::min(world.meshInstances.chunkCount(), chunkBounds.chunkCount());
    for (size_t c = 0; c < chunkCount; c++) {
        if (!holds(chunkBounds.chunkBounds(c))) {
            continue;
        }
        counts.chunks++;
        for (const auto& draw : world.meshInstances.chunk(c)) {
            if (!draw.alive || !holds(draw.worldBounds)) {
                continue;
            }
            const auto* mesh = meshes.get(draw.mesh);
            if (mesh == nullptr) {
                continue;
            }
            counts.draws++;
            // The ray in the mesh's space; t stays a distance along it in world units only up to the transform's scale, so the hit is taken
            // back to world space to compare.
            auto toLocal = glm::inverse(draw.worldTransform);
            auto localOrigin = glm::vec3(toLocal * glm::vec4(origin, 1.0f));
            auto localDir = glm::vec3(toLocal * glm::vec4(-up, 0.0f));
            auto end = std::min<size_t>(draw.indexOffset + draw.indexCount, mesh->indices.size());
            for (size_t i = draw.indexOffset; i + 2 < end; i += 3) {
                counts.triangles++;
                auto t = rayTriangle(localOrigin,
                                     localDir,
                                     position(mesh->vertices[mesh->indices[i]]),
                                     position(mesh->vertices[mesh->indices[i + 1]]),
                                     position(mesh->vertices[mesh->indices[i + 2]]));
                if (!t) {
                    continue;
                }
                auto point = glm::vec3(draw.worldTransform * glm::vec4(localOrigin + (localDir * *t), 1.0f));
                auto height = glm::dot(point, up);
                if (!best || height > bestHeight) {
                    best = GroundHit{.point = point, .prim = draw.prim};
                    bestHeight = height;
                }
            }
        }
    }
    return best;
}

auto drawCameraModes(const CameraModesState& modes, const glm::vec3& eye, double metersPerUnit, DebugDraw& draw) -> void {
    auto unitsPerMetre = (float) (1.0 / metersPerUnit);
    const glm::vec4 color = {0.3f, 0.9f, 1.0f, 1.0f};
    auto cross = [&](const glm::vec3& at, float halfMetres) {
        auto half = halfMetres * unitsPerMetre;
        draw.line(at - glm::vec3(half, 0.0f, 0.0f), at + glm::vec3(half, 0.0f, 0.0f), color);
        draw.line(at - glm::vec3(0.0f, half, 0.0f), at + glm::vec3(0.0f, half, 0.0f), color);
        draw.line(at - glm::vec3(0.0f, 0.0f, half), at + glm::vec3(0.0f, 0.0f, half), color);
    };
    if (modes.orbiting) {
        cross(modes.pivot, 0.5f);
    }
    if (viewconfig::follow.get() && modes.ground) {
        draw.line(eye, *modes.ground, color);
        cross(*modes.ground, 0.25f);
    }
}

namespace viewconfig {

config::Bool follow{"view.camera.follow", false, "Keep the eye of the camera that has the controls a height above the highest drawn surface under it (G)."};
config::Float followHeight{"view.camera.followHeight", 1.8, "Metres the eye is kept above the ground while following it; Q/E change it.", {.min = 0.0, .max = 1000.0}};

} // namespace viewconfig
