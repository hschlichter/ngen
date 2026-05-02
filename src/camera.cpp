#include "camera.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>

// Pick a stable horizontal axis perpendicular to worldUp for yaw=0.
static auto horizontalReference(glm::vec3 worldUp) -> glm::vec3 {
    // When worldUp ≈ ±X, fall back to Y; otherwise use X.
    return std::abs(worldUp.x) > 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
}

auto Camera::forward() const -> glm::vec3 {
    auto yawRad = glm::radians(yaw);
    auto pitchRad = glm::radians(pitch);
    auto axisA = horizontalReference(worldUp);
    // axisA × worldUp (not worldUp × axisA) so yaw increases clockwise viewed from +worldUp,
    // matching the original Y-up behavior (yaw=90° → +Z).
    auto axisB = glm::normalize(glm::cross(axisA, worldUp));
    return glm::normalize(axisA * (std::cos(pitchRad) * std::cos(yawRad)) + axisB * (std::cos(pitchRad) * std::sin(yawRad)) + worldUp * std::sin(pitchRad));
}

auto Camera::right() const -> glm::vec3 {
    return glm::normalize(glm::cross(forward(), worldUp));
}

auto Camera::handleMouseMotion(float xrel, float yrel) -> void {
    yaw += xrel * mouseSensitivity;
    pitch -= yrel * mouseSensitivity;
    pitch = std::min(pitch, 89.0f);
    pitch = std::max(pitch, -89.0f);
}

auto Camera::update(const bool* keys, float dt) -> void {
    // Reserve Ctrl-modified keystrokes for editor shortcuts (Ctrl+E, etc.)
    // so they don't double as camera movement.
    if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]) {
        return;
    }

    auto fwd = forward();
    auto r = right();
    auto up = worldUp;

    auto spd = keys[SDL_SCANCODE_LSHIFT] ? speed * 5.0f : speed;
    if (keys[SDL_SCANCODE_W]) {
        position += fwd * spd * dt;
    }
    if (keys[SDL_SCANCODE_S]) {
        position -= fwd * spd * dt;
    }
    if (keys[SDL_SCANCODE_A]) {
        position -= r * spd * dt;
    }
    if (keys[SDL_SCANCODE_D]) {
        position += r * spd * dt;
    }
    if (keys[SDL_SCANCODE_E]) {
        position += up * spd * dt;
    }
    if (keys[SDL_SCANCODE_Q]) {
        position -= up * spd * dt;
    }
}

auto Camera::snapToAxis(int axis, bool negative) -> void {
    switch (axis) {
        case 0:
            yaw = negative ? 180.0f : 0.0f;
            pitch = 0.0f;
            break;
        case 1:
            pitch = negative ? -89.0f : 89.0f;
            break;
        case 2:
            yaw = negative ? -90.0f : 90.0f;
            pitch = 0.0f;
            break;
    }
}

auto Camera::viewMatrix() const -> glm::mat4 {
    auto fwd = forward();
    return glm::lookAt(position, position + fwd, worldUp);
}

auto Camera::frame(const AABB& bounds, float fovRadians) -> void {
    if (!bounds.valid()) {
        return;
    }
    auto center = (bounds.min + bounds.max) * 0.5f;
    auto radius = glm::length(bounds.max - bounds.min) * 0.5f;
    // Distance such that the bounding sphere fits in the vertical FOV, with a
    // small padding so the object isn't right against the screen edge.
    auto dist = std::max(radius / std::sin(fovRadians * 0.5f), 0.01f) * 1.2f;
    position = center - forward() * dist;
}
