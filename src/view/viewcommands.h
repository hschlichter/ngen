#pragma once

#include "rpcrecords.h"
#include "rpcregistry.h"
#include "sessionscript.h"
#include <nlohmann/json.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class AssetClient;
class Renderer;
class RenderThread;
class RenderDocCapture;
class ViewDumps;
class ViewPicking;
class ViewScene;
class ViewUI;
struct Camera;
struct CullResult;

// Where "go to asset" is: the asset, the order it steps through its draws (nearest to the camera first, as they were at its first go-to),
// and the step. A rebuilt scene (another revision) starts the order again.
struct AssetGoToState {
    std::string assetId;
    std::vector<uint32_t> order;
    size_t index = 0;
    uint64_t revision = 0;
};

// An answer that waits until the scene is resident: everything it wants is loaded, uploaded and drawn. A method that changes what is
// drawn answers when the change is on screen; a script holds its clock while one waits.
struct ResidentWaiter {
    RpcResponder responder;
    rpc::Json result;
};

// What ngen-view's commands act on. Owned by the view's main; the commands hold references.
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

// A camera flight (`fly`, view.camera.fly): the camera moves through the scene's cameras whose paths start with a prefix, in path order, at a
// speed in metres a second of the frame's time, eye and direction interpolated between them. A test driver: it moves the camera, it decides
// nothing; a script holds its clock while it flies, and the call answers when the last camera is reached or the flight is stopped.
struct CameraFlight {
    std::vector<std::string> cameras;
    std::vector<glm::vec3> eyes;
    std::vector<float> yaws; // degrees
    std::vector<float> pitches;
    float speed = 0.0f; // metres a second
    size_t segment = 0; // from eyes[segment] to eyes[segment + 1]
    float along = 0.0f; // metres into the segment
    double metres = 0.0;
    double seconds = 0.0;
    std::optional<RpcResponder> responder;

    auto flying() const -> bool { return segment + 1 < eyes.size(); }
};

// A `renderlod` record or `view.renderlod` call waiting for a culling readback with every render-LOD group's state (frames later): the
// readback frame it was asked at, and for the call the path whose groups it lists.
struct RenderLodRequest {
    enum class Kind : uint8_t {
        Record,  // the renderlod record (with `path`, the groups under it)
        Inspect, // view.lod.inspect of the root at `path`
        Tool,    // the lodtool record
    };
    RpcResponder responder;
    uint64_t askedAt = 0;
    std::optional<std::string> path;
    Kind kind = Kind::Record;
};

struct ViewContext {
    Camera& cam;
    ViewScene& scene;
    ViewUI& ui;
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
    std::string sceneLabel;
};

// Registers every view command as an RPC method (view.*, introspect.*, capture.*,
// renderdoc.*). The same methods serve live calls and --script lines.
auto registerViewMethods(RpcRegistry& registry, ViewContext& view) -> void;

// Registers ngen-view's records (status, scene, assets, culling, profile, render, memory, counters), served by
// introspect.list and introspect.get, and introspect.trace (the `record` verb), which puts a record into the trace.
// `records` must outlive the registry.
auto registerViewRecords(RpcRecords& records, RpcRegistry& registry, ViewContext& view) -> void;

// Starts a flight through the cameras whose paths start with `prefix` at `speed` metres a second; the responder, when given, answers when it
// ends. An error when fewer than two cameras match. A flight running before is stopped.
auto startFlight(ViewContext& view, const std::string& prefix, float speed, std::optional<RpcResponder> responder) -> std::optional<std::string>;
// Stops the flight where the camera is; its call answers.
auto stopFlight(ViewContext& view) -> void;
// Moves the camera along its flight by a frame's time; answers the flight's call when it ends. Once a frame, before the camera is posted.
auto advanceFlight(ViewContext& view, float dt) -> void;
// The camera LOD and streaming are decided from: the main camera, or the debug camera while it is on and set to drive them.
auto lodCamera(const ViewContext& view) -> const Camera&;

// Answers the render-LOD requests whose readback has arrived; once a frame, after the culling readback is taken. True while some still
// wait, so the next frames read the states back.
auto serviceRenderLodRequests(ViewContext& view) -> bool;

// Moves the camera to an asset: the next of its draws (the first go-to picks the nearest), framed and its prim selected, or with `all`
// every draw framed together. `index` and `count` say where in its draws it is; `error` is set when nothing draws it.
struct AssetGoToResult {
    std::string prim;
    size_t index = 0;
    size_t count = 0;
    std::string error;
};
auto goToAsset(ViewContext& view, const std::string& assetId, bool all) -> AssetGoToResult;

// Runs one `<frame> <verb> [args]` script command: the verb's parser turns the text into the
// method's parameters, and failures print as `<verb>: <message>`, as the verbs always have.
auto runViewScriptCommand(const RpcRegistry& registry, const SessionCommand& command) -> void;
