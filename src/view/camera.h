#pragma once

#include "scenetypes.h" // AABB

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <optional>

// The view's vertical field of view; the projection is the renderer's (projection.h).
inline constexpr float cameraFovDegrees = 45.0f;

// What the controls ask of the camera this frame, each -1 to 1: along its forward, its right, and worldUp; `fast` moves five times as
// fast. The application maps its input (keys) onto it.
struct CameraMove {
    float forward = 0.0f;
    float right = 0.0f;
    float up = 0.0f;
    bool fast = false;
};

struct Camera {
    glm::vec3 position;
    glm::vec3 worldUp = glm::vec3(0.0f, 1.0f, 0.0f); // set from the stage's up axis
    float yaw;                                       // degrees, azimuth around worldUp
    float pitch;                                     // degrees, elevation toward worldUp
    float speed;
    float mouseSensitivity;
    // Following the ground: the eye's height above it in world units. While set, update() moves across the ground by yaw only and Q/E
    // change the height; the view sets the eye's height from the ground under it.
    std::optional<float> followHeight;

    [[nodiscard]] auto forward() const -> glm::vec3;
    [[nodiscard]] auto right() const -> glm::vec3;
    auto handleMouseMotion(float xrel, float yrel) -> void;
    auto update(const CameraMove& move, float dt) -> void;
    [[nodiscard]] auto viewMatrix() const -> glm::mat4;
    auto snapToAxis(int axis, bool negative) -> void;

    // Move the camera so `bounds` fills the view, keeping current yaw/pitch and fly speed.
    // Distance is sized to the bounds' diagonal radius and the given vertical FOV.
    auto frame(const AABB& bounds, float fovRadians) -> void;
    // Scale the fly speed by 1.25 per step (negative steps slow it down), 0.1 to 5000 m/s.
    auto scaleSpeed(float steps) -> void;
    // Move the camera to where a camera transform is, looking down its local -Z as USD's cameras do. Roll is dropped (the view's up stays
    // worldUp), and pitch is clamped as mouse look clamps it.
    auto place(const glm::mat4& cameraTransform) -> void;
    // Turn to look along a direction, keeping the position; roll dropped and pitch clamped as in place().
    auto lookAlong(glm::vec3 look) -> void;
    // Turn around a pivot by angles, as mouse look turns in place: yaw around worldUp through the pivot, pitch around the camera's right
    // (clamped as mouse look clamps it). The distance to the pivot is kept, and the pivot stays where it was in the view.
    auto orbit(glm::vec3 pivot, float yawDegrees, float pitchDegrees) -> void;
    // Move toward the pivot (positive steps) or away from it, each step 15% of the distance, never through it.
    auto dolly(glm::vec3 pivot, float steps) -> void;
};
