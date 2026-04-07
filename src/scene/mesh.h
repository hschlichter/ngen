#pragma once

#include "scenehandles.h"
#include "scenetypes.h"
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

    AABB bounds(MeshHandle h) const {
        const auto* m = get(h);
        if (!m || m->vertices.empty()) {
            return {};
        }
        AABB b = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
        for (const auto& v : m->vertices) {
            auto p = glm::vec3(v.position[0], v.position[1], v.position[2]);
            b.min = glm::min(b.min, p);
            b.max = glm::max(b.max, p);
        }
        return b;
    }

    uint32_t count() const { return (uint32_t) m_meshes.size(); }

private:
    std::vector<MeshDesc> m_meshes;
};
