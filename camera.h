#pragma once

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

struct Camera {
    glm::vec3 position;
    float yaw;              // degrees
    float pitch;            // degrees
    float speed;
    float mouseSensitivity;

    glm::vec3 forward() const;
    glm::vec3 right() const;
    void handleMouseMotion(float xrel, float yrel);
    void update(const bool* keys, float dt);
    glm::mat4 viewMatrix() const;
};
