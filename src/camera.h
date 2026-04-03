#pragma once

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

struct Camera {
    glm::vec3 position;
    float yaw;   // degrees
    float pitch; // degrees
    float speed;
    float mouseSensitivity;

    [[nodiscard]] [[nodiscard]] auto forward() const -> glm::vec3;
    [[nodiscard]] [[nodiscard]] auto right() const -> glm::vec3;
    auto handleMouseMotion(float xrel, float yrel) -> void;
    auto update(const bool* keys, float dt) -> void;
    [[nodiscard]] [[nodiscard]] auto viewMatrix() const -> glm::mat4;
};
