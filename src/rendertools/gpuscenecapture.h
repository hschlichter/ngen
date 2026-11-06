#pragma once

#include "capture.h"
#include "renderworld.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <glm/glm.hpp>
#include <map>
#include <span>
#include <string>
#include <vector>

// The GPU scene's tables read back from captures and joined per instance: what introspect.gpuscene answers and the GPU Scene window shows.

// The tables to capture, as (pass, resource): each where it is complete for the frame ("" is the end of the frame).
auto gpuSceneCaptures() -> const std::vector<std::pair<std::string, std::string>>&;

// A captured table by resource name; null when it wasn't captured or the capture failed.
auto capturedTable(const std::map<std::string, CaptureResult>& byResource, const char* resource) -> const CaptureResult*;
// A value in a captured table at a byte offset; zero past its end.
template <typename T>
auto capturedValue(const CaptureResult& table, size_t offset) -> T {
    T value{};
    if (table.bytes && offset + sizeof(T) <= table.bytes->size()) {
        std::memcpy(&value, table.bytes->data() + offset, sizeof(T));
    }
    return value;
}

// A culling view's name: 0 is the camera, then the shadow cascades.
auto gpuSceneViewName(uint32_t view) -> std::string;
// An instance record's flags as words: "doubleSided noBounds".
auto gpuInstanceFlagText(uint32_t flags) -> std::string;

// One instance of the GPU scene, joined from the captured tables: the instance record, the
// mesh and material tables, and the culling result per view.
struct GpuSceneInstanceRow {
    uint32_t index = 0;
    uint32_t prim = 0;
    std::string primPath;
    uint32_t mesh = 0;
    uint32_t material = 0;    // material table entry
    uint32_t textureSlot = 0; // from the material table
    uint32_t meshletOffset = 0;
    uint32_t meshletCount = 0;
    uint32_t meshMeshletCount = 0; // from the mesh table; 0 = mesh not in the pool
    uint32_t flags = 0;
    glm::vec3 boundsMin = glm::vec3(0.0f);
    glm::vec3 boundsMax = glm::vec3(0.0f);
    uint32_t visibleBits = 0; // bit v: visible in view v
    uint32_t cullPlanes = 0;  // 4 bits per view (cullPlaneName)
};

// The prim of every mesh instance, through the render world's prim ranges.
auto primOfEachInstance(const RenderWorld& renderWorld) -> std::vector<uint32_t>;

// Joins the captured tables by instance; primOfInstance holds the prim of every live instance
// and primPath names a prim. Missing captures leave their fields at 0.
auto buildGpuSceneRows(const std::map<std::string, CaptureResult>& byResource, std::span<const uint32_t> primOfInstance, const std::function<std::string(uint32_t)>& primPath) -> std::vector<GpuSceneInstanceRow>;
auto writeGpuSceneJoinedJson(const std::string& path, const std::vector<GpuSceneInstanceRow>& rows, uint32_t viewCount) -> bool;
auto writeGpuSceneJoinedJson(FILE* f, const std::vector<GpuSceneInstanceRow>& rows, uint32_t viewCount) -> void;
