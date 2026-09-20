#pragma once

#include <array>
#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

enum class LightKind : uint8_t {
    Distant,
    Sphere,
    Rect,
    Disk,
    Cylinder,
    Dome,
};

struct LightDesc {
    LightKind kind = LightKind::Distant;
    glm::vec3 color = glm::vec3(1.0f);
    float intensity = 1.0f;
    float exposure = 0.0f;
    // Distant light solid angle in degrees (sun ≈ 0.53°). Unused for other kinds.
    float angle = 0.53f;
    bool shadowEnable = true;
    glm::vec3 shadowColor = glm::vec3(0.0f);
};

struct Transform {
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale = glm::vec3(1.0f);

    glm::mat4 toMat4() const {
        auto m = glm::translate(glm::mat4(1.0f), position);
        m *= glm::mat4_cast(rotation);
        m = glm::scale(m, scale);
        return m;
    }
};

struct AABB {
    glm::vec3 min = glm::vec3(0.0f);
    glm::vec3 max = glm::vec3(0.0f);

    bool valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }

    bool contains(glm::vec3 p) const { return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z; }

    AABB transformed(const glm::mat4& m) const {
        glm::vec3 corners[8] = {
            {min.x, min.y, min.z},
            {max.x, min.y, min.z},
            {min.x, max.y, min.z},
            {max.x, max.y, min.z},
            {min.x, min.y, max.z},
            {max.x, min.y, max.z},
            {min.x, max.y, max.z},
            {max.x, max.y, max.z},
        };
        AABB result = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
        for (auto& c : corners) {
            auto p = glm::vec3(m * glm::vec4(c, 1.0f));
            result.min = glm::min(result.min, p);
            result.max = glm::max(result.max, p);
        }
        return result;
    }
};

// View frustum as six planes in world space, extracted from a view-projection matrix
// (Gribb-Hartmann). A point p is inside when dot(plane.xyz, p) + plane.w >= 0 for every
// plane. Works for any clip-space convention the matrix encodes, including the flipped
// y of the Vulkan projection, since the planes come from the clip inequalities directly.
struct Frustum {
    std::array<glm::vec4, 6> planes;

    static auto fromViewProj(const glm::mat4& viewProj) -> Frustum {
        // glm is column-major: row i of the matrix is (m[0][i], m[1][i], m[2][i], m[3][i]).
        auto row = [&](int i) { return glm::vec4(viewProj[0][i], viewProj[1][i], viewProj[2][i], viewProj[3][i]); };
        auto r0 = row(0);
        auto r1 = row(1);
        auto r2 = row(2);
        auto r3 = row(3);
        Frustum f;
        f.planes[0] = r3 + r0; // left:   x >= -w
        f.planes[1] = r3 - r0; // right:  x <=  w
        f.planes[2] = r3 + r1; // bottom: y >= -w
        f.planes[3] = r3 - r1; // top:    y <=  w
        f.planes[4] = r2;      // near:   z >= 0 (Vulkan depth range)
        f.planes[5] = r3 - r2; // far:    z <= w
        for (auto& p : f.planes) {
            float len = glm::length(glm::vec3(p));
            if (len > 0.0f) {
                p /= len;
            }
        }
        return f;
    }

    // Conservative AABB test: for each plane, take the box corner furthest along the
    // plane normal; the box is outside only if that corner is behind the plane. Boxes
    // that straddle a frustum corner pass, which is fine for a draw-or-skip decision.
    auto contains(const AABB& box) const -> bool {
        for (const auto& p : planes) {
            glm::vec3 positive = {
                p.x >= 0.0f ? box.max.x : box.min.x,
                p.y >= 0.0f ? box.max.y : box.min.y,
                p.z >= 0.0f ? box.max.z : box.min.z,
            };
            if (glm::dot(glm::vec3(p), positive) + p.w < 0.0f) {
                return false;
            }
        }
        return true;
    }

    // World-space corners of the frustum the matrix describes, for drawing it. Order:
    // near plane (-x-y, +x-y, +x+y, -x+y) then far plane in the same order.
    static auto corners(const glm::mat4& viewProj) -> std::array<glm::vec3, 8> {
        auto inv = glm::inverse(viewProj);
        std::array<glm::vec3, 8> out;
        int i = 0;
        for (float z : {0.0f, 1.0f}) {
            for (auto [x, y] : {std::pair{-1.0f, -1.0f}, std::pair{1.0f, -1.0f}, std::pair{1.0f, 1.0f}, std::pair{-1.0f, 1.0f}}) {
                auto p = inv * glm::vec4(x, y, z, 1.0f);
                out[i++] = glm::vec3(p) / p.w;
            }
        }
        return out;
    }
};

struct Ray {
    glm::vec3 origin;
    glm::vec3 direction;
};

