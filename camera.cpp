#include "camera.h"

#include <cmath>
#include <SDL3/SDL.h>

glm::vec3 Camera::forward() const {
    float yawRad = glm::radians(yaw);
    float pitchRad = glm::radians(pitch);
    return glm::normalize(glm::vec3(
        cosf(pitchRad) * cosf(yawRad),
        sinf(pitchRad),
        cosf(pitchRad) * sinf(yawRad)
    ));
}

glm::vec3 Camera::right() const {
    return glm::normalize(glm::cross(forward(), glm::vec3(0, 1, 0)));
}

void Camera::handleMouseMotion(float xrel, float yrel) {
    yaw += xrel * mouseSensitivity;
    pitch -= yrel * mouseSensitivity;
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
}

void Camera::update(const bool* keys, float dt) {
    glm::vec3 fwd = forward();
    glm::vec3 r = right();
    glm::vec3 up = glm::vec3(0, 1, 0);

    float spd = keys[SDL_SCANCODE_LSHIFT] ? speed * 2.0f : speed;
    if (keys[SDL_SCANCODE_W]) position += fwd * spd * dt;
    if (keys[SDL_SCANCODE_S]) position -= fwd * spd * dt;
    if (keys[SDL_SCANCODE_A]) position -= r * spd * dt;
    if (keys[SDL_SCANCODE_D]) position += r * spd * dt;
    if (keys[SDL_SCANCODE_E]) position += up * spd * dt;
    if (keys[SDL_SCANCODE_Q]) position -= up * spd * dt;
}

glm::mat4 Camera::viewMatrix() const {
    glm::vec3 fwd = forward();
    return glm::lookAt(position, position + fwd, glm::vec3(0, 1, 0));
}
