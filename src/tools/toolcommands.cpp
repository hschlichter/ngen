#include "toolcommands.h"

#include "camera.h"
#include "culling.h"
#include "debugview.h"
#include "drawlists.h"
#include "primpath.h"
#include "renderconfig.h"
#include "viewscene.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <sstream>
#include <string_view>

namespace {

using rpc::Json;

auto parseDebugCamera(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string first;
    std::string second;
    in >> first >> second;
    if ((first == "on" || first == "off") && second.empty()) {
        return Json{{"on", first == "on"}};
    }
    if (first == "place" && second.empty()) {
        return Json{{"place", true}};
    }
    if (first == "control" && (second == "main" || second == "debug")) {
        return Json{{"controlMain", second == "main"}};
    }
    if ((first == "lod" || first == "cull" || first == "shadows") && (second == "main" || second == "debug")) {
        return Json{{first + "FromDebug", second == "debug"}};
    }
    if (first == "trail" && (second == "on" || second == "off")) {
        return Json{{"trail", second == "on"}};
    }
    if (first == "trail" && second == "clear") {
        return Json{{"clearTrail", true}};
    }
    return std::unexpected(std::format("expected 'on|off', 'place', 'control main|debug', 'lod|cull|shadows main|debug' or 'trail on|off|clear', got '{}'", args));
}

// "labels|gizmo|gizmo-all|flash on|off".
auto parseLodShow(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string layer;
    std::string state;
    in >> layer >> state;
    if (layer == "color" && (state == "level" || state == "depth")) {
        return configValues({{"tools.lod.colorByDepth", state == "depth" ? "true" : "false"}});
    }
    static constexpr std::array<std::pair<std::string_view, const char*>, 4> layers = {{
        {"labels", "tools.lod.labels"},
        {"gizmo", "tools.lod.gizmo"},
        {"gizmo-all", "tools.lod.gizmoAll"},
        {"flash", "tools.lod.popFlash"},
    }};
    auto found = std::ranges::find(layers, std::string_view(layer), &std::pair<std::string_view, const char*>::first);
    if (found == layers.end() || (state != "on" && state != "off")) {
        return std::unexpected(std::format("expected 'labels|gizmo|gizmo-all|flash on|off' or 'color level|depth', got '{}'", args));
    }
    return configValues({{found->second, state}});
}

// `streaming-show bounds|rings|rings-all|labels on|off`.
auto parseStreamingShow(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string layer;
    std::string state;
    in >> layer >> state;
    static constexpr std::array<std::pair<std::string_view, const char*>, 4> layers = {{
        {"bounds", "tools.streaming.bounds"},
        {"rings", "tools.streaming.rings"},
        {"rings-all", "tools.streaming.ringsAll"},
        {"labels", "tools.streaming.labels"},
    }};
    auto found = std::ranges::find(layers, std::string_view(layer), &std::pair<std::string_view, const char*>::first);
    if (found == layers.end() || (state != "on" && state != "off")) {
        return std::unexpected(std::format("expected 'bounds|rings|rings-all|labels on|off', got '{}'", args));
    }
    return configValues({{found->second, state}});
}

// The debug camera's state: its pose and the trail's length (its switches are the config record's tools.debugcamera.*).
auto debugCameraJson(const DebugCameraState& debug) -> Json {
    return {
        {"position", Json(debug.camera.position)},
        {"yaw", debug.camera.yaw},
        {"pitch", debug.camera.pitch},
        {"trailPoints", debug.trailPoints.size()},
    };
}

// The camera a LOD inspection measures from: the camera LOD is decided from, with the view's field of view and aspect.
auto lodInspectionCamera(const ViewContext& view, const ToolsState& tools) -> LodToolCamera {
    return {.eye = lodCamera(tools.debugCamera, view.cam).position, .fovY = glm::radians(cameraFovDegrees), .aspect = view.viewAspect};
}

auto streamingInspectionJson(const StreamingInspection& in) -> Json {
    const auto& row = in.row;
    auto levels = Json::array();
    for (size_t level = 0; level < row.levels.size(); level++) {
        const auto& info = row.levels[level];
        levels.push_back({{"level", level},
                          {"name", info.name},
                          {"requested", info.requested},
                          {"loaded", info.loaded},
                          {"ready", info.ready},
                          {"drawn", info.drawn},
                          {"leadSeconds", info.lead >= 0.0 ? Json(info.lead) : Json(nullptr)}});
    }
    auto heuristics = Json::array();
    for (const auto& heuristic : in.heuristics) {
        heuristics.push_back({{"domain", heuristic.domain},
                              {"kind", heuristic.kind},
                              {"path", heuristic.path},
                              {"used", heuristic.used},
                              {"why", heuristic.why},
                              {"authored", {{"thresholds", heuristic.authored}, {"unloadThresholds", heuristic.authoredUnload}}},
                              {"thresholds", heuristic.thresholds},
                              {"unloadThresholds", heuristic.unload},
                              {"measure", heuristic.measure >= 0.0f ? Json(heuristic.measure) : Json(nullptr)}});
    }
    return {
        {"path", row.path},
        {"key", row.key},
        {"rootState", streamingStateName(row.rootState)},
        {"selected", row.selected},
        {"why", row.why},
        {"state", row.state},
        {"stateWhy", in.stateWhy},
        {"resident", row.resident},
        {"residentWhy", row.residentWhy},
        {"drawn", row.drawn >= 0 ? Json(row.drawn) : Json(nullptr)},
        {"streaming", row.streamingHeuristic >= 0 ? Json{{"heuristic", row.streamingHeuristic}, {"kind", row.kind}, {"measureMetres", row.measure}, {"loadMetres", row.load}, {"unloadMetres", row.unload}} : Json(nullptr)},
        {"tuned", row.tuned},
        {"levels", levels},
        {"heuristics", heuristics},
        {"findings", row.findings},
        {"parent", in.parent.empty() ? Json(nullptr) : Json(in.parent)},
        {"children", in.children},
    };
}

auto lodInspectionJson(const LodInspection& in, double metersPerUnit) -> Json {
    auto heuristics = Json::array();
    for (const auto& heuristic : in.heuristics) {
        heuristics.push_back({
            {"kind", heuristic.kind},
            {"path", heuristic.path},
            {"used", heuristic.used},
            {"why", heuristic.why},
            {"authored", {{"thresholds", heuristic.authored}, {"blendThresholds", heuristic.authoredBlends}}},
            {"thresholds", heuristic.thresholds},
            {"blendThresholds", heuristic.blendThresholds},
            {"overridden", heuristic.overridden},
            {"measure", heuristic.measure >= 0.0f ? Json(heuristic.measure) : Json(nullptr)},
            {"picks", heuristic.picks >= 0 ? Json(heuristic.picks) : Json(nullptr)},
            {"blend", heuristic.blend},
        });
    }
    auto levels = Json::array();
    for (size_t level = 0; level < in.levels.size(); level++) {
        const auto& info = in.levels[level];
        levels.push_back({
            {"level", level},
            {"name", info.name},
            {"triangles", info.known ? Json(info.triangles) : Json(nullptr)},
            {"draws", info.known ? Json(info.draws) : Json(nullptr)},
            {"loaded", info.loaded},
        });
    }
    return {
        {"path", in.path},
        {"kind", in.renderLod ? "render" : "streaming"},
        {"group", in.group >= 0 ? Json(in.group) : Json(nullptr)},
        {"key", in.key},
        {"default", in.defaultIndex},
        {"wanted", in.wanted >= 0 ? Json(in.wanted) : Json(nullptr)},
        {"drawn", in.drawn >= 0 ? Json(in.drawn) : Json(nullptr)},
        {"fadeLevel", in.fadeLevel >= 0 ? Json(in.fadeLevel) : Json(nullptr)},
        {"fade", in.fade},
        {"setByHand", in.setByHand},
        {"forced", in.forced},
        {"why", in.why},
        {"distanceUnit", lodUnitName(metersPerUnit)},
        {"heuristics", heuristics},
        {"levels", levels},
    };
}

// The LOD tool: the selected root's inspection, the tuning, the viewport's layers and the pops seen.
auto lodToolRecord(const ViewContext& view, const ToolsState& tools) -> Json {
    const auto& window = tools.lod;
    auto inspection = Json(nullptr);
    std::string root;
    if (!view.selectedPath.empty()) {
        // The LOD root at or above the selection, as the window takes it.
        for (const auto& candidate : view.scene.lodRoots()) {
            if (primpath::isAtOrUnder(view.selectedPath, candidate.path) && candidate.path.size() > root.size()) {
                root = candidate.path;
            }
        }
        const auto& draws = *view.scene.snapshot().draws;
        for (const auto& candidate : draws.lodGroupPaths) {
            if (!candidate.empty() && primpath::isAtOrUnder(view.selectedPath, candidate) && candidate.size() > root.size()) {
                root = candidate;
            }
        }
    }
    if (!root.empty()) {
        if (auto found = inspectLodRoot(view.scene, view.latestCull, root, lodInspectionCamera(view, tools))) {
            inspection = lodInspectionJson(*found, view.scene.lodMetersPerUnit());
        }
    }
    return {
        {"selectedRoot", root.empty() ? Json(nullptr) : Json(root)},
        {"inspection", inspection},
        {"tuning", lodTuningJson(view.scene.lodTuning())},
        {"levelView", renderconfig::bufferView.get() == (size_t) GBufferView::LodLevel},
        {"pops", view.lodPops},
        {"windowRows", window.rows.size()},
    };
}

} // namespace

auto registerToolCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records, ViewContext& view, ToolsState& tools) -> void {
    view.debugCamera = [&tools]() -> Camera* {
        return toolsconfig::debugCamera.get() ? &tools.debugCamera.camera : nullptr;
    };

    registry.add(
        {
            .name = "view.camera.debug",
            .summary = "The debug camera's actions: place puts it 15 m behind and 10 m above the main camera, clearTrail forgets the main camera's trail. Its switches are configuration values (tools.debugcamera.on, controlMain, lod, cull, shadows, trail); the switches here set them, as the debug-camera verb does.",
            .params = {optionalField("place", RpcType::Bool, "put it behind and above the main camera"),
                       optionalField("clearTrail", RpcType::Bool, "forget the trail"),
                       optionalField("on", RpcType::Bool, "tools.debugcamera.on"),
                       optionalField("controlMain", RpcType::Bool, "tools.debugcamera.controlMain"),
                       optionalField("lodFromDebug", RpcType::Bool, "tools.debugcamera.lod"),
                       optionalField("cullFromDebug", RpcType::Bool, "tools.debugcamera.cull"),
                       optionalField("shadowsFromDebug", RpcType::Bool, "tools.debugcamera.shadows"),
                       optionalField("trail", RpcType::Bool, "tools.debugcamera.trail")},
            .result = "{position, yaw, pitch, trailPoints}",
        },
        [&tools](const Json& p, RpcResponder responder) {
            auto& debug = tools.debugCamera;
            for (auto [field, value] : {std::pair{"on", &toolsconfig::debugCamera},
                                        std::pair{"controlMain", &toolsconfig::debugControlMain},
                                        std::pair{"lodFromDebug", &toolsconfig::debugLod},
                                        std::pair{"cullFromDebug", &toolsconfig::debugCull},
                                        std::pair{"shadowsFromDebug", &toolsconfig::debugShadows},
                                        std::pair{"trail", &toolsconfig::debugTrail}}) {
                if (p.contains(field)) {
                    value->set(p[field].get<bool>());
                }
            }
            debug.placeRequested = debug.placeRequested || p.value("place", false);
            if (p.value("clearTrail", false)) {
                debug.trailPoints.clear();
            }
            responder.respond(debugCameraJson(debug));
        });
    registry.add(
        {
            .name = "view.lod.inspect",
            .summary = "One LOD root as the LOD window's inspector shows it: kind, authored root, state (wanted, drawn, fade, why), every heuristic (used or why not, thresholds authored and in force, its measure from the camera now and the level it would pick) and every level (triangles, draws).",
            .params = {requiredField("root", RpcType::String, "a LOD root's full path")},
            .result = "the inspection",
        },
        [&view, &tools](const Json& p, RpcResponder responder) {
            // Answered once a readback carries every group's state, as the renderlod record is.
            auto root = p["root"].get<std::string>();
            view.renderLodRequests.push_back({.askedAt = view.latestCull.lodFrame, .answer = [&view, &tools, responder, root]() mutable {
                                                  auto inspection = inspectLodRoot(view.scene, view.latestCull, root, lodInspectionCamera(view, tools));
                                                  if (!inspection) {
                                                      responder.fail(rpc::invalidParams, std::format("no LOD root placed at {}", root));
                                                      return;
                                                  }
                                                  responder.respond(lodInspectionJson(*inspection, view.scene.lodMetersPerUnit()));
                                              }});
        });
    registry.add(
        {
            .name = "view.streaming.inspect",
            .summary = "One streaming root inspected, as the Streaming window shows it: its residency (selected, state and why, resident and why, drawn, its state's name), every heuristic it names by domain with its thresholds authored and in force and its measure from the camera, its levels (requested, loaded, ready, drawn, lead), its authoring findings, its parent and children.",
            .params = {requiredField("root", RpcType::String, "a streaming LOD root's full path")},
            .result = "the inspection",
        },
        [&view, &tools](const Json& p, RpcResponder responder) {
            auto root = p["root"].get<std::string>();
            auto inspection = inspectStreamingRoot(view.scene, root, lodCamera(tools.debugCamera, view.cam).position, lodWindowArea(glm::radians(cameraFovDegrees), view.viewAspect));
            if (!inspection) {
                responder.fail(rpc::appError, std::format("no streaming root is placed at '{}'", root));
                return;
            }
            responder.respond(streamingInspectionJson(*inspection));
        });

    verbs.add("debug-camera", "view.camera.debug", parseDebugCamera);
    verbs.add("lod-show", "config.set", parseLodShow);
    verbs.add("streaming-show", "config.set", parseStreamingShow);

    records.add("debugcamera", "the debug camera: its position, yaw and pitch, and the main camera's trail points; its switches are the config record's tools.debugcamera.*", [&tools](RpcResponder responder) {
        responder.respond(debugCameraJson(tools.debugCamera));
    });
    records.add("streamingtool",
                "the Streaming tool: the inspection of the streaming root at or above the selection (residency and why, heuristics by domain, levels with lead, findings), the totals and the map's view (whether the Streaming window is open is the windows record's)",
                [&view, &tools](RpcResponder responder) {
                    // The streaming root at or above the selection: the longest root path that holds it.
                    std::string root;
                    if (!view.selectedPath.empty()) {
                        for (const auto& placed : view.scene.lodRoots()) {
                            bool atOrAbove = primpath::isAtOrUnder(view.selectedPath, placed.path);
                            if (atOrAbove && placed.path.size() > root.size() && view.scene.lodChoices().contains(placed.path)) {
                                root = placed.path;
                            }
                        }
                    }
                    auto inspection = root.empty() ? std::nullopt : inspectStreamingRoot(view.scene, root, lodCamera(tools.debugCamera, view.cam).position, lodWindowArea(glm::radians(cameraFovDegrees), view.viewAspect));
                    const auto& state = tools.streaming;
                    const auto& t = state.totals;
                    responder.respond({
                        {"selected", view.selectedPath},
                        {"inspection", inspection ? streamingInspectionJson(*inspection) : Json(nullptr)},
                        {"totals", {{"roots", t.roots}, {"residentLevels", t.residentLevels}, {"inFlight", t.inFlight}, {"lateNow", t.lateNow}, {"holesNow", t.holesNow}}},
                        {"streamingView", state.streamingView},
                    });
                });
    records.add("lodtool",
                "the LOD tool: the inspection of the LOD root at or above the selection (heuristics with their measures and picks, levels with triangles), the LOD tuning, the viewport's layers and the pops seen (next frames)",
                [&view, &tools](RpcResponder responder) {
                    view.renderLodRequests.push_back({.askedAt = view.latestCull.lodFrame, .answer = [&view, &tools, responder]() mutable { responder.respond(lodToolRecord(view, tools)); }});
                });
}
