#pragma once

#include <array>

struct Vertex {
    std::array<float, 3> position;
    std::array<float, 3> normal;
    std::array<float, 3> color;
    std::array<float, 2> texCoord;
};
