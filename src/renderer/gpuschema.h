#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Field layouts of the structs the renderer puts in GPU buffers, so buffer contents can be
// shown and dumped as typed rows. One schema per struct, next to nothing else: the offsets
// come from the C++ definitions (offsetof), so a layout change updates the tools.
enum class GpuFieldType : uint8_t {
    U32,
    I32,
    F32,
    Bits, // u32 shown in binary
    Vec3,
    Vec4,
    Mat4,
    CullPlane, // 4-bit cull reason `nibble` of a u32: plane name, "visible" or "-"
};

struct GpuField {
    std::string name;
    uint32_t offset = 0;
    GpuFieldType type = GpuFieldType::U32;
    uint32_t nibble = 0; // CullPlane: which 4-bit group
};

// Name of a cull reason code written by instancecull.comp (0..5 planes, 0xE visible, 0xF not tested).
auto cullPlaneName(uint32_t code) -> const char*;

struct GpuSchema {
    std::string name;    // struct name
    uint32_t stride = 0; // bytes per row
    std::vector<GpuField> fields;
};

// The schema for a frame-graph resource or static scene buffer name ("drawCommands",
// "instances", "gpuscene.meshtable", ...); plain u32 rows for names without one.
auto schemaForResource(std::string_view resource) -> const GpuSchema&;
auto fieldTypeName(GpuFieldType type) -> const char*;
auto fieldSize(GpuFieldType type) -> uint32_t;
// One field of one row as text; empty when the row is shorter than the field.
auto decodeField(const GpuField& field, std::span<const std::byte> row) -> std::string;
// Every field of row `index` as text.
auto decodeRow(const GpuSchema& schema, std::span<const std::byte> bytes, size_t index) -> std::vector<std::string>;
