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
    PrimitiveId = 7,  // value: triangle index in its mesh's meshlets (meshlet * 124 + triangle)
    Uv = 8,           // value: u, v
    Streaming = 9,    // value: the streaming root slot + 1 (RenderMeshInstance::streamRoot), coloured by that root's state (StreamingRootState)
    Meshlet = 10,     // value: meshlet index in its mesh, instance index
    MeshletCull = 11, // value: why the task shader culls it (0 kept, 1 frustum, 2 cone, 3 depth), instance index, meshlet index in its mesh
};

inline constexpr uint32_t debugViewCount = 12;

// A streaming root's state as the streaming view colours it (shaders/debugview.comp) and the Streaming tool names it, worst first when
// several hold: drawing nothing, drawing coarser than selected, a budget granting a coarser level, a resident level still on its way, a
// finer level resident ahead of being drawn, drawing what it selects. `streamingStateFinding` is added when the root breaks the authoring
// contract.
enum class StreamingRootState : uint32_t {
    None = 0,
    Drawn = 1,
    Prefetched = 2,
    Requested = 3,
    Refused = 4,
    Late = 5,
    Hole = 6,
};
inline constexpr uint32_t streamingStateFinding = 16;

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
    "streaming",
    "meshlet",
    "meshletcull",
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
    "Streaming State",
    "Meshlet",
    "Meshlet Culling",
};
