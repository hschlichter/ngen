#pragma once

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

struct Ray {
    glm::vec3 origin;
    glm::vec3 direction;
};

struct Frustum {
    glm::vec4 planes[6];
};
