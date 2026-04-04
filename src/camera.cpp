#include "camera.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>

auto Camera::forward() const -> glm::vec3 {
    auto yawRad = glm::radians(yaw);
    auto pitchRad = glm::radians(pitch);
    return glm::normalize(glm::vec3(cosf(pitchRad) * cosf(yawRad), sinf(pitchRad), cosf(pitchRad) * sinf(yawRad)));
}

auto Camera::right() const -> glm::vec3 {
    return glm::normalize(glm::cross(forward(), glm::vec3(0, 1, 0)));
}

auto Camera::handleMouseMotion(float xrel, float yrel) -> void {
    yaw += xrel * mouseSensitivity;
    pitch -= yrel * mouseSensitivity;
    pitch = std::min(pitch, 89.0f);
    pitch = std::max(pitch, -89.0f);
}

auto Camera::update(const bool* keys, float dt) -> void {
    auto fwd = forward();
    auto r = right();
    auto up = glm::vec3(0, 1, 0);

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

auto Camera::viewMatrix() const -> glm::mat4 {
    auto fwd = forward();
    return glm::lookAt(position, position + fwd, glm::vec3(0, 1, 0));
}
