#pragma once

#include "scenetypes.h" // AABB

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// The view's projection: reversed-Z (depth 1 at the near plane, falling to 0 at infinity) with no far plane, and the Vulkan y flip. Depth
// is a 32-bit float, where reversed-Z keeps precision even far away, so nothing is ever clipped at a distance.
inline constexpr float cameraNear = 0.1f;
inline constexpr float cameraFovDegrees = 45.0f;

auto cameraProjection(float aspect) -> glm::mat4;
// The depth (0..1] at which a point `distance` in front of the camera lands.
auto cameraDepthAt(float distance) -> float;

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
    // The fly speed follows: a tenth of the distance per second, 1 to 500 m/s.
    auto frame(const AABB& bounds, float fovRadians) -> void;
};
