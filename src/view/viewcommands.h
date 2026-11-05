#pragma once

#include "assetgoto.h"
#include "cameraflight.h"
#include "ground.h"
#include "rpcrecords.h"
#include "rpcregistry.h"
#include "rpcverbs.h"
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

// An answer that waits until the scene is resident: everything it wants is loaded, uploaded and drawn. A method that changes what is
// drawn answers when the change is on screen; a script holds its clock while one waits.
struct ResidentWaiter {
    RpcResponder responder;
    rpc::Json result;
};

// The script's clock: its frames count only while nothing holds it (the scene first becoming resident, unless --no-wait-resident; a method
// that answers on residency; a flight), so a script sees the scene as it is once loaded. The view quits after `lastFrame` (--frames).
struct ScriptClock {
    bool waitForResident = true;
    uint64_t lastFrame = 0; // none: 0
    bool held = false;
    uint64_t frame = 0;
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
    RenderTools& renderTools; // the renderer's instrumentation: screenshots, the texture inspector and dumps
    ViewDumps& dumps;
    const CullResult& latestCull;
    const AssetClient& assets;
    std::function<void()> frameSceneView;
    bool& quit;
    const uint64_t& frameCounter;
    std::vector<ResidentWaiter>& residentWaiters;
    const ViewResidency& residency;
    ScriptClock& script;
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

// The commands due this frame: the script's lines at its frame while nothing holds its clock, then the calls the endpoint received. Once a
// frame, after the scene update and the tools' lists.
auto runFrameCommands(ViewContext& view, SessionScript& session, const RpcRegistry& registry, const RpcVerbs& verbs, RpcEndpoint& endpoint) -> void;

// Answers the methods waiting for the scene to be resident once it is; once a frame, after residency is updated.
auto answerResidentWaiters(ViewContext& view) -> void;

// Answers the render-LOD requests whose readback has arrived; once a frame, after the culling readback is taken. True while some still
// wait, so the next frames read the states back.
auto serviceRenderLodRequests(ViewContext& view) -> bool;

// The view's LOD tuning as the records give it: bias, hysteresis, blending, and every override by authored root and heuristic.
auto lodTuningJson(const LodTuning& tuning) -> rpc::Json;

// The verbs that set configuration values (`view lit`, `prepass on`, `lod-show labels on`, …): their arguments as config.set's {values}.
auto configValues(std::initializer_list<std::pair<const char*, std::string>> values) -> rpc::Json;
