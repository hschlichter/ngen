#pragma once

#include "scenehandles.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

struct MaterialDesc {
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    int texWidth = 0;
    int texHeight = 0;
    // RGBA8 sRGB. A packed texture's levels, level 0 first, when texMipLevels > 0 (the renderer uploads them as they are);
    // otherwise only level 0, and the renderer builds the mip chain (the generated 1x1 textures).
    std::vector<uint8_t> texPixels;
    uint32_t texMipLevels = 0;
};

class MaterialLibrary {
public:
    MaterialHandle add(MaterialDesc desc) {
        m_materials.push_back(std::move(desc));
        return {.index = (uint32_t) m_materials.size()};
    }

    const MaterialDesc* get(MaterialHandle h) const {
        if (h.index == 0 || h.index > m_materials.size()) {
            return nullptr;
        }
        return &m_materials[h.index - 1];
    }

    uint32_t count() const { return (uint32_t) m_materials.size(); }

private:
    std::vector<MaterialDesc> m_materials;
};
