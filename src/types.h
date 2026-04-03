#pragma once

#include <array>
#include <cstdint>
#include <vector>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

struct Vertex {
    std::array<float, 3> position;
    std::array<float, 3> normal;
    std::array<float, 3> color;
    std::array<float, 2> texCoord;
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
