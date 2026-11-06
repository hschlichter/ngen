#include "viewcommands.h"

#include "assetclient.h"
#include "camera.h"
#include "renderthread.h"
#include "renderworld.h"
#include "trace.h"
#include "viewpicking.h"
#include "viewscene.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <expected>
#include <format>
#include <map>
#include <sstream>

// The scene's commands: selection, picking, a prim's transform, purposes, residency, repacking and the assets drawn; and the records of the
// scene, the scene thread and the GPU scene's flow, what drawing reached (instancers, variants, payloads) and the assets asked for.

namespace {

using rpc::Json;

auto parsePrim(const std::string& args) -> std::expected<Json, std::string> {
    return Json{{"prim", args}};
}

auto parseTranslate(const std::string& args) -> std::expected<Json, std::string> {
    auto space = args.find(' ');
    glm::vec3 offset(0.0f);
    bool parsed = space != std::string::npos && std::sscanf(args.c_str() + space + 1, "%f,%f,%f", &offset.x, &offset.y, &offset.z) == 3;
    if (!parsed) {
        return std::unexpected(std::format("expected '/prim dx,dy,dz', got '{}'", args));
    }
    return Json{{"prim", args.substr(0, space)}, {"offset", Json(offset)}};
}

auto parsePick(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    int x = -1;
    int y = -1;
    if (!(in >> x >> y) || x < 0 || y < 0) {
        return std::unexpected(std::format("expected '<x> <y>' in framebuffer pixels, got '{}'", args));
    }
    return Json{{"x", x}, {"y", y}};
}

auto parseRepack(const std::string& args) -> std::expected<Json, std::string> {
    if (args == "scene") {
        return Json{{"scene", true}};
    }
    std::istringstream in(args);
    Json ids = Json::array();
    std::string id;
    while (in >> id) {
        ids.push_back(id);
    }
    if (ids.empty()) {
        return std::unexpected(std::format("expected 'scene' or asset ids, got '{}'", args));
    }
    return Json{{"ids", ids}};
}

auto parseGoToAsset(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string id;
    std::string mode;
    in >> id >> mode;
    if (id.empty() || (!mode.empty() && mode != "all")) {
        return std::unexpected(std::format("expected '<asset id> [all]', got '{}'", args));
    }
    return Json{{"id", id}, {"all", mode == "all"}};
}

auto parsePurposes(const std::string& args) -> std::expected<Json, std::string> {
    Json params = Json::object();
    for (const auto& [key, value] : RpcVerbs::items(args)) {
        if ((key != "guide" && key != "proxy") || (value != "on" && value != "off")) {
            return std::unexpected(std::format("expected guide=on|off,proxy=on|off, got '{}={}'", key, value));
        }
        params[key] = value == "on";
    }
    return params;
}

auto cameraModesJson(const CameraModesState& modes) -> Json {
    Json ground = nullptr;
    if (modes.ground) {
        ground = {{"point", Json(*modes.ground)}, {"prim", modes.groundPrim}};
    }
    return {
        {"follow",
         {{"ground", ground},
          {"tested", {{"chunks", modes.groundChunks}, {"draws", modes.groundDraws}, {"triangles", modes.groundTriangles}, {"ms", modes.groundMs}}}}},
        {"orbit", modes.orbiting ? Json{{"pivot", Json(modes.pivot)}, {"picked", modes.pivotPicked}} : Json(nullptr)},
    };
}

auto statusRecord(const ViewContext& view) -> Json {
    return {
        {"frame", view.frameCounter},
        {"scene", view.sceneLabel},
        {"selected", view.selectedPath},
        {"camera", {{"position", Json(view.cam.position)}, {"yaw", view.cam.yaw}, {"pitch", view.cam.pitch}, {"speed", view.cam.speed}}},
        {"cameraModes", cameraModesJson(view.cameraModes)},
    };
}

auto sceneThreadRecord(const ViewContext& view) -> Json {
    const auto& t = view.scene.threadStats();
    const auto& r = view.residency;
    auto [posted, applied] = view.scene.requestCounts();
    return {
        {"requests", {{"posted", posted}, {"applied", applied}}},
        {"batches", t.batches},
        {"compose",
         {{"count", t.composes},
          {"lastMs", t.lastComposeMs},
          {"needed", t.composeNeeded},
          {"last",
           {{"ms", t.lastComposeCallMs},
            {"stageMs", t.composeStageMs},
            {"resolveMs", t.composeResolveMs},
            {"finishMs", t.composeFinishMs},
            {"prototypesComposed", t.prototypesComposed},
            {"levelsComposed", t.levelsComposed},
            {"contentsReached", t.contentsReached}}},
          {"held", {{"prototypes", t.prototypesHeld}, {"levels", t.levelsHeld}}}}},
        {"placement",
         {{"passes", t.places},
          {"needed", t.placeNeeded},
          {"placements", t.placements},
          {"drawn", t.placementsDrawn},
          {"walksBuilt", t.walksBuilt},
          {"walksKept", t.walksKept},
          {"lodGroups", t.lodGroups},
          {"last", {{"ms", t.lastPlaceMs}, {"built", t.placementsBuilt}, {"slotsAdded", t.slotsAdded}, {"slotsRemoved", t.slotsRemoved}, {"slotsUpdated", t.slotsUpdated}, {"lodGroupsChanged", t.lodGroupsChanged}}}}},
        {"publishMs", t.publishMs},
        {"assetsReceived", t.assetsReceived},
        {"inFlight", {{"layers", t.inFlightLayers}, {"meshes", t.inFlightMeshes}, {"materials", t.inFlightMaterials}, {"textures", t.inFlightTextures}, {"other", t.inFlightOther}}},
        {"held", {{"layerPacks", t.layerPacks}, {"meshPacks", t.meshPacks}, {"materialPacks", t.materialPacks}, {"texturePacks", t.texturePacks}, {"failed", t.failedAssets}}},
        {"library", {{"meshes", t.meshEntries}, {"materials", t.materialEntries}, {"textures", t.textureEntries}}},
        {"unloaded",
         {{"levels", t.releasedLevels},
          {"contents", t.releasedContents},
          {"meshPacks", t.releasedMeshes},
          {"meshBytes", t.releasedMeshBytes}}},
        {"sinceOpenMs", t.sinceOpenMs},
        {"idle", view.scene.idle()},
        {"renderThreadDrawn", view.renderThread.sceneDrawn()},
        {"resident", r.resident},
        {"everResident", r.everResident},
        {"scriptHeld", view.script.held},
        {"scriptFrame", view.script.frame},
        {"loading", {{"frames", r.loadingFrames}, {"longestFrameMs", r.longestLoadingFrameMs}}},
        {"lastResident", {{"frame", r.lastResidentFrame}, {"ms", r.lastResidentMs}, {"loadFrames", r.lastLoadFrames}, {"longestFrameMs", r.lastLoadLongestMs}}},
    };
}

auto gpuSceneRecord(const ViewContext& view) -> Json {
    auto s = view.renderThread.latestSceneGpuStats();
    const auto& u = s.uploads;
    const auto& p = s.geometryPool;
    return {
        {"frame", s.frame},
        {"changeLists", {{"submitted", s.changeListsSubmitted}, {"drawn", s.changeListsDrawn}}},
        {"gpuIdle", s.gpuIdle},
        {"tables", {{"drawSlots", s.drawSlots}, {"meshes", s.meshes}, {"materials", s.materials}, {"textures", s.textures}}},
        {"uploads",
         {{"queued", u.queued},
          {"queuedBytes", u.queuedBytes},
          {"budget", u.budget},
          {"last", {{"uploads", u.lastUploads}, {"bytes", u.lastBytes}, {"frame", u.lastFrame}}},
          {"totalBytes", u.totalBytes},
          {"texturesWaitingSlot", u.texturesWaitingSlot},
          {"lastInstanceUpload", {{"records", u.instanceRecords}, {"regions", u.instanceRegions}}}}},
        {"geometryPool",
         {{"limitBytes", p.limitBytes},
          {"vertexLimit", p.vertexLimit},
          {"indexLimit", p.indexLimit},
          {"vertexCapacity", p.vertexCapacity},
          {"vertexUsed", p.vertexUsed},
          {"indexCapacity", p.indexCapacity},
          {"indexUsed", p.indexUsed},
          {"waitingMeshes", p.waitingMeshes},
          {"waitingBytes", p.waitingBytes}}},
    };
}

// The scene's sky: its dome light as authored, its image, and what the renderer lights with (the irradiance's coefficients, and the light
// arriving at a surface facing straight up and straight down, as the lighting pass reflects it: E / π).
auto environmentRecord(const ViewContext& view) -> Json {
    const auto& environment = view.scene.snapshot().environment;
    if (!environment) {
        return {{"dome", nullptr}, {"drawn", false}};
    }
    auto vec = [](const glm::vec3& v) {
        return Json::array({v.x, v.y, v.z});
    };
    Json out = {
        {"dome", environment->prim},
        {"texture", environment->texture},
        {"textureFormat", environment->textureFormat},
        {"poleAxis", environment->poleAxis},
        {"color", vec(environment->color)},
        {"intensity", environment->intensity},
        {"exposure", environment->exposure},
        {"imageHeld", environment->imageHeld},
        {"size", Json::array({environment->width, environment->height})},
        {"domesIgnored", environment->domesIgnored},
        {"drawn", view.renderThread.latestSceneGpuStats().skyDrawn},
    };
    if (environment->placed) {
        const auto& placed = *environment->placed;
        auto coefficients = Json::array();
        for (const auto& c : placed.irradiance) {
            coefficients.push_back(vec(c));
        }
        // The irradiance at a world direction, through the image's frame (packedtexture.h's basis), as the lighting pass evaluates it.
        auto reflected = [&placed](const glm::vec3& worldNormal) {
            auto n = glm::normalize(placed.toImage * worldNormal);
            std::array<float, 9> basis = {0.282095f,
                                          0.488603f * n.y,
                                          0.488603f * n.z,
                                          0.488603f * n.x,
                                          1.092548f * n.x * n.y,
                                          1.092548f * n.y * n.z,
                                          0.315392f * ((3.0f * n.z * n.z) - 1.0f),
                                          1.092548f * n.x * n.z,
                                          0.546274f * ((n.x * n.x) - (n.y * n.y))};
            glm::vec3 e(0.0f);
            for (size_t i = 0; i < basis.size(); i++) {
                e += placed.irradiance[i] * basis[i];
            }
            return glm::max(e, glm::vec3(0.0f)) * placed.radiance / glm::pi<float>();
        };
        auto up = view.scene.worldUp();
        out["radiance"] = vec(placed.radiance);
        out["irradiance"] = coefficients;
        out["lightUp"] = vec(reflected(up));
        out["lightDown"] = vec(reflected(-up));
    }
    return out;
}

auto sceneRecord(const ViewContext& view) -> Json {
    const auto& snapshot = view.scene.snapshot();
    auto lodLevels = Json::object();
    for (const auto& [root, level] : *snapshot.lodLevels) {
        lodLevels[root] = level;
    }
    return {
        {"scene", view.sceneLabel},
        {"layerStack", *snapshot.layerStack},
        {"upAxis", snapshot.upAxis},
        {"metersPerUnit", snapshot.metersPerUnit},
        {"up", Json(view.scene.worldUp())},
        {"prims", snapshot.prims},
        {"prototypes", snapshot.prototypes},
        {"meshInstances", view.scene.renderWorld().meshInstances.size()},
        {"instancers", view.scene.instancers().size()},
        {"instancerPoints", std::ranges::fold_left(view.scene.instancers(), (uint64_t) 0, [](uint64_t sum, const SceneInstancer& instancer) { return sum + instancer.points; })},
        {"primsWithVariants", view.scene.variantPrims().size()},
        {"lights", view.scene.renderWorld().lights.size()},
        {"lodLevels", lodLevels},
    };
}

auto payloadsRecord(const ViewContext& view) -> Json {
    auto placements = Json::array();
    size_t loaded = 0;
    size_t withoutBounds = 0;
    for (const auto& payload : view.scene.payloads()) {
        auto bounds = Json(nullptr);
        if (payload.worldBounds) {
            bounds = {{"min", Json(payload.worldBounds->min)}, {"max", Json(payload.worldBounds->max)}};
        } else {
            withoutBounds++;
        }
        if (payload.loaded) {
            loaded++;
        }
        placements.push_back({{"path", payload.path}, {"assets", payload.assetIds}, {"loaded", payload.loaded}, {"worldBounds", bounds}});
    }
    return {{"count", placements.size()}, {"loaded", loaded}, {"withoutBounds", withoutBounds}, {"placements", placements}};
}

auto instancersRecord(const ViewContext& view) -> Json {
    auto instancers = Json::array();
    uint64_t points = 0;
    uint64_t visible = 0;
    uint64_t primsDrawn = 0;
    for (const auto& instancer : view.scene.instancers()) {
        points += instancer.points;
        visible += instancer.visiblePoints;
        primsDrawn += instancer.primsDrawn;
        instancers.push_back({{"path", instancer.path}, {"points", instancer.points}, {"visiblePoints", instancer.visiblePoints}, {"prototypes", instancer.prototypes}, {"primsDrawn", instancer.primsDrawn}});
    }
    return {{"count", instancers.size()}, {"points", points}, {"visiblePoints", visible}, {"primsDrawn", primsDrawn}, {"instancers", instancers}};
}

auto variantsRecord(const ViewContext& view) -> Json {
    auto prims = Json::array();
    std::map<std::string, std::map<std::string, uint64_t>> bySet;
    for (const auto& prim : view.scene.variantPrims()) {
        auto selections = Json::object();
        for (const auto& [set, variant] : prim.selections) {
            selections[set] = variant;
            bySet[set][variant]++;
        }
        prims.push_back({{"path", prim.path}, {"selections", selections}});
    }
    auto sets = Json::object();
    for (const auto& [set, variants] : bySet) {
        auto counts = Json::object();
        for (const auto& [variant, count] : variants) {
            counts[variant.empty() ? "(none)" : variant] = count;
        }
        sets[set] = counts;
    }
    return {{"count", prims.size()}, {"sets", sets}, {"prims", prims}};
}

auto assetsRecord(const ViewContext& view) -> Json {
    auto stats = view.assets.stats();
    auto assets = Json::array();
    for (const auto& entry : view.assets.log()) {
        const char* state = entry.state == AssetLogState::Received ? "received" : (entry.state == AssetLogState::Failed ? "failed" : "inFlight");
        auto total = entry.doneMs >= 0 && entry.requestedMs >= 0 ? entry.doneMs - entry.requestedMs : -1.0;
        assets.push_back({
            {"id", entry.id},
            {"state", state},
            {"packed", entry.packed},
            {"version", entry.version},
            {"bytes", entry.bytes},
            {"requests", entry.requests},
            {"requestedMs", entry.requestedMs},
            {"firstDataMs", entry.firstDataMs},
            {"doneMs", entry.doneMs},
            {"totalMs", total},
            {"serverMs", entry.serverMs},
            {"errors", entry.errors},
            {"stale", entry.stale},
            {"changes", entry.changes},
        });
    }
    return {
        {"requested", stats.requested},
        {"received", stats.received},
        {"packed", stats.packed},
        {"cached", stats.cached},
        {"failed", stats.failed},
        {"inFlight", stats.inFlight()},
        {"bytes", stats.bytes},
        {"assets", assets},
    };
}

} // namespace

auto registerSceneCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records, ViewContext& view) -> void {
    registry.add(
        {
            .name = "view.select",
            .summary = "Select a prim by its full path (instances expanded), as clicking it does; an empty path deselects.",
            .params = {requiredField("prim", RpcType::String, "prim path, or empty")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto path = p["prim"].get<std::string>();
            if (!path.empty() && !view.scene.transform(path)) {
                responder.fail(rpc::appError, std::format("no prim at '{}' is drawn", path));
                return;
            }
            view.selectedPath = path;
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.transform",
            .summary = "A drawn prim's transforms: local, and world in metres (position, rotation as a quaternion x y z w, scale), and whether it takes a transform (view.translate).",
            .params = {requiredField("prim", RpcType::String, "prim path")},
            .result = "{local, world, xformable}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto path = p["prim"].get<std::string>();
            auto xf = view.scene.transform(path);
            if (!xf) {
                responder.fail(rpc::appError, std::format("no drawn prim at '{}'", path));
                return;
            }
            auto json = [](const glm::mat4& m) {
                auto t = decomposeTransform(m);
                return Json{{"position", Json(t.position)}, {"rotation", {t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w}}, {"scale", Json(t.scale)}};
            };
            responder.respond({{"local", json(xf->local)}, {"world", json(xf->world)}, {"xformable", xf->xformable}});
        });
    registry.add(
        {
            .name = "view.translate",
            .summary = "Offset a prim's local position, as a gizmo drag does: the view's own opinion until it closes, never saved.",
            .params = {
                requiredField("prim", RpcType::String, "prim path"),
                requiredField("offset", RpcType::Vec3, "added to the local position"),
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto path = p["prim"].get<std::string>();
            auto xf = view.scene.transform(path);
            if (!xf || !xf->xformable) {
                responder.fail(rpc::appError, std::format("no prim at '{}' that takes a transform is drawn", path));
                return;
            }
            const auto& o = p["offset"];
            auto local = decomposeTransform(xf->local);
            local.position += glm::vec3(o[0].get<float>(), o[1].get<float>(), o[2].get<float>());
            view.scene.setLocalTransform(path, local.toMat4());
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.pick",
            .summary = "Pick at a framebuffer pixel as a click does: the instance there is read back from the GPU and its prim selected.",
            .params = {
                requiredField("x", RpcType::Int, "pixel column"),
                requiredField("y", RpcType::Int, "pixel row"),
            },
            .result = "{prim} a few frames later; empty when nothing is drawn there",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto x = (uint32_t) std::max(0, p["x"].get<int>());
            auto y = (uint32_t) std::max(0, p["y"].get<int>());
            view.picking.request(x, y, [&view, responder](std::optional<uint32_t> instance) mutable {
                view.selectedPath = instance ? view.scene.instancePath(*instance) : std::string();
                TRACE_EVENT("Engine", "Picked", view.selectedPath.empty() ? "(nothing)" : view.selectedPath)
                    .text(view.selectedPath.empty() ? std::string("picked nothing") : std::format("picked {}", view.selectedPath))
                    .field("prim", view.selectedPath);
                responder.respond({{"prim", view.selectedPath}});
            });
        });
    registry.add(
        {
            .name = "view.purposes",
            .summary = "Draw the prims of the guide and proxy purposes, or not; default and render prims are always drawn. Answers once the change is drawn.",
            .params = {
                optionalField("guide", RpcType::Bool, "guides drawn"),
                optionalField("proxy", RpcType::Bool, "proxies drawn"),
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            if (p.contains("guide")) {
                viewconfig::purposeGuide.set(p["guide"].get<bool>());
                view.scene.setPurposeShown(Purpose::Guide, p["guide"].get<bool>());
            }
            if (p.contains("proxy")) {
                viewconfig::purposeProxy.set(p["proxy"].get<bool>());
                view.scene.setPurposeShown(Purpose::Proxy, p["proxy"].get<bool>());
            }
            view.residentWaiters.push_back({.responder = std::move(responder), .result = Json::object()});
        });
    registry.add(
        {
            .name = "view.waitResident",
            .summary = "Answers once the scene is resident: everything it wants loaded, uploaded and drawn. A script holds its frame count until then.",
            .params = {},
            .result = "{}",
        },
        [&view](const Json&, RpcResponder responder) {
            view.residentWaiters.push_back({.responder = std::move(responder), .result = Json::object()});
        });
    registry.add(
        {
            .name = "view.repack",
            .summary = "Have the asset server repack assets even when its cache is up to date, and rebuild the scene from them. Answers once the scene is resident with them drawn.",
            .params = {
                optionalField("ids", RpcType::Array, "asset ids"),
                optionalField("scene", RpcType::Bool, "every asset of the scene"),
            },
            .result = "{assets}",
        },
        [&view](const Json& p, RpcResponder responder) {
            std::vector<std::string> ids;
            if (p.value("scene", false)) {
                ids = view.scene.assetIds();
            } else if (p.contains("ids") && p["ids"].is_array()) {
                for (const auto& id : p["ids"]) {
                    ids.push_back(id.get<std::string>());
                }
            }
            if (ids.empty()) {
                responder.fail(rpc::invalidParams, "expected ids or scene");
                return;
            }
            view.scene.refresh(ids, true);
            view.residentWaiters.push_back({.responder = std::move(responder), .result = {{"assets", ids.size()}}});
        });
    registry.add(
        {
            .name = "view.gotoAsset",
            .summary = "Move the camera to an asset's draws: the next of them (the nearest first), framed and selected, or all of them framed.",
            .params = {
                requiredField("id", RpcType::String, "asset id: a mesh, material, texture or USD file"),
                optionalField("all", RpcType::Bool, "frame every draw together"),
            },
            .result = "{prim, index, count}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto result = goToAsset(view.goTo, view.cam, view.scene, view.selectedPath, p["id"].get<std::string>(), p.value("all", false));
            if (!result.error.empty()) {
                responder.fail(rpc::appError, result.error);
                return;
            }
            responder.respond({{"prim", result.prim}, {"index", result.index}, {"count", result.count}});
        });
    registry.add(
        {
            .name = "view.assetsOf",
            .summary = "The assets used by what is drawn at or under a prim: meshes, the USD files defining them, materials and their textures.",
            .params = {optionalField("prim", RpcType::String, "prim path; the selection when left out")},
            .result = "{prim, assets}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto path = p.value("prim", std::string());
            if (path.empty()) {
                path = view.selectedPath;
            }
            if (path.empty()) {
                responder.fail(rpc::invalidParams, "no prim given and none selected");
                return;
            }
            auto assets = view.scene.assetsUnder(path);
            TRACE_EVENT("Engine", "AssetsOf", path).text(std::format("{} uses {} assets", path, assets.size())).field("assets", (int64_t) assets.size());
            responder.respond({{"prim", path}, {"assets", assets}});
        });
    registry.add({.name = "view.quit", .summary = "Stop the view after this frame.", .result = "{}"}, [&view](const Json&, RpcResponder responder) {
        view.quit = true;
        responder.respond(Json::object());
    });

    verbs.add("select", "view.select", parsePrim);
    verbs.add("translate", "view.translate", parseTranslate);
    verbs.add("pick", "view.pick", parsePick);
    verbs.add("purposes", "view.purposes", parsePurposes);
    verbs.add("repack", "view.repack", parseRepack);
    verbs.add("goto-asset", "view.gotoAsset", parseGoToAsset);
    verbs.add("assets-of", "view.assetsOf", parsePrim);
    verbs.add("wait-resident", "view.waitResident", RpcVerbs::none);
    verbs.add("quit", "view.quit", RpcVerbs::none);

    records.add("status", "frame counter, scene, selection, the main camera and the camera modes; the render settings are the config record's, the debug camera's the debugcamera record", [&view](RpcResponder responder) {
        responder.respond(statusRecord(view));
    });
    records.add("scene", "the open scene: its layer stack, up axis and units, prims (instances expanded), prototypes, draws, lights and LOD levels set", [&view](RpcResponder responder) {
        responder.respond(sceneRecord(view));
    });
    records.add("environment", "the scene's sky: its dome light, the image, whether it is drawn, and the light it casts (irradiance, up and down)", [&view](RpcResponder responder) {
        responder.respond(environmentRecord(view));
    });
    records.add("scenethread", "the scene thread: requests, composes and placement passes, assets in flight and held, and residency (the script's hold, the load's longest frame)", [&view](RpcResponder responder) {
        responder.respond(sceneThreadRecord(view));
    });
    records.add("gpuscene", "the GPU scene's flow: change lists taken and drawn, the upload queue and its budget, the geometry pool and what waits for room", [&view](RpcResponder responder) {
        responder.respond(gpuSceneRecord(view));
    });
    records.add("instancers", "every PointInstancer reached while drawing: its points and those shown, its prototypes, and the prims its points draw", [&view](RpcResponder responder) {
        responder.respond(instancersRecord(view));
    });
    records.add("variants", "every prim with variant sets reached while drawing: each set and the variant it composed to, and per set how many prims chose each variant", [&view](RpcResponder responder) {
        responder.respond(variantsRecord(view));
    });
    records.add("payloads", "every payload placement reached while drawing: its prim, asset ids, whether loaded, and its extentsHint in world metres", [&view](RpcResponder responder) {
        responder.respond(payloadsRecord(view));
    });
    records.add("assets", "what the view asked the asset server for and received: totals, and every asset with its state, size and times", [&view](RpcResponder responder) {
        responder.respond(assetsRecord(view));
    });
}
