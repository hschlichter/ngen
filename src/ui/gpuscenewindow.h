#pragma once

#include "capture.h"
#include "renderworld.h"

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <map>
#include <span>
#include <string>
#include <vector>

// One instance of the GPU scene, joined from the captured tables: the instance record, the
// mesh and material tables, and the culling result per view.
struct GpuSceneInstanceRow {
    uint32_t index = 0;
    uint32_t prim = 0;
    std::string primPath;
    uint32_t mesh = 0;
    uint32_t material = 0;    // material table entry
    uint32_t textureSlot = 0; // from the material table
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    uint32_t meshIndexCount = 0; // from the mesh table; 0 = mesh not in the pool
    uint32_t flags = 0;
    glm::vec3 boundsMin = glm::vec3(0.0f);
    glm::vec3 boundsMax = glm::vec3(0.0f);
    uint32_t visibleBits = 0; // bit v: visible in view v
    uint32_t cullPlanes = 0;  // 4 bits per view (cullPlaneName)
};

// The prim of every mesh instance, through the render world's prim ranges.
auto primOfEachInstance(const RenderWorld& renderWorld) -> std::vector<uint32_t>;

// Capture watch ids of the GPU Scene window, in this order.
inline constexpr uint32_t gpuSceneWatchBase = 2000;
auto gpuSceneCaptures() -> const std::vector<std::pair<std::string, std::string>>&;

// Joins the captured tables by instance; primOfInstance holds the prim of every live instance
// and primPath names a prim. Missing captures leave their fields at 0.
auto buildGpuSceneRows(const std::map<std::string, CaptureResult>& byResource, std::span<const uint32_t> primOfInstance, const std::function<std::string(uint32_t)>& primPath) -> std::vector<GpuSceneInstanceRow>;
auto writeGpuSceneJoinedJson(const std::string& path, const std::vector<GpuSceneInstanceRow>& rows, uint32_t viewCount) -> bool;

struct GpuSceneWindowState {
    bool live = false;
    uint32_t trigger = 1;
    std::map<std::string, CaptureResult> results; // by resource
    int cullView = 0;
    int compactionRegion = 0;
    char filter[64] = "";
};

// The GPU scene tables as browsable data, culling per view with reasons, and the compaction.
// primPath names a prim; selectPrim is called with a prim index when a row is clicked.
auto drawGpuSceneWindow(bool& show,
                        GpuSceneWindowState& state,
                        uint32_t viewCount,
                        std::span<const uint32_t> primOfInstance,
                        const std::function<std::string(uint32_t)>& primPath,
                        const std::function<void(uint32_t)>& selectPrim) -> void;
auto gpuSceneWatches(const GpuSceneWindowState& state) -> std::vector<CaptureWatch>;
auto gpuSceneResourceOfWatch(uint32_t id) -> const std::string*;
