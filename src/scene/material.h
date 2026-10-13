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
    // Cutout: drawn only where the opacity reaches opacityThreshold; 0 is opaque. The opacity is a channel of the opacity texture (packed,
    // with its levels as texPixels are), of the base colour texture when opacityFromBaseColor, or the constant when there is neither.
    float opacityThreshold = 0.0f;
    float opacity = 1.0f;
    uint32_t opacityChannel = 0; // 0 r, 1 g, 2 b, 3 a
    bool opacityRaw = false;     // authored raw: the sampler's sRGB decode is undone before the comparison
    bool opacityFromBaseColor = false;
    int opacityWidth = 0;
    int opacityHeight = 0;
    std::vector<uint8_t> opacityPixels;
    uint32_t opacityMipLevels = 0;

    auto cutout() const -> bool { return opacityThreshold > 0.0f; }
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
