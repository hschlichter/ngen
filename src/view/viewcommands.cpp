#include "viewcommands.h"

#include "assetclient.h"
#include "camera.h"
#include "debugview.h"
#include "drawlists.h"
#include "profile.h"
#include "renderdoccapture.h"
#include "renderer.h"
#include "renderthread.h"
#include "renderworld.h"
#include "rpcdiscovery.h"
#include "shadowcascades.h"
#include "trace.h"
#include "viewdumps.h"
#include "viewpicking.h"
#include "viewscene.h"
#include "viewui.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unistd.h>

namespace {

using rpc::Json;

constexpr std::array<const char*, 11> viewModeNames = {"lit", "albedo", "normals", "depth", "shadowfactor", "shadowmap", "shadowuv", "worldpos", "miplevel", "cascades", "lodlevel"};

auto enumValues(auto names) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& n : names) {
        out.emplace_back(n);
    }
    return out;
}

auto vec3Json(const glm::vec3& v) -> Json {
    return Json::array({v.x, v.y, v.z});
}

// A target from optional "path"/"dir" parameters and the call's responder.
auto dumpTarget(const Json& params, const char* key, RpcResponder responder) -> DumpTarget {
    return {.path = params.value(key, std::string{}), .responder = std::move(responder)};
}

// `key=value,key=value` items, as the sampler, shadow and overlay verbs take them.
auto splitItems(const std::string& args) -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> items;
    size_t start = 0;
    while (start < args.size()) {
        auto comma = args.find(',', start);
        auto item = args.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        auto eq = item.find('=');
        items.emplace_back(item.substr(0, eq), eq == std::string::npos ? std::string{} : item.substr(eq + 1));
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return items;
}

// ---- Script verbs: text in, method parameters out. Errors are the message after "<verb>: ".

using ScriptParser = std::expected<Json, std::string> (*)(const std::string& args);

struct ScriptVerb {
    const char* verb;
    const char* method;
    ScriptParser parse;
};

auto parseCamera(const std::string& args) -> std::expected<Json, std::string> {
    float pose[5] = {};
    if (!parseCameraPose(args, pose)) {
        return std::unexpected(std::format("expected x,y,z,yaw,pitch, got '{}'", args));
    }
    return Json{{"x", pose[0]}, {"y", pose[1]}, {"z", pose[2]}, {"yaw", pose[3]}, {"pitch", pose[4]}};
}

auto parseTarget(const std::string& args) -> std::expected<Json, std::string> {
    return Json{{"target", args}};
}

auto parseCameraPath(const std::string& args) -> std::expected<Json, std::string> {
    return Json{{"camera", args}};
}

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
    return Json{{"prim", args.substr(0, space)}, {"offset", vec3Json(offset)}};
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
    for (const auto& [key, value] : splitItems(args)) {
        if ((key != "guide" && key != "proxy") || (value != "on" && value != "off")) {
            return std::unexpected(std::format("expected guide=on|off,proxy=on|off, got '{}={}'", key, value));
        }
        params[key] = value == "on";
    }
    return params;
}

auto parseLodRoot(const std::string& args) -> std::expected<Json, std::string> {
    return Json{{"root", args}};
}

auto parseLod(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string root;
    int level = -1;
    if (!(in >> root >> level) || level < 0) {
        return std::unexpected(std::format("expected '<LOD root path> <level>', got '{}'", args));
    }
    return Json{{"root", root}, {"level", level}};
}

auto parseViewMode(const std::string& args) -> std::expected<Json, std::string> {
    if (std::ranges::find(viewModeNames, std::string_view(args)) == viewModeNames.end()) {
        return std::unexpected(std::format("unknown mode '{}'", args));
    }
    return Json{{"mode", args}};
}

auto parseWindow(const std::string& args) -> std::expected<Json, std::string> {
    auto space = args.find(' ');
    bool open = space == std::string::npos || args.substr(space + 1) != "off";
    return Json{{"name", args.substr(0, space)}, {"open", open}};
}

auto parseDebugView(const std::string& args) -> std::expected<Json, std::string> {
    if (std::ranges::find(debugViewNames, std::string_view(args)) == debugViewNames.end()) {
        return std::unexpected(std::format("unknown view '{}'", args));
    }
    return Json{{"view", args}};
}

auto parseOverlay(const std::string& args) -> std::expected<Json, std::string> {
    Json settings = Json::object();
    size_t start = 0;
    while (start < args.size()) {
        auto comma = args.find(',', start);
        auto item = args.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        auto eq = item.find('=');
        if (eq == std::string::npos) {
            return std::unexpected(std::format("expected name=on|off, got '{}'", item));
        }
        settings[item.substr(0, eq)] = item.substr(eq + 1) == "on";
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return Json{{"settings", settings}};
}

auto parseCull(const std::string& args) -> std::expected<Json, std::string> {
    if (args == "on" || args == "off") {
        return Json{{"enabled", args == "on"}};
    }
    if (args == "freeze" || args == "unfreeze") {
        return Json{{"frozen", args == "freeze"}};
    }
    if (args == "show" || args == "hide") {
        return Json{{"showCulled", args == "show"}};
    }
    if (args.starts_with("view ")) {
        return Json{{"overlayView", std::atoi(args.c_str() + 5)}};
    }
    return std::unexpected(std::format("unknown argument '{}'", args));
}

auto parseSampler(const std::string& args) -> std::expected<Json, std::string> {
    Json params = Json::object();
    for (const auto& [key, value] : splitItems(args)) {
        if (key == "aniso") {
            params["aniso"] = std::strtof(value.c_str(), nullptr);
        } else if (key == "bias") {
            params["bias"] = std::strtof(value.c_str(), nullptr);
        } else if (key == "minlod") {
            params["minLod"] = std::strtof(value.c_str(), nullptr);
        } else if (key == "mip") {
            params["mip"] = value == "nearest" ? "nearest" : "linear";
        } else {
            return std::unexpected(std::format("unknown key '{}'", key));
        }
    }
    return params;
}

auto parseShadow(const std::string& args) -> std::expected<Json, std::string> {
    Json params = Json::object();
    for (const auto& [key, value] : splitItems(args)) {
        if (key == "cascades") {
            params["cascades"] = std::atoi(value.c_str());
        } else if (key == "tile") {
            params["tile"] = std::atoi(value.c_str());
        } else if (key == "lambda") {
            params["lambda"] = std::strtof(value.c_str(), nullptr);
        } else if (key == "pcf") {
            params["pcf"] = value == "on";
        } else if (key == "distance") {
            params["distance"] = std::strtof(value.c_str(), nullptr);
        } else {
            return std::unexpected(std::format("unknown key '{}'", key));
        }
    }
    return params;
}

// "sky=r,g,b ground=r,g,b", either or both, or "reset".
auto parseAmbient(const std::string& args) -> std::expected<Json, std::string> {
    Json params = Json::object();
    std::istringstream in(args);
    std::string item;
    while (in >> item) {
        if (item == "reset") {
            params["reset"] = true;
            continue;
        }
        auto equals = item.find('=');
        glm::vec3 colour(0.0f);
        bool parsed = equals != std::string::npos && std::sscanf(item.c_str() + equals + 1, "%f,%f,%f", &colour.x, &colour.y, &colour.z) == 3;
        auto key = item.substr(0, equals);
        if (!parsed || (key != "sky" && key != "ground")) {
            return std::unexpected(std::format("expected 'sky=r,g,b', 'ground=r,g,b' or 'reset', got '{}'", item));
        }
        params[key] = vec3Json(colour);
    }
    return params;
}

auto parseGpuScene(const std::string& args) -> std::expected<Json, std::string> {
    Json params = Json::object();
    for (const auto& [key, value] : splitItems(args)) {
        if (key == "budget") {
            params["uploadBudgetMiB"] = std::atoi(value.c_str());
        } else if (key == "limit") {
            params["geometryLimitMiB"] = std::atoi(value.c_str());
        } else {
            return std::unexpected(std::format("unknown key '{}'", key));
        }
    }
    return params;
}

auto parsePrepass(const std::string& args) -> std::expected<Json, std::string> {
    if (args != "on" && args != "off") {
        return std::unexpected(std::format("unknown argument '{}'", args));
    }
    return Json{{"enabled", args == "on"}};
}

auto parseInspect(const std::string& args) -> std::expected<Json, std::string> {
    if (args == "off") {
        return Json{{"enabled", false}};
    }
    std::istringstream in(args);
    int texture = 0;
    int level = 0;
    if (!(in >> texture >> level)) {
        return std::unexpected(std::format("expected '<texture> <level>' or 'off', got '{}'", args));
    }
    return Json{{"enabled", true}, {"texture", texture}, {"level", level}};
}

auto parseDumpTexture(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    int texture = 0;
    int level = 0;
    std::string path;
    if (!(in >> texture >> level >> path)) {
        return std::unexpected(std::format("expected '<texture> <level> <path>', got '{}'", args));
    }
    return Json{{"texture", texture}, {"level", level}, {"path", path}};
}

auto parsePath(const std::string& args) -> std::expected<Json, std::string> {
    return Json{{"path", args}};
}

auto parseRecord(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string name;
    std::string path;
    std::string extra;
    if (!(in >> name >> path) || (in >> extra)) {
        return std::unexpected(std::format("expected '<record> <file>', such as 'render records.jsonl', got '{}'", args));
    }
    return Json{{"name", name}, {"path", path}};
}

auto parseDir(const std::string& args) -> std::expected<Json, std::string> {
    return Json{{"dir", args}};
}

auto parseNothing(const std::string&) -> std::expected<Json, std::string> {
    return Json::object();
}

auto parseCapture(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string pass;
    std::string resource;
    std::string path;
    in >> pass >> resource >> path;
    // Optional texture region: "X Y W H".
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    in >> x >> y >> width >> height;
    if (path.empty()) {
        return std::unexpected(std::format("expected '<pass|-> <resource> <path> [x y w h]', got '{}'", args));
    }
    Json params = {{"pass", pass}, {"resource", resource}, {"path", path}};
    if (width > 0) {
        params["region"] = {{"x", x}, {"y", y}, {"width", width}, {"height", height}};
    }
    return params;
}

constexpr std::array scriptVerbs = {
    ScriptVerb{"camera", "view.camera.set", parseCamera},
    ScriptVerb{"camera-frame", "view.camera.frame", parseTarget},
    ScriptVerb{"camera-use", "view.camera.use", parseCameraPath},
    ScriptVerb{"select", "view.select", parsePrim},
    ScriptVerb{"translate", "view.translate", parseTranslate},
    ScriptVerb{"pick", "view.pick", parsePick},
    ScriptVerb{"purposes", "view.purposes", parsePurposes},
    ScriptVerb{"repack", "view.repack", parseRepack},
    ScriptVerb{"goto-asset", "view.gotoAsset", parseGoToAsset},
    ScriptVerb{"assets-of", "view.assetsOf", parsePrim},
    ScriptVerb{"lod", "view.lod.set", parseLod},
    ScriptVerb{"lod-reset", "view.lod.reset", parseLodRoot},
    ScriptVerb{"wait-resident", "view.waitResident", parseNothing},
    ScriptVerb{"view", "view.mode", parseViewMode},
    ScriptVerb{"window", "view.window", parseWindow},
    ScriptVerb{"debugview", "view.debugview", parseDebugView},
    ScriptVerb{"overlay", "view.overlay", parseOverlay},
    ScriptVerb{"cull", "view.cull", parseCull},
    ScriptVerb{"sampler", "view.sampler", parseSampler},
    ScriptVerb{"inspect", "view.inspect", parseInspect},
    ScriptVerb{"dump-texture", "view.dumpTexture", parseDumpTexture},
    ScriptVerb{"shadow", "view.shadow", parseShadow},
    ScriptVerb{"prepass", "view.prepass", parsePrepass},
    ScriptVerb{"ambient", "view.ambient", parseAmbient},
    ScriptVerb{"gpuscene", "view.gpuscene.set", parseGpuScene},
    ScriptVerb{"screenshot", "view.screenshot", parsePath},
    ScriptVerb{"renderdoc-capture", "renderdoc.capture", parseNothing},
    ScriptVerb{"capture", "capture.request", parseCapture},
    ScriptVerb{"dump-frame", "introspect.frame", parseDir},
    ScriptVerb{"dump-gpuscene", "introspect.gpuscene", parseDir},
    ScriptVerb{"dump-profile", "introspect.profile", parsePath},
    ScriptVerb{"record", "introspect.record", parseRecord},
    ScriptVerb{"quit", "view.quit", parseNothing},
};

// ---- Helpers for handlers.

auto optionalField(const std::string& name, RpcType type, std::string description) -> RpcField {
    return {.name = name, .type = type, .required = false, .description = std::move(description)};
}

auto requiredField(const std::string& name, RpcType type, std::string description) -> RpcField {
    return {.name = name, .type = type, .required = true, .description = std::move(description)};
}

auto registerCamera(RpcRegistry& registry, ViewContext& view) -> void {
    registry.add(
        {
            .name = "view.camera.set",
            .summary = "Place the camera: position and yaw/pitch in degrees.",
            .params = {
                requiredField("x", RpcType::Float, "position x"),
                requiredField("y", RpcType::Float, "position y"),
                requiredField("z", RpcType::Float, "position z"),
                requiredField("yaw", RpcType::Float, "degrees"),
                requiredField("pitch", RpcType::Float, "degrees"),
                optionalField("speed", RpcType::Float, "fly speed, m/s"),
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.cam.position = glm::vec3(p["x"].get<float>(), p["y"].get<float>(), p["z"].get<float>());
            view.cam.yaw = p["yaw"].get<float>();
            view.cam.pitch = p["pitch"].get<float>();
            if (p.contains("speed")) {
                view.cam.speed = std::clamp(p["speed"].get<float>(), 0.1f, 5000.0f);
            }
            responder.respond(Json::object());
        });
    registry.add({.name = "view.camera.get", .summary = "The camera's position and yaw/pitch.", .result = "{x, y, z, yaw, pitch}"}, [&view](const Json&, RpcResponder responder) {
        responder.respond({{"x", view.cam.position.x}, {"y", view.cam.position.y}, {"z", view.cam.position.z}, {"yaw", view.cam.yaw}, {"pitch", view.cam.pitch}});
    });
    registry.add({.name = "view.camera.list", .summary = "The cameras the scene authors, in scene order.", .result = "{cameras: [{path, x, y, z, yaw, pitch}]}"},
                 [&view](const Json&, RpcResponder responder) {
                     auto list = Json::array();
                     for (const auto& camera : view.scene.cameras()) {
                         Camera placed = view.cam;
                         placed.place(camera.worldTransform);
                         list.push_back({{"path", camera.path}, {"x", placed.position.x}, {"y", placed.position.y}, {"z", placed.position.z}, {"yaw", placed.yaw}, {"pitch", placed.pitch}});
                     }
                     responder.respond({{"cameras", list}});
                 });
    registry.add(
        {
            .name = "view.camera.use",
            .summary = "Place the camera where a camera the scene authors is, looking where it looks (its roll is dropped).",
            .params = {requiredField("camera", RpcType::String, "the camera's prim path")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto path = p["camera"].get<std::string>();
            for (const auto& camera : view.scene.cameras()) {
                if (camera.path == path) {
                    view.cam.place(camera.worldTransform);
                    responder.respond(Json::object());
                    return;
                }
            }
            responder.fail(rpc::appError, std::format("the scene authors no camera at '{}'", path));
        });
    registry.add(
        {
            .name = "view.camera.frame",
            .summary = "Frame the whole scene, or what is drawn at and under a prim.",
            .params = {requiredField("target", RpcType::String, "\"scene\" or a prim path")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto target = p["target"].get<std::string>();
            if (target == "scene") {
                view.frameSceneView();
                responder.respond(Json::object());
                return;
            }
            auto bb = view.scene.bounds(target);
            if (!bb.valid()) {
                responder.fail(rpc::appError, std::format("nothing is drawn at or under '{}'", target));
                return;
            }
            view.cam.frame(bb, glm::radians(45.0f));
            responder.respond(Json::object());
        });
}

auto registerScene(RpcRegistry& registry, ViewContext& view) -> void {
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
                view.ui.setShowGuides(p["guide"].get<bool>());
                view.scene.setPurposeShown(Purpose::Guide, p["guide"].get<bool>());
            }
            if (p.contains("proxy")) {
                view.ui.setShowProxies(p["proxy"].get<bool>());
                view.scene.setPurposeShown(Purpose::Proxy, p["proxy"].get<bool>());
            }
            view.residentWaiters.push_back({.responder = std::move(responder), .result = Json::object()});
        });
    registry.add(
        {
            .name = "view.lod.set",
            .summary = "The level a LOD root wants, by its full path (instances expanded); loads it. Answers once the scene is resident with it drawn.",
            .params = {
                requiredField("root", RpcType::String, "the path of the prim with LODRootAPI"),
                requiredField("level", RpcType::Int, "0 is the finest"),
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.scene.setLodLevel(p["root"].get<std::string>(), (uint32_t) std::max(0, p["level"].get<int>()));
            view.residentWaiters.push_back({.responder = std::move(responder), .result = Json::object()});
        });
    registry.add(
        {
            .name = "view.lod.reset",
            .summary = "Puts a LOD root back to its default level (lod:default:index). Answers once the scene is resident with it drawn.",
            .params = {requiredField("root", RpcType::String, "the path of the prim with LODRootAPI")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.scene.resetLodLevel(p["root"].get<std::string>());
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
            auto result = goToAsset(view, p["id"].get<std::string>(), p.value("all", false));
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
}

auto registerDisplay(RpcRegistry& registry, ViewContext& view) -> void {
    registry.add(
        {
            .name = "view.mode",
            .summary = "The lighting pass's view mode.",
            .params = {{.name = "mode", .type = RpcType::Enum, .required = true, .description = "buffer view", .enumValues = enumValues(viewModeNames)}},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto mode = p["mode"].get<std::string>();
            auto it = std::ranges::find(viewModeNames, std::string_view(mode));
            view.ui.setGBufferViewMode((int) (it - viewModeNames.begin()));
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.debugview",
            .summary = "A debug view in place of the lit image.",
            .params = {{.name = "view", .type = RpcType::Enum, .required = true, .description = "debug view", .enumValues = enumValues(debugViewNames)}},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto name = p["view"].get<std::string>();
            auto it = std::ranges::find(debugViewNames, std::string_view(name));
            view.ui.setDebugView((int) (it - debugViewNames.begin()));
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.window",
            .summary = "Open or close an introspection window.",
            .params = {
                requiredField("name", RpcType::String, "memory, capture, framedebugger, gpuscene, counters, shaders, culling, assets"),
                optionalField("open", RpcType::Bool, "default true"),
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto name = p["name"].get<std::string>();
            if (!view.ui.setIntrospectionWindow(name, p.value("open", true))) {
                responder.fail(rpc::appError, std::format("unknown window '{}'", name));
                return;
            }
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.overlay",
            .summary = "Turn viewport overlays on or off.",
            .params = {requiredField("settings", RpcType::Object, "overlay name to bool: grid, origin, gizmo, aabbs, lightgizmos, buffer, shadow, aa, cascadefrusta")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            for (const auto& [name, on] : p["settings"].items()) {
                if (!on.is_boolean() || !view.ui.setOverlay(name, on.get<bool>())) {
                    responder.fail(rpc::appError, std::format("expected name=on|off, got '{}={}'", name, on.is_boolean() && on.get<bool>() ? "on" : "off"));
                    return;
                }
            }
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.cull",
            .summary = "GPU culling: on/off, frozen frustum, culled overlay, overlay view.",
            .params = {
                optionalField("enabled", RpcType::Bool, "culling on"),
                optionalField("frozen", RpcType::Bool, "freeze the culling frustum"),
                optionalField("showCulled", RpcType::Bool, "the red/green AABB overlay"),
                optionalField("overlayView", RpcType::Int, "0 camera, 1+ cascades"),
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            if (p.contains("enabled")) {
                view.ui.setCullEnabled(p["enabled"].get<bool>());
            }
            if (p.contains("frozen")) {
                view.ui.setCullFrozen(p["frozen"].get<bool>());
            }
            if (p.contains("showCulled")) {
                view.ui.setShowCulled(p["showCulled"].get<bool>());
            }
            if (p.contains("overlayView")) {
                view.ui.setCullOverlayView(p["overlayView"].get<int>());
            }
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.sampler",
            .summary = "The material sampler.",
            .params = {
                optionalField("aniso", RpcType::Float, "max anisotropy, 0 = off"),
                optionalField("bias", RpcType::Float, "LOD bias"),
                optionalField("minLod", RpcType::Float, "minimum LOD"),
                {.name = "mip", .type = RpcType::Enum, .required = false, .description = "mip filter", .enumValues = {"linear", "nearest"}},
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto& settings = view.ui.samplerSettingsMutable();
            if (p.contains("aniso")) {
                settings.maxAnisotropy = p["aniso"].get<float>();
            }
            if (p.contains("bias")) {
                settings.lodBias = p["bias"].get<float>();
            }
            if (p.contains("minLod")) {
                settings.minLod = p["minLod"].get<float>();
            }
            if (p.contains("mip")) {
                settings.nearestMip = p["mip"].get<std::string>() == "nearest";
            }
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.shadow",
            .summary = "Cascaded shadow settings.",
            .params = {
                optionalField("cascades", RpcType::Int, "1..4"),
                optionalField("tile", RpcType::Int, "texels per cascade, at least 64"),
                optionalField("lambda", RpcType::Float, "0 uniform .. 1 logarithmic splits"),
                optionalField("pcf", RpcType::Bool, "3x3 PCF"),
                optionalField("distance", RpcType::Float, "metres from the camera where shadows end"),
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto& settings = view.ui.shadowSettingsMutable();
            if (p.contains("cascades")) {
                settings.count = (uint32_t) std::clamp(p["cascades"].get<int>(), 1, (int) maxShadowCascades);
            }
            if (p.contains("tile")) {
                settings.tileSize = (uint32_t) std::max(64, p["tile"].get<int>());
            }
            if (p.contains("lambda")) {
                settings.splitLambda = std::clamp(p["lambda"].get<float>(), 0.0f, 1.0f);
            }
            if (p.contains("pcf")) {
                settings.pcf = p["pcf"].get<bool>();
            }
            if (p.contains("distance")) {
                settings.maxDistance = std::max(1.0f, p["distance"].get<float>());
            }
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.ambient",
            .summary = "The hemisphere ambient: sky and ground colours, as fractions of the sun's radiance; reset for the defaults.",
            .params = {
                optionalField("sky", RpcType::Vec3, "[r, g, b]"),
                optionalField("ground", RpcType::Vec3, "[r, g, b]"),
                optionalField("reset", RpcType::Bool, "back to the defaults first"),
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto& ambient = view.ui.ambientSettingsMutable();
            if (p.contains("reset") && p["reset"].get<bool>()) {
                ambient = AmbientSettings{};
            }
            if (p.contains("sky")) {
                ambient.sky = glm::vec3(p["sky"][0].get<float>(), p["sky"][1].get<float>(), p["sky"][2].get<float>());
            }
            if (p.contains("ground")) {
                ambient.ground = glm::vec3(p["ground"][0].get<float>(), p["ground"][1].get<float>(), p["ground"][2].get<float>());
            }
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.gpuscene.set",
            .summary = "GPU scene settings, from the next frame: bytes uploaded per frame, and the most each geometry pool buffer may take (0 for the device's limit; lowering it stops growth, it doesn't shrink the pool).",
            .params = {
                optionalField("uploadBudgetMiB", RpcType::Int, "MiB per SceneUpload pass, at least 1"),
                optionalField("geometryLimitMiB", RpcType::Int, "MiB per pool buffer, 0 for the device's limit"),
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            if (p.contains("uploadBudgetMiB")) {
                view.renderThread.setUploadBudget((uint64_t) std::max(1, p["uploadBudgetMiB"].get<int>()) * 1024 * 1024);
            }
            if (p.contains("geometryLimitMiB")) {
                view.renderThread.setGeometryLimit((uint64_t) std::max(0, p["geometryLimitMiB"].get<int>()) * 1024 * 1024);
            }
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.prepass",
            .summary = "The depth prepass.",
            .params = {requiredField("enabled", RpcType::Bool, "prepass on")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.ui.setDepthPrepass(p["enabled"].get<bool>());
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.inspect",
            .summary = "The texture inspector's capture of one texture level.",
            .params = {
                requiredField("enabled", RpcType::Bool, "false turns it off"),
                optionalField("texture", RpcType::Int, "texture index, as the render record's textures list them"),
                optionalField("level", RpcType::Int, "mip level"),
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            if (!p["enabled"].get<bool>()) {
                view.renderThread.setTextureInspect({});
            } else {
                view.renderThread.setTextureInspect({.enabled = true, .texture = p.value("texture", 0u), .level = p.value("level", 0u)});
            }
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.dumpTexture",
            .summary = "Write one mip level of a material texture as PNG.",
            .params = {
                requiredField("texture", RpcType::Int, "texture index, as the render record's textures list them"),
                requiredField("level", RpcType::Int, "mip level"),
                requiredField("path", RpcType::String, "PNG path"),
            },
            .result = "{} once requested; the file is written after the frame",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.renderer.requestTextureDump(p["texture"].get<uint32_t>(), p["level"].get<uint32_t>(), p["path"].get<std::string>());
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.screenshot",
            .summary = "Write the presented image as PNG; answers when the file is written.",
            .params = {
                optionalField("path", RpcType::String, "PNG path; default screenshot-<pid>-<frame>.png in the working directory"),
                optionalField("inline", RpcType::Bool, "also return the PNG as base64"),
            },
            .result = "{path, frame, width, height, png?}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto path = p.value("path", std::string{});
            if (path.empty()) {
                path = (std::filesystem::current_path() / std::format("screenshot-{}-{}.png", (int) getpid(), view.frameCounter)).string();
            }
            view.renderer.requestScreenshot(path);
            view.dumps.trackScreenshot(path, responder, p.value("inline", false));
        });
}

auto registerIntrospection(RpcRegistry& registry, ViewContext& view) -> void {
    registry.add(
        {
            .name = "introspect.frame",
            .summary = "The frame debug capture: passes, barriers with Vulkan details, commands, descriptor contents.",
            .params = {optionalField("dir", RpcType::String, "also write frame.json and every pass's written resources here")},
            .result = "the frame debug JSON",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.dumps.requestFrame(dumpTarget(p, "dir", std::move(responder)));
        });
    registry.add(
        {
            .name = "introspect.gpuscene",
            .summary = "The GPU scene joined per instance: prim, mesh, material, bounds, visibility and cull plane per view.",
            .params = {optionalField("dir", RpcType::String, "also write the tables and instances_joined.json here")},
            .result = "{views, instances}",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.dumps.requestGpuScene(dumpTarget(p, "dir", std::move(responder)));
        });
    registry.add(
        {
            .name = "introspect.profile",
            .summary = "Write the profiler history as a Chrome trace.",
            .params = {requiredField("path", RpcType::String, "trace JSON path")},
            .result = "{path}",
        },
        [](const Json& p, RpcResponder responder) {
            auto path = p["path"].get<std::string>();
            if (!profile::exportChromeTrace(path.c_str())) {
                responder.fail(rpc::appError, std::format("cannot write {}", path));
                return;
            }
            TRACE_EVENT("Engine", "ProfileWritten", path).text(std::format("profile trace written: {}", path));
            responder.respond({{"path", path}});
        });
    registry.add(
        {
            .name = "capture.request",
            .summary = "Capture a frame-graph resource right after a pass (\"\" or \"-\" = end of frame, also static buffers).",
            .params = {
                requiredField("pass", RpcType::String, "pass name, \"\" or \"-\""),
                requiredField("resource", RpcType::String, "resource name"),
                optionalField("path", RpcType::String, "also write it: PNG plus .json for textures, rows JSON for buffers"),
                optionalField("region", RpcType::Object, "{x, y, width, height} of a texture"),
            },
            .result = "texture: format, size, channel ranges, texels of small regions; buffer: schema and rows",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto pass = p["pass"].get<std::string>();
            CaptureWatch watch = {.pass = pass == "-" ? "" : pass, .resource = p["resource"].get<std::string>(), .trigger = 1};
            if (p.contains("region")) {
                const auto& r = p["region"];
                watch.regionX = r.value("x", 0u);
                watch.regionY = r.value("y", 0u);
                watch.regionWidth = r.value("width", 0u);
                watch.regionHeight = r.value("height", 0u);
            }
            view.dumps.requestCapture(watch, dumpTarget(p, "path", std::move(responder)));
        });
    registry.add({.name = "renderdoc.capture", .summary = "A RenderDoc capture of the next frame (start with --renderdoc).", .result = "{}"}, [&view](const Json&, RpcResponder responder) {
        if (!view.renderDoc.triggerCapture()) {
            responder.fail(rpc::appError, "RenderDoc is not loaded (start with --renderdoc)");
            return;
        }
        responder.respond(Json::object());
    });
}

auto statusRecord(const ViewContext& view) -> Json {
    const auto& sampler = view.ui.getSamplerSettings();
    const auto& shadow = view.ui.getShadowSettings();
    return {
        {"frame", view.frameCounter},
        {"scene", view.sceneLabel},
        {"selected", view.selectedPath},
        {"camera", {{"position", vec3Json(view.cam.position)}, {"yaw", view.cam.yaw}, {"pitch", view.cam.pitch}, {"speed", view.cam.speed}}},
        {"shadow", {{"cascades", shadow.count}, {"tile", shadow.tileSize}, {"lambda", shadow.splitLambda}, {"pcf", shadow.pcf}, {"distance", shadow.maxDistance}}},
        {"antiAliasing", view.ui.getAntiAliasing()},
        {"sampler", {{"anisotropy", sampler.maxAnisotropy}, {"lodBias", sampler.lodBias}, {"minLod", sampler.minLod}, {"nearestMip", sampler.nearestMip}}},
        {"purposes", {{"guide", view.ui.showGuides()}, {"proxy", view.ui.showProxies()}}},
        {"ambient", {{"sky", vec3Json(view.ui.ambientSettings().sky)}, {"ground", vec3Json(view.ui.ambientSettings().ground)}}},
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
        {"extract", {{"count", t.extracts}, {"lastMs", t.lastExtractMs}, {"needed", t.extractNeeded}}},
        {"assetsReceived", t.assetsReceived},
        {"inFlight", {{"layers", t.inFlightLayers}, {"meshes", t.inFlightMeshes}, {"materials", t.inFlightMaterials}, {"textures", t.inFlightTextures}, {"other", t.inFlightOther}}},
        {"held", {{"layerPacks", t.layerPacks}, {"meshPacks", t.meshPacks}, {"materialPacks", t.materialPacks}, {"texturePacks", t.texturePacks}, {"failed", t.failedAssets}}},
        {"library", {{"meshes", t.meshEntries}, {"materials", t.materialEntries}, {"textures", t.textureEntries}}},
        {"sinceOpenMs", t.sinceOpenMs},
        {"idle", view.scene.idle()},
        {"renderThreadDrawn", view.renderThread.sceneDrawn()},
        {"resident", r.resident},
        {"everResident", r.everResident},
        {"scriptHeld", r.scriptHeld},
        {"scriptFrame", r.scriptFrame},
        {"loading", {{"frames", r.loadingFrames}, {"longestFrameMs", r.longestLoadingFrameMs}}},
        {"lastResident", {{"frame", r.lastResidentFrame}, {"ms", r.lastResidentMs}, {"loadFrames", r.lastLoadFrames}, {"longestFrameMs", r.lastLoadLongestMs}}},
    };
}

auto mapRecord(const ViewContext& view) -> Json {
    auto stats = engineMapStats(view);
    auto lanes = Json::array();
    for (const auto* lane : engineMapLanes()) {
        lanes.push_back(lane);
    }
    auto nodes = Json::array();
    for (const auto& node : engineMapNodes()) {
        auto records = Json::array();
        for (const auto* record : node.records) {
            records.push_back(record);
        }
        auto windows = Json::array();
        for (const auto* window : node.windows) {
            windows.push_back(window);
        }
        auto it = stats.find(node.id);
        nodes.push_back({
            {"id", node.id},
            {"label", node.label},
            {"lane", engineMapLanes()[(size_t) node.lane]},
            {"description", node.description},
            {"records", records},
            {"windows", windows},
            {"readme", node.readme},
            {"now", it != stats.end() ? Json(it->second) : Json::array()},
        });
    }
    auto edges = Json::array();
    for (const auto& edge : engineMapEdges()) {
        edges.push_back({{"from", edge.from}, {"to", edge.to}, {"what", edge.what}});
    }
    return {{"lanes", lanes}, {"nodes", nodes}, {"edges", edges}};
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
          {"texturesWaitingSlot", u.texturesWaitingSlot}}},
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

auto sceneRecord(const ViewContext& view) -> Json {
    const auto& snapshot = view.scene.snapshot();
    auto lodLevels = Json::object();
    for (const auto& [root, level] : snapshot.lodLevels) {
        lodLevels[root] = level;
    }
    return {
        {"scene", view.sceneLabel},
        {"layerStack", snapshot.layerStack},
        {"upAxis", snapshot.upAxis},
        {"metersPerUnit", snapshot.metersPerUnit},
        {"up", vec3Json(view.scene.worldUp())},
        {"prims", snapshot.prims},
        {"prototypes", snapshot.prototypes},
        {"meshInstances", view.scene.renderWorld().meshInstances.size()},
        {"lights", view.scene.renderWorld().lights.size()},
        {"lodLevels", lodLevels},
    };
}

auto lodRecord(const ViewContext& view) -> Json {
    auto roots = Json::array();
    for (const auto& root : view.scene.lodRoots()) {
        auto heuristic = Json(nullptr);
        if (!root.heuristicType.empty()) {
            heuristic = {
                {"type", root.heuristicType},
                {"thresholds", root.thresholds},
                {"extent", {vec3Json(root.extent[0]), vec3Json(root.extent[1])}},
                {"projection", root.projectedExtent ? "projectedExtent" : "projectedSphere"},
            };
        }
        roots.push_back({
            {"path", root.path},
            {"levels", root.levels},
            {"levelPayloads", root.levelPayloads},
            {"heuristic", heuristic},
            {"default", root.defaultIndex},
            {"wanted", root.wanted},
            {"drawn", root.drawn ? Json(*root.drawn) : Json(nullptr)},
            {"loaded", root.loaded},
            {"setByHand", root.setByHand},
        });
    }
    return {{"roots", roots}};
}

auto payloadsRecord(const ViewContext& view) -> Json {
    auto placements = Json::array();
    size_t loaded = 0;
    size_t withoutBounds = 0;
    for (const auto& payload : view.scene.payloads()) {
        auto bounds = Json(nullptr);
        if (payload.worldBounds) {
            bounds = {{"min", vec3Json(payload.worldBounds->min)}, {"max", vec3Json(payload.worldBounds->max)}};
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

auto cullingRecord(const ViewContext& view) -> Json {
    const auto& cull = view.latestCull;
    auto cascades = Json::array();
    for (uint32_t i = 0; i < cull.cascadeCount; i++) {
        cascades.push_back({
            {"cascade", i},
            {"drawn", cull.cascadeDrawn[i]},
            {"culled", cull.cascadeCulled[i]},
            {"triangles", cull.cascadeTriangles[i]},
        });
    }
    return {
        {"frame", cull.frame},
        {"instances", cull.instances},
        {"camera", {{"drawn", cull.instances - cull.cameraCulled}, {"culled", cull.cameraCulled}, {"triangles", cull.cameraTriangles}}},
        {"sceneTriangles", cull.sceneTriangles},
        {"cascades", cascades},
    };
}

auto pipelinesRecord(const ViewContext& view) -> Json {
    auto pipelines = Json::array();
    for (const auto& info : view.renderer.pipelineInfos()) {
        auto shaders = Json::array();
        for (const auto& [id, version] : info.shaders) {
            shaders.push_back({{"id", id}, {"version", version}});
        }
        const char* state = info.state == PipelineState::Ready ? "ready" : (info.state == PipelineState::Failed ? "failed" : "pending");
        pipelines.push_back({
            {"name", info.name},
            {"use", info.use == PipelineUse::AtStart ? "atStart" : "firstUse"},
            {"state", state},
            {"requested", info.requested},
            {"shaders", shaders},
            {"error", info.error},
        });
    }
    return {{"pipelines", pipelines}};
}

auto profileRecord() -> Json {
    std::vector<profile::FrameStats> history;
    profile::frameHistory(history);
    auto frames = Json::array();
    double cpuSum = 0.0;
    double gpuSum = 0.0;
    size_t gpuCount = 0;
    for (const auto& frame : history) {
        frames.push_back({{"frame", frame.frameIndex}, {"cpuMs", frame.cpuMs}, {"gpuMs", frame.gpuMs}});
        cpuSum += frame.cpuMs;
        if (frame.gpuMs > 0.0) {
            gpuSum += frame.gpuMs;
            gpuCount++;
        }
    }
    double cpuAverage = history.empty() ? 0.0 : cpuSum / (double) history.size();
    double gpuAverage = gpuCount == 0 ? 0.0 : gpuSum / (double) gpuCount;
    return {
        {"frameCount", history.size()},
        {"cpuMsAverage", cpuAverage},
        {"gpuMsAverage", gpuAverage},
        {"frames", frames},
    };
}

} // namespace

auto engineMapStats(const ViewContext& view) -> EngineMapStats {
    EngineMapStats stats;
    auto assets = view.assets.stats();
    const auto& t = view.scene.threadStats();
    const auto& r = view.residency;
    auto gpu = view.renderThread.latestSceneGpuStats();
    const auto& cull = view.latestCull;
    auto mib = [](uint64_t bytes) -> double {
        return (double) bytes / (1024.0 * 1024.0);
    };
    stats["assets.packing"] = {std::format("{} packed for this view", assets.packed)};
    stats["assets.cache"] = {std::format("{} answered from the cache", assets.cached)};
    stats["assets.delivery"] = {std::format("{:.1f} MiB to this view", mib(assets.bytes))};
    stats["view.assetclient"] = {std::format("{} in flight, {} received", assets.inFlight(), assets.received), std::format("{} failed", assets.failed)};
    stats["scene.compose"] = {std::format("{} composes, last {:.1f} ms", t.composes, t.lastComposeMs), std::format("{} layer packs, {} in flight", t.layerPacks, t.inFlightLayers)};
    stats["scene.extract"] = {std::format("{} extracts, last {:.1f} ms", t.extracts, t.lastExtractMs),
                              std::format("{} meshes, {} materials, {} textures", t.meshEntries, t.materialEntries, t.textureEntries)};
    stats["scene.changes"] = {std::format("{} change lists sent", gpu.changeListsSubmitted), view.scene.idle() ? std::string("scene thread idle") : std::string("scene thread working")};
    stats["main.scene"] = {r.resident ? std::string("resident") : std::format("loading, {} frames", r.loadingFrames), std::format("script frame {}{}", r.scriptFrame, r.scriptHeld ? ", held" : "")};
    stats["main.input"] = {std::format("{:.1f} m/s", view.cam.speed)};
    stats["main.frame"] = {std::format("frame {}", view.frameCounter)};
    size_t pending = 0;
    size_t failed = 0;
    auto pipelines = view.renderer.pipelineInfos();
    for (const auto& info : pipelines) {
        if (info.state == PipelineState::Failed) {
            failed++;
        } else if (info.state != PipelineState::Ready) {
            pending++;
        }
    }
    stats["render.pipelines"] = {std::format("{} pipelines, {} pending, {} failed", pipelines.size(), pending, failed)};
    stats["render.gpuscene"] = {std::format("{} draw slots, {} meshes", gpu.drawSlots, gpu.meshes), std::format("{} materials, {} textures", gpu.materials, gpu.textures)};
    stats["render.uploads"] = {std::format("{} queued, {:.1f} MiB", gpu.uploads.queued, mib(gpu.uploads.queuedBytes)), std::format("budget {:.0f} MiB per frame", mib(gpu.uploads.budget))};
    const auto& pool = gpu.geometryPool;
    stats["render.pool"] = {std::format("{:.1f}M of {:.1f}M vertices", (double) pool.vertexUsed / 1e6, (double) pool.vertexLimit / 1e6),
                            pool.waitingMeshes > 0 ? std::format("{} meshes waiting", pool.waitingMeshes) : std::string("nothing waiting")};
    stats["render.framegraph"] = {std::format("{} of {} instances drawn", cull.instances - cull.cameraCulled, cull.instances), std::format("{:.1f}M triangles", (double) cull.cameraTriangles / 1e6)};
    return stats;
}

auto goToAsset(ViewContext& view, const std::string& assetId, bool all) -> AssetGoToResult {
    AssetGoToResult result;
    auto prims = view.scene.drawsOf(assetId);
    if (prims.empty()) {
        result.error = std::format("nothing drawn uses {}", assetId);
        return result;
    }
    result.count = prims.size();
    if (all) {
        AABB bounds = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
        for (auto prim : prims) {
            auto b = view.scene.primBounds(prim);
            if (b.valid()) {
                bounds.min = glm::min(bounds.min, b.min);
                bounds.max = glm::max(bounds.max, b.max);
            }
        }
        if (bounds.valid()) {
            view.cam.frame(bounds, glm::radians(45.0f));
        }
        view.goTo = {};
        return result;
    }
    auto& state = view.goTo;
    if (state.assetId != assetId || state.revision != view.scene.revision() || state.order.size() != prims.size()) {
        auto camera = view.cam.position;
        auto distance = [&](uint32_t prim) {
            auto b = view.scene.primBounds(prim);
            return b.valid() ? glm::length((b.min + b.max) * 0.5f - camera) : 1e30f;
        };
        std::ranges::stable_sort(prims, [&](uint32_t a, uint32_t b) { return distance(a) < distance(b); });
        state = {.assetId = assetId, .order = std::move(prims), .index = 0, .revision = view.scene.revision()};
    } else {
        state.index = (state.index + 1) % state.order.size();
    }
    auto prim = state.order[state.index];
    auto bounds = view.scene.primBounds(prim);
    if (bounds.valid()) {
        view.cam.frame(bounds, glm::radians(45.0f));
    }
    view.selectedPath = view.scene.primPath(prim);
    result.prim = view.selectedPath;
    result.index = state.index;
    return result;
}

auto registerViewMethods(RpcRegistry& registry, ViewContext& view) -> void {
    registerCamera(registry, view);
    registerScene(registry, view);
    registerDisplay(registry, view);
    registerIntrospection(registry, view);
}

auto runViewScriptCommand(const RpcRegistry& registry, const SessionCommand& command) -> void {
    const auto* verb = std::ranges::find_if(scriptVerbs, [&](const ScriptVerb& v) { return command.verb == v.verb; });
    if (verb == scriptVerbs.end()) {
        TRACE_ERROR("Engine", "ScriptCommandFailed", command.verb).text(std::format("unknown script command '{}'", command.verb));
        return;
    }
    auto params = verb->parse(command.args);
    if (!params) {
        TRACE_ERROR("Engine", "ScriptCommandFailed", command.verb).text(std::format("{}: {}", command.verb, params.error()));
        return;
    }
    std::string name = verb->verb;
    RpcResponder responder([name](const RpcReply& reply) {
        if (!reply.ok) {
            TRACE_ERROR("Engine", "ScriptCommandFailed", name).text(std::format("{}: {}", name, reply.message));
        }
    });
    registry.invoke(verb->method, *params, responder);
}

auto registerViewRecords(RpcRecords& records, RpcRegistry& registry, ViewContext& view) -> void {
    records.add("status", "frame counter, scene, selection, camera, anti-aliasing, the material sampler and the purposes drawn", [&view](RpcResponder responder) {
        responder.respond(statusRecord(view));
    });
    records.add("scene", "the open scene: its layer stack, up axis and units, prims (instances expanded), prototypes, draws, lights and LOD levels set", [&view](RpcResponder responder) {
        responder.respond(sceneRecord(view));
    });
    records.add("map", "the engine as a map: lanes (processes, threads), nodes (what each part is, its records, windows and README, its live numbers) and edges (what flows between them)", [&view](RpcResponder responder) {
        responder.respond(mapRecord(view));
    });
    records.add("scenethread", "the scene thread: requests, composes and extracts, assets in flight and held, and residency (the script's hold, the load's longest frame)", [&view](RpcResponder responder) {
        responder.respond(sceneThreadRecord(view));
    });
    records.add("gpuscene", "the GPU scene's flow: change lists taken and drawn, the upload queue and its budget, the geometry pool and what waits for room", [&view](RpcResponder responder) {
        responder.respond(gpuSceneRecord(view));
    });
    records.add("payloads", "every payload placement reached while drawing: its prim, asset ids, whether loaded, and its extentsHint in world metres", [&view](RpcResponder responder) {
        responder.respond(payloadsRecord(view));
    });
    records.add("lod", "every LOD root drawn: its levels, the level it defaults to, wants and draws, the levels loaded, and whether set by hand", [&view](RpcResponder responder) {
        responder.respond(lodRecord(view));
    });
    records.add("assets", "what the view asked the asset server for and received: totals, and every asset with its state, size and times", [&view](RpcResponder responder) {
        responder.respond(assetsRecord(view));
    });
    records.add("culling", "GPU culling's latest readback: instances and triangles drawn and culled, per view", [&view](RpcResponder responder) {
        responder.respond(cullingRecord(view));
    });
    records.add("pipelines", "every pipeline the renderer registered: start-up or first use, state, shaders and their versions, the last error", [&view](RpcResponder responder) {
        responder.respond(pipelinesRecord(view));
    });
    records.add("profile", "the profiler's frame history: CPU and GPU time per frame", [](RpcResponder responder) {
        responder.respond(profileRecord());
    });
    records.add("render", "the render debug snapshot: device, swapchain, scene tables, per-pass stats, draw log (next frames)", [&view](RpcResponder responder) {
        view.dumps.requestRenderDebug(std::move(responder));
    });
    records.add("memory", "every allocation and memory heap (next frames)", [&view](RpcResponder responder) {
        view.dumps.requestMemory(std::move(responder));
    });
    records.add("counters", "one frame's GPU zones and pipeline statistics per pass (next frames)", [&view](RpcResponder responder) {
        view.dumps.requestCounters(std::move(responder));
    });

    // A record taken at a known frame, for scripted runs: the view samples it, so the frame is exact, and appends it to
    // a file, so nothing has to be connected at that frame to receive it.
    registry.add(
        {
            .name = "introspect.record",
            .summary = "Append a record to a file as one JSON line: {record, requested_frame, frame, ts_ns, value}.",
            .params = {
                requiredField("name", RpcType::String, "record name, from introspect.list"),
                requiredField("path", RpcType::String, "the file the line is appended to"),
            },
            .result = "{path, frame} once the line is written",
        },
        [&view, &records](const Json& p, RpcResponder responder) {
            auto name = p["name"].get<std::string>();
            if (!records.contains(name)) {
                responder.fail(rpc::invalidParams, std::format("no record '{}'", name));
                return;
            }
            auto path = p["path"].get<std::string>();
            auto requested = view.frameCounter;
            RpcResponder write([&view, name, path, requested, responder](const RpcReply& reply) mutable {
                if (!reply.ok) {
                    responder.fail(reply.code, reply.message);
                    return;
                }
                nlohmann::ordered_json line;
                line["record"] = name;
                line["requested_frame"] = requested;
                line["frame"] = view.frameCounter;
                line["ts_ns"] = trace::now();
                line["value"] = *reply.result;
                std::ofstream out(path, std::ios::app);
                out << line.dump() << '\n';
                if (!out) {
                    responder.fail(rpc::appError, std::format("cannot write {}", path));
                    return;
                }
                responder.respond({{"path", path}, {"frame", view.frameCounter}});
            });
            records.get(name, write);
        });
}
