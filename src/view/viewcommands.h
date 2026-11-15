#pragma once

#include "assetgoto.h"
#include "cameraflight.h"
#include "consolecommands.h"
#include "ground.h"
#include "rpcrecords.h"
#include "rpcregistry.h"
#include "viewframe.h"
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
class RpcEndpoint;
class SessionScript;
class Renderer;
class RenderThread;
class RenderTools;
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

// An answer that waits until the scene is ready: everything it wants is loaded, uploaded and drawn. A method that changes what is
// drawn answers when the change is on screen; a script holds its clock while one waits.
struct ReadyWaiter {
    RpcResponder responder;
    rpc::Json result;
};

// The script's clock: its frames count only while nothing holds it (the scene first becoming ready, unless --no-wait-ready; a method
// that answers on readiness; a flight), so a script sees the scene as it is once loaded. The view quits after `lastFrame` (--frames).
struct ScriptClock {
    bool waitForReady = true;
    uint64_t lastFrame = 0; // none: 0
    bool held = false;
    uint64_t frame = 0;
};

// An answer waiting for a culling readback with every LOD group's state (frames later): the `lodgroups` record, view.lodgroups, and
// the LOD tool's inspection and record. `answer` runs once a readback taken after `askedAt` has arrived.
struct LodGroupRequest {
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
    RenderTools& renderTools; // the renderer's instrumentation: screenshots, the texture inspector and dumps
    ViewDumps& dumps;
    const CullResult& latestCull;
    const AssetClient& assets;
    std::function<void()> frameSceneView;
    bool& quit;
    const uint64_t& frameCounter;
    std::vector<ReadyWaiter>& readyWaiters;
    const SceneReadiness& readiness;
    ScriptClock& script;
    std::vector<LodGroupRequest>& lodGroupRequests;
    const float& viewAspect; // the view's width over height, as the LOD heuristic and the GPU's pick measure it
    const uint64_t& lodPops; // LOD-group pops (a drawn level changed without a fade) counted over the readbacks seen
    CameraFlight& flight;
    CameraModesState& cameraModes;  // the camera modes' state (ground.h)
    const std::string& startCamera; // the start camera not placed yet (ViewFrame::startCamera); empty once placed
    const bool& autoFrame;          // the camera follows the scene as it arrives, until it is first ready or moved
    std::string sceneLabel;
    // The camera a `camera: "debug"` parameter moves: the tools' debug camera while it is on; unset (or null) without one.
    std::function<Camera*()> debugCamera;
};

// Registers ngen-view's engine commands, each domain's methods, console commands and records: the camera (cameracommands.cpp), the scene
// (scenecommands.cpp), LOD and streaming (lodcommands.cpp), the cvars (cvarcommands.cpp), the renderer's display and captures
// (displaycommands.cpp), the profiler (profilecommands.cpp), and introspect.record. The tools and the UI add theirs (src/tools/toolcommands.h,
// src/ui/uicommands.h). `records` must outlive the registry.
auto registerViewCommands(RpcRegistry& registry, ConsoleCommands& console, RpcRecords& records, ViewContext& view) -> void;

auto registerCameraCommands(RpcRegistry& registry, ConsoleCommands& console, ViewContext& view) -> void;
auto registerSceneCommands(RpcRegistry& registry, ConsoleCommands& console, RpcRecords& records, ViewContext& view) -> void;
auto registerLodCommands(RpcRegistry& registry, ConsoleCommands& console, RpcRecords& records, ViewContext& view) -> void;
auto registerCvarCommands(RpcRegistry& registry, ConsoleCommands& console, RpcRecords& records) -> void;
auto registerDisplayCommands(RpcRegistry& registry, ConsoleCommands& console, RpcRecords& records, ViewContext& view) -> void;
auto registerProfileCommands(RpcRegistry& registry, ConsoleCommands& console, RpcRecords& records) -> void;

// The commands due this frame: the script's lines at its frame while nothing holds its clock, then the calls the endpoint received. Once a
// frame, after the scene update and the tools' lists.
auto runFrameCommands(ViewContext& view, SessionScript& session, const RpcRegistry& registry, const ConsoleCommands& console, RpcEndpoint& endpoint) -> void;

// Answers the methods waiting for the scene to be ready once it is; once a frame, after readiness is updated.
auto answerReadyWaiters(ViewContext& view) -> void;

// Answers the LOD-group requests whose readback has arrived; once a frame, after the culling readback is taken. True while some still
// wait, so the next frames read the states back.
auto serviceLodGroupRequests(ViewContext& view) -> bool;

// The view's LOD tuning as the records give it: bias, hysteresis, blending, and every override by authored root and heuristic.
auto lodTuningJson(const LodTuning& tuning) -> rpc::Json;

// The console commands that set cvars (`view lit`, `prepass on`, `lod-show labels on`, …): their arguments as cvar.set's {values}.
auto cvarValues(std::initializer_list<std::pair<const char*, std::string>> values) -> rpc::Json;
