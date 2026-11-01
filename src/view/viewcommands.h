#pragma once

#include "assetgoto.h"
#include "cameraflight.h"
#include "ground.h"
#include "rpcrecords.h"
#include "rpcregistry.h"
#include "rpcverbs.h"
#include <nlohmann/json.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class AssetClient;
class Renderer;
class RenderThread;
class RenderDocCapture;
class ViewDumps;
class ViewPicking;
class ViewScene;
struct Camera;
struct CullResult;
struct LodTuning;

// A position or a direction in a record or a result: [x, y, z].
template <>
struct nlohmann::adl_serializer<glm::vec3> {
    static auto to_json(nlohmann::json& json, const glm::vec3& v) -> void { json = nlohmann::json::array({v.x, v.y, v.z}); }
};

// An answer that waits until the scene is resident: everything it wants is loaded, uploaded and drawn. A method that changes what is
// drawn answers when the change is on screen; a script holds its clock while one waits.
struct ResidentWaiter {
    RpcResponder responder;
    rpc::Json result;
};

// Where the view is in loading its scene, kept by the main loop and read by the `scenethread` record and the Scene window. Resident: the scene
// thread has nothing left to do and the render thread has drawn every change with nothing left to upload.
struct ViewResidency {
    bool resident = false;
    bool everResident = false;
    bool scriptHeld = false; // script frames don't count: waiting for the first residency or for a method that answers on it
    uint64_t scriptFrame = 0;
    // The load in progress: main-loop frames while not resident, and the longest of them.
    uint64_t loadingFrames = 0;
    double longestLoadingFrameMs = 0.0;
    // The last time it became resident: the frame, the time since start, and that load's frames and longest frame.
    uint64_t lastResidentFrame = 0;
    double lastResidentMs = 0.0;
    uint64_t lastLoadFrames = 0;
    double lastLoadLongestMs = 0.0;
};

// An answer waiting for a culling readback with every render-LOD group's state (frames later): the `renderlod` record, view.renderlod, and
// the LOD tool's inspection and record. `answer` runs once a readback taken after `askedAt` has arrived.
struct RenderLodRequest {
    uint64_t askedAt = 0;
    std::function<void()> answer;
};

// What ngen-view's commands act on. Owned by the view's main; the commands hold references.
struct ViewContext {
    Camera& cam;
    ViewScene& scene;
    std::string& selectedPath; // the selected prim's full path; empty for none
    ViewPicking& picking;
    AssetGoToState& goTo;
    Renderer& renderer;
    RenderThread& renderThread;
    RenderDocCapture& renderDoc;
    ViewDumps& dumps;
    const CullResult& latestCull;
    const AssetClient& assets;
    std::function<void()> frameSceneView;
    bool& quit;
    const uint64_t& frameCounter;
    std::vector<ResidentWaiter>& residentWaiters;
    const ViewResidency& residency;
    std::vector<RenderLodRequest>& renderLodRequests;
    const float& viewAspect; // the view's width over height, as the LOD heuristic and the GPU's pick measure it
    const uint64_t& lodPops; // render-LOD pops (a drawn level changed without a fade) counted over the readbacks seen
    CameraFlight& flight;
    CameraModesState& cameraModes; // the camera modes' state (ground.h)
    std::string sceneLabel;
    // The camera a `camera: "debug"` parameter moves: the tools' debug camera while it is on; unset (or null) without one.
    std::function<Camera*()> debugCamera;
};

// Registers ngen-view's engine commands, each domain's methods, script verbs and records: the camera (cameracommands.cpp), the scene
// (scenecommands.cpp), LOD and streaming (lodcommands.cpp), the configuration (configcommands.cpp), the renderer's display and captures
// (displaycommands.cpp), the profiler (profilecommands.cpp), and introspect.record. The tools and the UI add theirs (src/tools/toolcommands.h,
// src/ui/uicommands.h). `records` must outlive the registry.
auto registerViewCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records, ViewContext& view) -> void;

auto registerCameraCommands(RpcRegistry& registry, RpcVerbs& verbs, ViewContext& view) -> void;
auto registerSceneCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records, ViewContext& view) -> void;
auto registerLodCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records, ViewContext& view) -> void;
auto registerConfigCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records) -> void;
auto registerDisplayCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records, ViewContext& view) -> void;
auto registerProfileCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records) -> void;

// Answers the render-LOD requests whose readback has arrived; once a frame, after the culling readback is taken. True while some still
// wait, so the next frames read the states back.
auto serviceRenderLodRequests(ViewContext& view) -> bool;

// The view's LOD tuning as the records give it: bias, hysteresis, blending, and every override by authored root and heuristic.
auto lodTuningJson(const LodTuning& tuning) -> rpc::Json;

// The verbs that set configuration values (`view lit`, `prepass on`, `lod-show labels on`, …): their arguments as config.set's {values}.
auto configValues(std::initializer_list<std::pair<const char*, std::string>> values) -> rpc::Json;
