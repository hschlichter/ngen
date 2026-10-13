#pragma once

#include "rpcrecords.h"
#include "rpcregistry.h"
#include "sessionscript.h"

#include <cstdint>
#include <functional>
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

// What ngen-view's commands act on. Owned by the view's main; the commands hold references.
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
    std::string sceneLabel;
};

// Registers every view command as an RPC method (view.*, introspect.*, capture.*,
// renderdoc.*). The same methods serve live calls and --script lines.
auto registerViewMethods(RpcRegistry& registry, ViewContext& view) -> void;

// Registers ngen-view's records (status, scene, assets, culling, profile, render, memory, counters), served by
// introspect.list and introspect.get, and introspect.trace (the `record` verb), which puts a record into the trace.
// `records` must outlive the registry.
auto registerViewRecords(RpcRecords& records, RpcRegistry& registry, ViewContext& view) -> void;

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
