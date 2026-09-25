#include "gpuschema.h"

#include "drawlists.h"
#include "gpuscene.h"
#include "renderertypes.h"

#include <bitset>
#include <cstring>
#include <format>
#include <glm/glm.hpp>

namespace {

auto makeInstanceSchema() -> GpuSchema {
    using R = GpuInstanceRecord;
    return {
        .name = "GpuInstanceRecord",
        .stride = sizeof(R),
        .fields = {
            {"model", offsetof(R, model), GpuFieldType::Mat4},
            {"material", offsetof(R, material), GpuFieldType::U32},
            {"mesh", offsetof(R, mesh), GpuFieldType::U32},
            {"indexOffset", offsetof(R, indexOffset), GpuFieldType::U32},
            {"indexCount", offsetof(R, indexCount), GpuFieldType::U32},
            {"boundsMin", offsetof(R, boundsMin), GpuFieldType::Vec3},
            {"flags", offsetof(R, flags), GpuFieldType::Bits},
            {"boundsMax", offsetof(R, boundsMax), GpuFieldType::Vec3},
        },
    };
}

auto makeMeshSchema() -> GpuSchema {
    using M = GpuMeshEntry;
    return {
        .name = "GpuMeshEntry",
        .stride = sizeof(M),
        .fields = {
            {"firstIndex", offsetof(M, firstIndex), GpuFieldType::U32},
            {"vertexOffset", offsetof(M, vertexOffset), GpuFieldType::I32},
            {"indexCount", offsetof(M, indexCount), GpuFieldType::U32},
        },
    };
}

auto makeMaterialSchema() -> GpuSchema {
    return {
        .name = "GpuMaterial",
        .stride = sizeof(GpuMaterial),
        .fields = {{"baseColorTexture", offsetof(GpuMaterial, baseColorTexture), GpuFieldType::U32}},
    };
}

auto makeCommandSchema() -> GpuSchema {
    using C = RhiDrawIndexedIndirectCommand;
    return {
        .name = "RhiDrawIndexedIndirectCommand",
        .stride = sizeof(C),
        .fields = {
            {"indexCount", offsetof(C, indexCount), GpuFieldType::U32},
            {"instanceCount", offsetof(C, instanceCount), GpuFieldType::U32},
            {"firstIndex", offsetof(C, firstIndex), GpuFieldType::U32},
            {"vertexOffset", offsetof(C, vertexOffset), GpuFieldType::I32},
            {"firstInstance", offsetof(C, firstInstance), GpuFieldType::U32},
        },
    };
}

// The culling counters (DrawLists::counter*): per workgroup in cullGroupCounters, totals in drawCounts.
auto makeCounterSchema(const char* name) -> GpuSchema {
    GpuSchema schema = {.name = name, .stride = DrawLists::counterCount * sizeof(uint32_t)};
    for (uint32_t r = 0; r < DrawLists::regionCount; r++) {
        schema.fields.push_back({std::format("draws.r{}", r), (DrawLists::counterDraws + r) * 4, GpuFieldType::U32});
    }
    for (uint32_t r = 0; r < DrawLists::regionCount; r++) {
        schema.fields.push_back({std::format("primitives.r{}", r), (DrawLists::counterPrimitives + r) * 4, GpuFieldType::U32});
    }
    schema.fields.push_back({"cameraCulled", DrawLists::counterCameraCulled * 4, GpuFieldType::U32});
    for (uint32_t c = 0; c < maxShadowCascades; c++) {
        schema.fields.push_back({std::format("cascadeCulled{}", c), (DrawLists::counterCascadeCulled + c) * 4, GpuFieldType::U32});
    }
    for (uint32_t c = 0; c < maxShadowCascades; c++) {
        schema.fields.push_back({std::format("cascadeDrawn{}", c), (DrawLists::counterCascadeDrawn + c) * 4, GpuFieldType::U32});
    }
    return schema;
}

auto makeOffsetSchema() -> GpuSchema {
    GpuSchema schema = {.name = "GroupOffsets", .stride = DrawLists::regionCount * sizeof(uint32_t)};
    for (uint32_t r = 0; r < DrawLists::regionCount; r++) {
        schema.fields.push_back({std::format("offset.r{}", r), r * 4, GpuFieldType::U32});
    }
    return schema;
}

auto makeParamsSchema() -> GpuSchema {
    using P = DrawLists::CullParams;
    GpuSchema schema = {
        .name = "CullParams",
        .stride = sizeof(P),
        .fields = {
            {"instanceCount", offsetof(P, instanceCount), GpuFieldType::U32},
            {"viewCount", offsetof(P, viewCount), GpuFieldType::U32},
            {"cullEnabled", offsetof(P, cullEnabled), GpuFieldType::U32},
            {"regionCapacity", offsetof(P, regionCapacity), GpuFieldType::U32},
            {"groupCount", offsetof(P, groupCount), GpuFieldType::U32},
        },
    };
    for (uint32_t v = 0; v < DrawLists::viewCount; v++) {
        for (uint32_t p = 0; p < 6; p++) {
            auto offset = (uint32_t) (offsetof(P, planes) + (((v * 6) + p) * sizeof(glm::vec4)));
            schema.fields.push_back({std::format("view{}.plane{}", v, p), offset, GpuFieldType::Vec4});
        }
    }
    return schema;
}

auto makeVisibilitySchema() -> GpuSchema {
    return {.name = "VisibilityBits", .stride = 4, .fields = {{"views", 0, GpuFieldType::Bits}}};
}

auto makeCullPlanesSchema() -> GpuSchema {
    GpuSchema schema = {.name = "CullPlanes", .stride = 4};
    for (uint32_t v = 0; v < DrawLists::viewCount; v++) {
        schema.fields.push_back({std::format("view{}", v), 0, GpuFieldType::CullPlane, v});
    }
    return schema;
}

auto makeViewUboSchema() -> GpuSchema {
    return {
        .name = "UniformBufferObject",
        .stride = sizeof(UniformBufferObject),
        .fields = {
            {"view", offsetof(UniformBufferObject, view), GpuFieldType::Mat4},
            {"proj", offsetof(UniformBufferObject, proj), GpuFieldType::Mat4},
        },
    };
}

auto makeU32Schema() -> GpuSchema {
    return {.name = "u32", .stride = 4, .fields = {{"value", 0, GpuFieldType::U32}}};
}

template <typename T>
auto load(std::span<const std::byte> row, uint32_t offset) -> T {
    T value;
    std::memcpy(&value, row.data() + offset, sizeof(T));
    return value;
}

} // namespace

auto schemaForResource(std::string_view resource) -> const GpuSchema& {
    static const GpuSchema instance = makeInstanceSchema();
    static const GpuSchema mesh = makeMeshSchema();
    static const GpuSchema material = makeMaterialSchema();
    static const GpuSchema command = makeCommandSchema();
    static const GpuSchema totals = makeCounterSchema("CullTotals");
    static const GpuSchema groups = makeCounterSchema("CullGroupCounters");
    static const GpuSchema offsets = makeOffsetSchema();
    static const GpuSchema params = makeParamsSchema();
    static const GpuSchema visibility = makeVisibilitySchema();
    static const GpuSchema ubo = makeViewUboSchema();
    static const GpuSchema planes = makeCullPlanesSchema();
    if (resource == "cullPlanes") {
        return planes;
    }
    static const GpuSchema u32 = makeU32Schema();
    if (resource == "instances" || resource == "instanceStaging") {
        return instance;
    }
    if (resource == "gpuscene.meshtable") {
        return mesh;
    }
    if (resource == "gpuscene.materials") {
        return material;
    }
    if (resource == "drawCommands") {
        return command;
    }
    if (resource == "drawCounts") {
        return totals;
    }
    if (resource == "cullGroupCounters") {
        return groups;
    }
    if (resource == "cullGroupOffsets") {
        return offsets;
    }
    if (resource == "cullParams") {
        return params;
    }
    if (resource == "cullVisibility") {
        return visibility;
    }
    if (resource == "renderer.view.ubo") {
        return ubo;
    }
    return u32;
}

auto fieldTypeName(GpuFieldType type) -> const char* {
    switch (type) {
        case GpuFieldType::U32:
            return "u32";
        case GpuFieldType::I32:
            return "i32";
        case GpuFieldType::F32:
            return "f32";
        case GpuFieldType::Bits:
            return "bits";
        case GpuFieldType::Vec3:
            return "vec3";
        case GpuFieldType::Vec4:
            return "vec4";
        case GpuFieldType::Mat4:
            return "mat4";
        case GpuFieldType::CullPlane:
            return "cullplane";
    }
    return "?";
}

auto fieldSize(GpuFieldType type) -> uint32_t {
    switch (type) {
        case GpuFieldType::Vec3:
            return 12;
        case GpuFieldType::Vec4:
            return 16;
        case GpuFieldType::Mat4:
            return 64;
        default:
            return 4;
    }
}

auto decodeField(const GpuField& field, std::span<const std::byte> row) -> std::string {
    if (field.offset + fieldSize(field.type) > row.size()) {
        return {};
    }
    switch (field.type) {
        case GpuFieldType::U32:
            return std::format("{}", load<uint32_t>(row, field.offset));
        case GpuFieldType::I32:
            return std::format("{}", load<int32_t>(row, field.offset));
        case GpuFieldType::F32:
            return std::format("{:.6g}", load<float>(row, field.offset));
        case GpuFieldType::Bits:
            return std::bitset<8>(load<uint32_t>(row, field.offset)).to_string();
        case GpuFieldType::Vec3: {
            auto v = load<glm::vec3>(row, field.offset);
            return std::format("({:.4g}, {:.4g}, {:.4g})", v.x, v.y, v.z);
        }
        case GpuFieldType::Vec4: {
            auto v = load<glm::vec4>(row, field.offset);
            return std::format("({:.4g}, {:.4g}, {:.4g}, {:.4g})", v.x, v.y, v.z, v.w);
        }
        case GpuFieldType::CullPlane:
            return cullPlaneName((load<uint32_t>(row, field.offset) >> (4 * field.nibble)) & 0xFu);
        case GpuFieldType::Mat4: {
            auto m = load<glm::mat4>(row, field.offset);
            // Rows of the matrix, as it reads mathematically (glm is column-major).
            return std::format("[{:.3g} {:.3g} {:.3g} {:.3g}; {:.3g} {:.3g} {:.3g} {:.3g}; {:.3g} {:.3g} {:.3g} {:.3g}; {:.3g} {:.3g} {:.3g} {:.3g}]",
                               m[0][0],
                               m[1][0],
                               m[2][0],
                               m[3][0],
                               m[0][1],
                               m[1][1],
                               m[2][1],
                               m[3][1],
                               m[0][2],
                               m[1][2],
                               m[2][2],
                               m[3][2],
                               m[0][3],
                               m[1][3],
                               m[2][3],
                               m[3][3]);
        }
    }
    return {};
}

auto decodeRow(const GpuSchema& schema, std::span<const std::byte> bytes, size_t index) -> std::vector<std::string> {
    std::vector<std::string> values;
    auto begin = index * schema.stride;
    if (begin >= bytes.size()) {
        return values;
    }
    auto row = bytes.subspan(begin, std::min<size_t>(schema.stride, bytes.size() - begin));
    values.reserve(schema.fields.size());
    for (const auto& field : schema.fields) {
        values.push_back(decodeField(field, row));
    }
    return values;
}

auto cullPlaneName(uint32_t code) -> const char* {
    switch (code) {
        case 0:
            return "left";
        case 1:
            return "right";
        case 2:
            return "bottom";
        case 3:
            return "top";
        case 4:
            return "near";
        case 5:
            return "far";
        case 0xE:
            return "visible";
        default:
            return "-";
    }
}
