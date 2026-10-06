#pragma once

#include "rpcrecords.h"
#include "rpcregistry.h"
#include "sessionscript.h"

#include <cstdint>
#include <functional>
#include <string>

class AssetClient;
class EditorUI;
class Renderer;
class RenderThread;
class RenderDocCapture;
class SceneQuerySystem;
class SceneUpdater;
class USDScene;
class ViewDumps;
struct Camera;
struct CullResult;
struct PrimHandle;
struct RenderWorld;

// What ngen-view's commands act on. Owned by the view's main; the commands hold references.
struct ViewContext {
    Camera& cam;
    USDScene& usdScene;
    SceneQuerySystem& sceneQuery;
    SceneUpdater& sceneUpdater;
    PrimHandle& selectedPrim;
    EditorUI& editorUI;
    Renderer& renderer;
    RenderThread& renderThread;
    RenderDocCapture& renderDoc;
    ViewDumps& dumps;
    const RenderWorld& renderWorld;
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

// Runs one `<frame> <verb> [args]` script command: the verb's parser turns the text into the
// method's parameters, and failures print as `<verb>: <message>`, as the verbs always have.
auto runViewScriptCommand(const RpcRegistry& registry, const SessionCommand& command) -> void;
