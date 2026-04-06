#pragma once

#include "scenehandles.h"
#include "types.h"

#include <vector>

struct MeshDesc {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

class MeshLibrary {
public:
    MeshHandle add(MeshDesc data) {
        m_meshes.push_back(std::move(data));
        return {.index = (uint32_t) m_meshes.size()};
    }

    const MeshDesc* get(MeshHandle h) const {
        if (h.index == 0 || h.index > m_meshes.size()) {
            return nullptr;
        }
        return &m_meshes[h.index - 1];
    }

    uint32_t count() const { return (uint32_t) m_meshes.size(); }

private:
    std::vector<MeshDesc> m_meshes;
};
