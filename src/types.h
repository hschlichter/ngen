#pragma once

#include <cstdint>
#include <vector>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

struct Vertex {
    float position[3];
    float normal[3];
    float color[3];
    float texCoord[2];
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    int texWidth = 0, texHeight = 0;
    std::vector<uint8_t> texPixels; // RGBA
    glm::mat4 transform = glm::mat4(1.0f);
};

struct Scene {
    std::vector<MeshData> meshes;
};
