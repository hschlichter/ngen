#pragma once

#include "scenetypes.h" // AABB

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

struct Camera {
    glm::vec3 position;
    glm::vec3 worldUp = glm::vec3(0.0f, 1.0f, 0.0f); // set from the stage's up axis
    float yaw;                                       // degrees, azimuth around worldUp
    float pitch;                                     // degrees, elevation toward worldUp
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
