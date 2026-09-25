#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// Viewport debug views drawn by DebugViewPass. The values match `mode` in
// shaders/debugview.frag and shaders/debugview.comp.
enum class DebugView : uint32_t {
    None = 0,
    Wireframe = 1,    // value: edge distance in pixels, shade
    TriangleSize = 2, // value: triangle screen area in pixels
    Overdraw = 3,     // value: fragments shaded, no depth test
    InstanceId = 4,   // value: instance index
    MeshId = 5,       // value: mesh table entry
    MaterialId = 6,   // value: material table entry
    PrimitiveId = 7,  // value: triangle index within its draw
    Uv = 8,           // value: u, v
};

inline constexpr uint32_t debugViewCount = 9;

inline constexpr std::array<std::string_view, debugViewCount> debugViewNames = {
    "off",
    "wireframe",
    "trianglesize",
    "overdraw",
    "instance",
    "mesh",
    "material",
    "primitive",
    "uv",
};

inline constexpr std::array<const char*, debugViewCount> debugViewLabels = {
    "Off",
    "Wireframe",
    "Triangle Size",
    "Overdraw",
    "Instance ID",
    "Mesh ID",
    "Material ID",
    "Primitive ID",
    "UV Checker",
};
