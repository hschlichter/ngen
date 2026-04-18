#pragma once

#include "scenetypes.h" // AABB

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

struct Camera {
    glm::vec3 position;
    float yaw;   // degrees
    float pitch; // degrees
    float speed;
    float mouseSensitivity;

    [[nodiscard]] auto forward() const -> glm::vec3;
    [[nodiscard]] auto right() const -> glm::vec3;
    auto handleMouseMotion(float xrel, float yrel) -> void;
    auto update(const bool* keys, float dt) -> void;
    [[nodiscard]] auto viewMatrix() const -> glm::mat4;
    auto snapToAxis(int axis, bool negative) -> void;

    // Move the camera so `bounds` fills the view, keeping current yaw/pitch.
    // Distance is sized to the bounds' diagonal radius and the given vertical FOV.
    auto frame(const AABB& bounds, float fovRadians) -> void;
};
