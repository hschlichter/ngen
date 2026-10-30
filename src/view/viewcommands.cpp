#include "viewcommands.h"

#include "lodgroups.h"
#include "lodtool.h"
#include "streamingtool.h"

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

#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
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

auto parseOnOff(const std::string& args) -> std::expected<Json, std::string> {
    if (args != "on" && args != "off") {
        return std::unexpected(std::format("expected on or off, got '{}'", args));
    }
    return Json{{"on", args == "on"}};
}

// `fly <camera path prefix> <speed m/s>`.
auto parseFly(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string prefix;
    float speed = 0.0f;
    if (!(in >> prefix >> speed) || speed <= 0.0f) {
        return std::unexpected(std::format("expected '<camera path prefix> <speed m/s>', got '{}'", args));
    }
    return Json{{"prefix", prefix}, {"speed", speed}};
}

auto parseLodPreference(const std::string& args) -> std::expected<Json, std::string> {
    if (!lodPreferenceFromName(args)) {
        return std::unexpected(std::format("expected distance, screen or authored, got '{}'", args));
    }
    return Json{{"prefer", args}};
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

// "on" or "off" as {enabled}: the prepass and anti-aliasing verbs.
auto parsePrepass(const std::string& args) -> std::expected<Json, std::string> {
    if (args != "on" && args != "off") {
        return std::unexpected(std::format("unknown argument '{}'", args));
    }
    return Json{{"enabled", args == "on"}};
}

// One LOD root's inspection as the view.lod.inspect method and the lodtool record answer it; distances in the stage's units.
auto lodInspectionJson(const LodInspection& in, double metersPerUnit) -> Json;
// One streaming root's inspection as view.streaming.inspect and the streamingtool record answer it; distances in metres.
auto streamingInspectionJson(const StreamingInspection& in) -> Json;

// "<root> [h=<heuristic>] <t> <t> …": one heuristic's thresholds (or blend thresholds) for a LOD root's authored root, the heuristic in use
// when none is named; no values clears the list.
auto parseLodThresholds(const std::string& args, const char* field) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string root;
    if (!(in >> root)) {
        return std::unexpected("expected '<root> [h=<heuristic>] <threshold> …'");
    }
    Json params = {{"root", root}};
    auto values = Json::array();
    std::string item;
    while (in >> item) {
        if (item.starts_with("h=")) {
            params["heuristic"] = std::atoi(item.c_str() + 2);
            continue;
        }
        char* end = nullptr;
        float value = std::strtof(item.c_str(), &end);
        if (end == item.c_str() || *end != '\0') {
            return std::unexpected(std::format("'{}' isn't a number", item));
        }
        values.push_back(value);
    }
    params[field] = values;
    return params;
}

auto parseLodThresholdList(const std::string& args) -> std::expected<Json, std::string> {
    return parseLodThresholds(args, "thresholds");
}

auto parseLodBlendList(const std::string& args) -> std::expected<Json, std::string> {
    return parseLodThresholds(args, "blendThresholds");
}

auto parseLodUnloadList(const std::string& args) -> std::expected<Json, std::string> {
    return parseLodThresholds(args, "unloadThresholds");
}

// "[<root>]": every override when empty.
auto parseLodOverrideReset(const std::string& args) -> std::expected<Json, std::string> {
    if (args.empty()) {
        return Json::object();
    }
    return Json{{"root", args}};
}

auto parseLodNumber(const std::string& args, const char* field) -> std::expected<Json, std::string> {
    char* end = nullptr;
    float value = std::strtof(args.c_str(), &end);
    if (args.empty() || *end != '\0') {
        return std::unexpected(std::format("expected a number, got '{}'", args));
    }
    return Json{{field, value}};
}

auto parseLodBias(const std::string& args) -> std::expected<Json, std::string> {
    return parseLodNumber(args, "bias");
}

auto parseLodHysteresis(const std::string& args) -> std::expected<Json, std::string> {
    return parseLodNumber(args, "hysteresis");
}

auto parseLodBlending(const std::string& args) -> std::expected<Json, std::string> {
    if (args != "on" && args != "off") {
        return std::unexpected(std::format("expected 'on' or 'off', got '{}'", args));
    }
    return Json{{"blending", args == "on"}};
}

// "labels|gizmo|gizmo-all|flash on|off".
auto parseLodShow(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string layer;
    std::string state;
    if (!(in >> layer >> state) || (state != "on" && state != "off")) {
        return std::unexpected(std::format("expected 'labels|gizmo|gizmo-all|flash on|off', got '{}'", args));
    }
    if (layer == "gizmo-all") {
        layer = "gizmoAll";
    }
    if (layer != "labels" && layer != "gizmo" && layer != "gizmoAll" && layer != "flash") {
        return std::unexpected(std::format("unknown layer '{}': labels, gizmo, gizmo-all or flash", layer));
    }
    return Json{{layer, state == "on"}};
}

// `streaming-show bounds|rings|rings-all|labels on|off`.
auto parseStreamingShow(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string layer;
    std::string state;
    if (!(in >> layer >> state) || (state != "on" && state != "off")) {
        return std::unexpected(std::format("expected 'bounds|rings|rings-all|labels on|off', got '{}'", args));
    }
    if (layer == "rings-all") {
        layer = "ringsAll";
    }
    if (layer != "bounds" && layer != "rings" && layer != "ringsAll" && layer != "labels") {
        return std::unexpected(std::format("unknown layer '{}': bounds, rings, rings-all or labels", layer));
    }
    return Json{{layer, state == "on"}};
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
    ScriptVerb{"lod-heuristic", "view.lod.heuristic", parseOnOff},
    ScriptVerb{"lod-prefer", "view.lod.heuristic", parseLodPreference},
    ScriptVerb{"lod-thresholds", "view.lod.override", parseLodThresholdList},
    ScriptVerb{"lod-blend-thresholds", "view.lod.override", parseLodBlendList},
    ScriptVerb{"lod-unload-thresholds", "view.lod.override", parseLodUnloadList},
    ScriptVerb{"lod-override-reset", "view.lod.override.reset", parseLodOverrideReset},
    ScriptVerb{"lod-bias", "view.lod.tuning", parseLodBias},
    ScriptVerb{"lod-hysteresis", "view.lod.tuning", parseLodHysteresis},
    ScriptVerb{"lod-blending", "view.lod.tuning", parseLodBlending},
    ScriptVerb{"lod-show", "view.lod.visualize", parseLodShow},
    ScriptVerb{"wait-resident", "view.waitResident", parseNothing},
    ScriptVerb{"fly", "view.camera.fly", parseFly},
    ScriptVerb{"fly-stop", "view.camera.fly.stop", parseNothing},
    ScriptVerb{"streaming-reset", "view.streaming.reset", parseNothing},
    ScriptVerb{"streaming-show", "view.streaming.visualize", parseStreamingShow},
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
    ScriptVerb{"aa", "view.antialiasing", parsePrepass},
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
            .name = "view.camera.fly",
            .summary = "Fly the camera through the cameras the scene authors whose paths start with a prefix, in path order (/cameras/flight_00, …), at a speed in metres a second, eye and direction interpolated. A test driver for streaming: a script holds its clock while it flies. Answers when the last camera is reached (or the flight is stopped), with the cameras, metres and seconds flown.",
            .params = {requiredField("prefix", RpcType::String, "a camera path prefix"), requiredField("speed", RpcType::Float, "metres a second")},
            .result = "{cameras, metres, seconds, stopped}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto speed = p["speed"].get<float>();
            if (speed <= 0.0f) {
                responder.fail(rpc::invalidParams, "speed is metres a second, above 0");
                return;
            }
            if (auto error = startFlight(view, p["prefix"].get<std::string>(), speed, responder)) {
                responder.fail(rpc::appError, *error);
            }
        });
    registry.add({.name = "view.streaming.reset", .summary = "Zeroes the streaming counters (late switches, root-seconds coarser, loads, reloads, leads, arrivals unwanted), for a run measured from a known start.", .result = "{}"},
                 [&view](const Json&, RpcResponder responder) {
                     view.scene.resetStreamingCounters();
                     responder.respond(Json::object());
                 });
    registry.add(
        {
            .name = "view.streaming.inspect",
            .summary = "One streaming root inspected, as the Streaming window shows it: its residency (selected, state and why, resident and why, drawn, its state's name), every heuristic it names by domain with its thresholds authored and in force and its measure from the camera, its levels (requested, loaded, ready, drawn, lead), its authoring findings, its parent and children.",
            .params = {requiredField("root", RpcType::String, "a streaming LOD root's full path")},
            .result = "the inspection",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto root = p["root"].get<std::string>();
            auto inspection = inspectStreamingRoot(view.scene, root, view.cam.position, lodWindowArea(glm::radians(cameraFovDegrees), view.viewAspect));
            if (!inspection) {
                responder.fail(rpc::appError, std::format("no streaming root is placed at '{}'", root));
                return;
            }
            responder.respond(streamingInspectionJson(*inspection));
        });
    registry.add(
        {
            .name = "view.streaming.visualize",
            .summary = "The Streaming tool's viewport layers: each root's streaming bounds in its state's colour (bounds), the load and unload radii of the selected root (rings) or of every root the Streaming window's list shows (ringsAll), labels. The streaming view is `view.debugview streaming`.",
            .params = {optionalField("bounds", RpcType::Bool, "bounds on"),
                       optionalField("rings", RpcType::Bool, "the selected root's rings on"),
                       optionalField("ringsAll", RpcType::Bool, "every shown root's rings on"),
                       optionalField("labels", RpcType::Bool, "labels on")},
            .result = "{bounds, rings, ringsAll, labels}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto& state = view.ui.streamingWindow();
            for (auto [name, flag] : {std::pair{"bounds", &state.bounds}, std::pair{"rings", &state.rings}, std::pair{"ringsAll", &state.ringsAll}, std::pair{"labels", &state.labels}}) {
                if (p.contains(name)) {
                    *flag = p[name].get<bool>();
                }
            }
            responder.respond({{"bounds", state.bounds}, {"rings", state.rings}, {"ringsAll", state.ringsAll}, {"labels", state.labels}});
        });
    registry.add({.name = "view.camera.fly.stop", .summary = "Stops the camera's flight where it is; its call answers.", .result = "{}"}, [&view](const Json&, RpcResponder responder) {
        stopFlight(view);
        responder.respond(Json::object());
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
            .name = "view.renderlod",
            .summary = "The render-LOD groups under a path (each render-LOD root where it is placed): sphere, thresholds, default and forced level, the screen size, and the level the GPU wants and draws; with the `renderlod` record's totals and check. Answers frames later, once the states are read back.",
            .params = {requiredField("path", RpcType::String, "prim path; / for every group")},
            .result = "the renderlod record, with groupsUnder",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.renderLodRequests.push_back({.responder = std::move(responder), .askedAt = view.latestCull.lodFrame, .path = p["path"].get<std::string>()});
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
                return Json{{"position", vec3Json(t.position)}, {"rotation", {t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w}}, {"scale", vec3Json(t.scale)}};
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
            .name = "view.lod.heuristic",
            .summary = "The LOD heuristic on (every root not set by hand wants the level its heuristic picks from the camera, and keeps resident what its streaming radii ask for) or off (its default level and every coarser one), and which heuristic a root naming several measures by.",
            .params = {optionalField("on", RpcType::Bool, "on or off"), optionalField("prefer", RpcType::String, "distance (the default), screen, or authored (the first the root names)")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            if (p.contains("on")) {
                view.scene.setLodHeuristic(p["on"].get<bool>());
            }
            if (p.contains("prefer")) {
                auto prefer = lodPreferenceFromName(p["prefer"].get<std::string>());
                if (!prefer) {
                    responder.fail(rpc::invalidParams, std::format("prefer is distance, screen or authored, not '{}'", p["prefer"].get<std::string>()));
                    return;
                }
                view.scene.setLodPreference(*prefer);
            }
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.lod.override",
            .summary = "One heuristic's thresholds and blend thresholds in place of the authored ones, for the authored root of the LOD root at a path: every placement of it (every instance of an instanceable asset). A view-local opinion, never saved. Thresholds in the heuristic's unit (a distance in the stage's units, ascending; a screen size as a fraction of the view, descending). The LodTuningChanged trace event says what changed; LodTuningRefused why nothing did.",
            .params = {requiredField("root", RpcType::String, "a LOD root's full path (a streaming root or a render-LOD group)"),
                       optionalField("heuristic", RpcType::Int, "which of its heuristics (an index in authored order); the one the preference uses when absent"),
                       optionalField("thresholds", RpcType::Array, "finest first"),
                       optionalField("blendThresholds", RpcType::Array, "where each band ends, finest first; [] for none"),
                       optionalField("unloadThresholds", RpcType::Array, "a streaming heuristic's unload radii, one per load threshold, at or past it; the streaming heuristic when no heuristic is named; [] for the load radii")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            LodOverride thresholds;
            if (p.contains("thresholds")) {
                thresholds.thresholds = p["thresholds"].get<std::vector<float>>();
            }
            if (p.contains("blendThresholds")) {
                thresholds.blendThresholds = p["blendThresholds"].get<std::vector<float>>();
            }
            if (p.contains("unloadThresholds")) {
                thresholds.unloadThresholds = p["unloadThresholds"].get<std::vector<float>>();
            }
            std::optional<uint32_t> heuristic;
            if (p.contains("heuristic")) {
                heuristic = (uint32_t) std::max(0, p["heuristic"].get<int>());
            }
            view.scene.overrideLodThresholds(p["root"].get<std::string>(), heuristic, std::move(thresholds));
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.lod.override.reset",
            .summary = "Drops the threshold overrides of the authored root of the LOD root at a path, or every override when no root is given.",
            .params = {optionalField("root", RpcType::String, "a LOD root's full path")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.scene.resetLodOverrides(p.contains("root") ? p["root"].get<std::string>() : std::string());
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.lod.tuning",
            .summary = "The view's LOD tuning, local and never saved: a bias (every root picks as if it were this many times closer), the hysteresis at a boundary without a blend band (a fraction of a screen size; a distance the same margin in size), and blending (off drops every band).",
            .params = {optionalField("bias", RpcType::Float, "1 as authored; 2 finer everywhere"),
                       optionalField("hysteresis", RpcType::Float, "0 to 0.9; 0.2 by default"),
                       optionalField("blending", RpcType::Bool, "blend bands on")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            std::optional<float> bias;
            std::optional<float> hysteresis;
            std::optional<bool> blending;
            if (p.contains("bias")) {
                bias = p["bias"].get<float>();
            }
            if (p.contains("hysteresis")) {
                hysteresis = p["hysteresis"].get<float>();
            }
            if (p.contains("blending")) {
                blending = p["blending"].get<bool>();
            }
            view.scene.setLodTuning(bias, hysteresis, blending);
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.lod.visualize",
            .summary = "The LOD tool's viewport layers: labels over the LOD roots, the heuristic gizmo of the selected root (gizmo) or of every root the LOD window's list shows (gizmoAll), and the pop flash (a render-LOD root's meshes flash white when its level changes without a fade). The LOD Level view is `view.mode lodlevel`.",
            .params = {optionalField("labels", RpcType::Bool, "labels on"),
                       optionalField("gizmo", RpcType::Bool, "the selected root's gizmo on"),
                       optionalField("gizmoAll", RpcType::Bool, "every shown root's gizmo on"),
                       optionalField("flash", RpcType::Bool, "the pop flash on")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto& window = view.ui.lodWindow();
            if (p.contains("labels")) {
                window.labels = p["labels"].get<bool>();
            }
            if (p.contains("gizmo")) {
                window.gizmo = p["gizmo"].get<bool>();
            }
            if (p.contains("gizmoAll")) {
                window.gizmoAll = p["gizmoAll"].get<bool>();
            }
            if (p.contains("flash")) {
                window.flash = p["flash"].get<bool>();
            }
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.lod.inspect",
            .summary = "One LOD root as the LOD window's inspector shows it: kind, authored root, state (wanted, drawn, fade, why), every heuristic (used or why not, thresholds authored and in force, its measure from the camera now and the level it would pick) and every level (triangles, draws).",
            .params = {requiredField("root", RpcType::String, "a LOD root's full path")},
            .result = "the inspection",
        },
        [&view](const Json& p, RpcResponder responder) {
            // Answered once a readback carries every group's state, as the renderlod record is.
            view.renderLodRequests.push_back({.responder = std::move(responder), .askedAt = view.latestCull.lodFrame, .path = p["root"].get<std::string>(), .kind = RenderLodRequest::Kind::Inspect});
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
            .name = "view.antialiasing",
            .summary = "The anti-aliasing pass (FXAA) on the presented image; off shows the image as the passes wrote it (a dither pattern unblurred).",
            .params = {requiredField("enabled", RpcType::Bool, "anti-aliasing on")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.ui.setAntiAliasing(p["enabled"].get<bool>());
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
        {"instancers", view.scene.instancers().size()},
        {"instancerPoints", std::ranges::fold_left(view.scene.instancers(), (uint64_t) 0, [](uint64_t sum, const SceneInstancer& instancer) { return sum + instancer.points; })},
        {"primsWithVariants", view.scene.variantPrims().size()},
        {"lights", view.scene.renderWorld().lights.size()},
        {"lodLevels", lodLevels},
    };
}

// A LOD root's heuristics as the records list them: kind, type, domain, thresholds and bands, bounds, and why one isn't read.
auto heuristicsJson(const std::vector<LodHeuristic>& heuristics, LodPreference preference) -> Json {
    auto chosen = chooseLodHeuristic(heuristics, LodDomain::Imaging, preference);
    auto chosenStreaming = chooseLodHeuristic(heuristics, LodDomain::Streaming, preference);
    auto out = Json::array();
    for (size_t i = 0; i < heuristics.size(); i++) {
        const auto& heuristic = heuristics[i];
        Json entry = {
            {"type", heuristic.type},
            {"path", heuristic.path},
            {"domain", heuristic.domain},
            {"thresholds", heuristic.thresholds},
            {"blendThresholds", heuristic.blendThresholds},
            {"used", (int32_t) i == chosen || (int32_t) i == chosenStreaming},
        };
        if (heuristic.domainKind == LodDomain::Streaming) {
            entry["unloadThresholds"] = heuristic.unloadThresholds;
        }
        if (heuristic.kind == LodHeuristicKind::Distance) {
            entry["kind"] = "distance";
            entry["center"] = vec3Json(heuristic.center);
            if (heuristic.box) {
                entry["box"] = {vec3Json((*heuristic.box)[0]), vec3Json((*heuristic.box)[1])};
            }
        } else if (heuristic.kind == LodHeuristicKind::ScreenSize) {
            entry["kind"] = "screen";
            entry["extent"] = {vec3Json(heuristic.extent[0]), vec3Json(heuristic.extent[1])};
            entry["projection"] = heuristic.projection == LodProjection::Extent ? "projectedExtent" : "projectedSphere";
        } else {
            entry["kind"] = "other";
        }
        if (!heuristic.usable()) {
            entry["unusable"] = heuristic.unusable;
        }
        out.push_back(entry);
    }
    return out;
}

// The view's LOD tuning: bias, hysteresis, blending, and every override by authored root and heuristic.
auto lodTuningJson(const LodTuning& tuning) -> Json {
    auto overrides = Json::array();
    for (const auto& [key, heuristics] : tuning.overrides) {
        for (const auto& [index, override] : heuristics) {
            overrides.push_back({
                {"root", key},
                {"heuristic", index},
                {"thresholds", override.thresholds ? Json(*override.thresholds) : Json(nullptr)},
                {"blendThresholds", override.blendThresholds ? Json(*override.blendThresholds) : Json(nullptr)},
                {"unloadThresholds", override.unloadThresholds ? Json(*override.unloadThresholds) : Json(nullptr)},
            });
        }
    }
    return {{"bias", tuning.bias}, {"hysteresis", tuning.hysteresis}, {"blending", tuning.blending}, {"overrides", overrides}};
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
auto lodToolRecord(const ViewContext& view) -> Json {
    const auto& window = view.ui.lodWindow();
    auto inspection = Json(nullptr);
    std::string root;
    if (!view.selectedPath.empty()) {
        // The LOD root at or above the selection, as the window takes it.
        for (const auto& candidate : view.scene.lodRoots()) {
            if ((view.selectedPath == candidate.path || view.selectedPath.starts_with(candidate.path + "/")) && candidate.path.size() > root.size()) {
                root = candidate.path;
            }
        }
        const auto& draws = *view.scene.snapshot().draws;
        for (const auto& candidate : draws.lodGroupPaths) {
            if (!candidate.empty() && (view.selectedPath == candidate || view.selectedPath.starts_with(candidate + "/")) && candidate.size() > root.size()) {
                root = candidate;
            }
        }
    }
    if (!root.empty()) {
        if (auto found = inspectLodRoot(view.scene, view.latestCull, root, {.eye = view.cam.position, .fovY = glm::radians(cameraFovDegrees), .aspect = view.viewAspect})) {
            inspection = lodInspectionJson(*found, view.scene.lodMetersPerUnit());
        }
    }
    return {
        {"selectedRoot", root.empty() ? Json(nullptr) : Json(root)},
        {"inspection", inspection},
        {"tuning", lodTuningJson(view.scene.lodTuning())},
        {"layers", {{"levelView", view.ui.getGBufferViewMode() == 10}, {"labels", window.labels}, {"gizmo", window.gizmo}, {"gizmoAll", window.gizmoAll}, {"flash", window.flash}}},
        {"pops", view.lodPops},
        {"windowRows", window.rows.size()},
    };
}

auto streamingRecord(const ViewContext& view) -> Json {
    const auto& choices = view.scene.lodChoices();
    const auto& t = view.scene.threadStats();
    const auto& streaming = view.scene.streaming();
    const auto& tuning = view.scene.lodTuning();
    auto roots = Json::array();
    auto aspect = view.scene.cameraAspect();
    uint32_t loading = 0;
    uint32_t inFrustum = 0;
    PlacementWait waitingTotal;
    for (const auto& root : view.scene.lodRoots()) {
        auto choice = choices.find(root.path);
        bool evaluated = choice != choices.end();
        bool visible = lodRootInFrustum(root, view.cam.position, view.cam.forward(), view.cam.right(), glm::radians(cameraFovDegrees), aspect);
        if (visible) {
            inFrustum++;
        }
        auto waiting = Json(nullptr);
        if (root.drawn && *root.drawn != root.wanted) {
            loading++;
        }
        if (root.waiting) {
            waiting = {{"meshes", root.waiting->meshes}, {"materials", root.waiting->materials}, {"textures", root.waiting->textures}, {"composing", root.waiting->composing}};
            waitingTotal += *root.waiting;
        }
        const SceneStreamingRoot* measured = nullptr;
        if (auto it = streaming.roots.find(root.path); it != streaming.roots.end()) {
            measured = &it->second;
        }
        // Per level: its name, whether it is asked to be resident, loaded (composed), ready (placed and waiting for nothing), drawn, what its
        // placement waits for (null while it is composing or not loaded), and its last lead.
        auto levels = Json::array();
        for (uint32_t level = 0; level < root.levels.size(); level++) {
            auto wait = Json(nullptr);
            if (level < root.levelWaits.size() && root.levelWaits[level]) {
                const auto& w = *root.levelWaits[level];
                wait = {{"meshes", w.meshes}, {"materials", w.materials}, {"textures", w.textures}, {"composing", w.composing}};
            }
            bool requested = evaluated && std::ranges::contains(choice->second.resident, level);
            bool ready = measured != nullptr && level < measured->ready.size() && measured->ready[level];
            double lead = measured != nullptr && level < measured->lead.size() ? measured->lead[level] : -1.0;
            levels.push_back({
                {"level", level},
                {"name", root.levels[level]},
                {"requested", requested},
                {"loaded", std::ranges::contains(root.loaded, level)},
                {"ready", ready},
                {"drawn", root.drawn && *root.drawn == level},
                {"waiting", wait},
                {"leadSeconds", lead >= 0.0 ? Json(lead) : Json(nullptr)},
            });
        }
        auto used = chooseLodHeuristic(root.heuristics, LodDomain::Imaging, view.scene.lodPreference());
        bool measuredImaging = evaluated && choice->second.measure >= 0.0f;
        // The thresholds the pick uses: the override's, the bias applied, no bands with blending off.
        LodHeuristic tuned;
        if (used >= 0) {
            tuned = tunedLodHeuristic(root.heuristics[(size_t) used], lodOverrideOf(tuning, root.key, (uint32_t) used), tuning);
        }
        auto streamingUsed = chooseLodHeuristic(root.heuristics, LodDomain::Streaming, view.scene.lodPreference());
        auto streamingJson = Json(nullptr);
        if (streamingUsed >= 0) {
            auto radii = tunedLodHeuristic(root.heuristics[(size_t) streamingUsed], lodOverrideOf(tuning, root.key, (uint32_t) streamingUsed), tuning);
            bool measuredStreaming = evaluated && choice->second.streamingMeasure >= 0.0f;
            streamingJson = {
                {"heuristic", streamingUsed},
                {"kind", radii.kind == LodHeuristicKind::Distance ? "distance" : "screen"},
                {"measure", measuredStreaming ? Json(choice->second.streamingMeasure) : Json(nullptr)},
                {"load", radii.thresholds},
                {"unload", radii.unloadThresholds},
            };
        }
        auto findings = Json::array();
        if (evaluated) {
            for (const auto& finding : choice->second.findings) {
                findings.push_back({{"rule", finding.rule}, {"heuristic", finding.heuristic}, {"boundary", finding.boundary}, {"detail", finding.detail}});
            }
        }
        roots.push_back({
            {"path", root.path},
            {"heuristic", used},
            {"kind", measuredImaging ? Json(choice->second.kind == LodHeuristicKind::Distance ? "distance" : "screen") : Json(nullptr)},
            {"measure", measuredImaging ? Json(choice->second.measure) : Json(nullptr)},
            {"inFrustum", visible},
            {"selected", evaluated ? Json(choice->second.level) : Json(nullptr)},
            {"why", evaluated ? Json(choice->second.reason) : Json("not evaluated yet")},
            {"thresholds", tuned.thresholds},
            {"blendThresholds", tuned.blendThresholds},
            {"streaming", streamingJson},
            {"state", evaluated ? Json(choice->second.state) : Json(nullptr)},
            {"resident", evaluated ? Json(choice->second.resident) : Json(nullptr)},
            {"residentWhy", evaluated ? Json(choice->second.residentReason) : Json(nullptr)},
            {"key", root.key},
            {"tuned", tuning.overrides.contains(root.key)},
            {"drawn", root.drawn ? Json(*root.drawn) : Json(nullptr)},
            {"late", measured != nullptr && measured->late},
            {"hole", measured != nullptr && measured->hole},
            {"reloads", measured != nullptr ? measured->reloads : 0},
            {"loaded", root.loaded},
            {"waiting", waiting},
            {"levels", levels},
            {"findings", findings},
        });
    }
    auto leads = streaming.leads;
    std::ranges::sort(leads);
    auto lead = Json(nullptr);
    if (!leads.empty()) {
        lead = {{"count", leads.size()}, {"minSeconds", leads.front()}, {"medianSeconds", leads[leads.size() / 2]}};
    }
    return {
        {"totals",
         {{"roots", roots.size()},
          {"placements", t.placements},
          {"contentsHeld", t.prototypesHeld + t.levelsHeld},
          {"assetsHeld", t.meshPacks + t.materialPacks + t.texturePacks},
          {"inFrustum", inFrustum},
          {"loading", loading},
          {"waiting", {{"meshes", waitingTotal.meshes}, {"materials", waitingTotal.materials}, {"textures", waitingTotal.textures}, {"composing", waitingTotal.composing}}},
          {"renderLodGroups", view.scene.threadStats().lodGroups}}},
        {"measured",
         {{"lateNow", streaming.lateNow},
          {"holesNow", streaming.holesNow},
          {"lateSwitches", streaming.lateSwitches},
          {"holes", streaming.holes},
          {"rootSecondsCoarser", streaming.lateSeconds},
          {"loads", streaming.loads},
          {"reloads", streaming.reloads},
          {"released", streaming.released},
          {"lead", lead},
          {"arrivedUnwanted", streaming.arrivedUnwanted},
          {"authoringFindings", streaming.findings}}},
        {"heuristic", view.scene.lodHeuristic()},
        {"preference", lodPreferenceName(view.scene.lodPreference())},
        {"tuning", lodTuningJson(tuning)},
        {"unloaded", {{"levels", t.releasedLevels}, {"contents", t.releasedContents}, {"meshPacks", t.releasedMeshes}, {"meshBytes", t.releasedMeshBytes}}},
        {"evaluation", {{"count", t.lodEvaluations}, {"roots", t.lodRootsEvaluated}, {"finer", t.lodFiner}, {"coarser", t.lodCoarser}, {"ms", t.lodEvaluateMs}, {"relinks", t.relinks}}},
        {"roots", roots},
    };
}

auto lodRecord(const ViewContext& view) -> Json {
    auto roots = Json::array();
    for (const auto& root : view.scene.lodRoots()) {
        roots.push_back({
            {"path", root.path},
            {"levels", root.levels},
            {"levelPayloads", root.levelPayloads},
            {"heuristics", heuristicsJson(root.heuristics, view.scene.lodPreference())},
            {"default", root.defaultIndex},
            {"wanted", root.wanted},
            {"drawn", root.drawn ? Json(*root.drawn) : Json(nullptr)},
            {"loaded", root.loaded},
            {"setByHand", root.setByHand},
            {"key", root.key},
            {"tuned", view.scene.lodTuning().overrides.contains(root.key)},
        });
    }
    return {{"preference", lodPreferenceName(view.scene.lodPreference())}, {"tuning", lodTuningJson(view.scene.lodTuning())}, {"roots", roots}};
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
        {"camera", {{"drawn", cull.instances - cull.cameraCulled - cull.lodDropped}, {"culled", cull.cameraCulled}, {"triangles", cull.cameraTriangles}}},
        {"lodLevelNotDrawn", cull.lodDropped},
        {"regionOverflow", cull.regionOverflow},
        {"sceneTriangles", cull.sceneTriangles},
        {"cascades", cascades},
    };
}

// The levels the view's heuristic may want for a group at a camera, as pickLodLevel picks them (src/world/world.cpp): from the level it
// keeps going coarser (wanting the finest before) to the level it goes finer to (wanting the coarsest before); which it keeps depends on the
// level it wanted before. Inside a band the two agree and `blend` is the next level's share. The measure is the GPU's: a distance in
// metres, or a screen size. onEdge when the measure is within 1e-4 of a threshold, a band's edge or a hysteresis edge, where float order
// may differ between the CPU and the GPU.
struct ExpectedLevels {
    float measure = 0.0f;
    uint32_t low = 0;
    uint32_t high = 0;
    float blend = 0.0f;
    bool onEdge = false;
};

auto expectedLevels(const RenderLodGroup& group, const glm::vec3& eye, float windowArea, float hysteresis) -> ExpectedLevels {
    ExpectedLevels out;
    bool distance = group.kind == RenderLodKind::Distance;
    if (distance) {
        auto target = group.point;
        auto eyeRoot = glm::vec3(group.toRoot * glm::vec4(eye, 1.0f));
        bool inside = group.hasBox && glm::all(glm::greaterThanEqual(eyeRoot, group.boxMin)) && glm::all(glm::lessThanEqual(eyeRoot, group.boxMax));
        if (group.hasBox) {
            target = glm::clamp(eyeRoot, group.boxMin, group.boxMax);
        }
        out.measure = inside ? 0.0f : glm::length(glm::vec3(group.toWorld * glm::vec4(target, 1.0f)) - eye);
    } else {
        auto toCenter = glm::length(eye - group.center);
        out.measure = 1e30f;
        if (toCenter > 1e-6f) {
            float r = group.radius / toCenter;
            out.measure = glm::pi<float>() * r * r / windowArea;
        }
    }
    std::vector<float> thresholds(group.thresholds.begin(), group.thresholds.begin() + group.thresholdCount);
    std::vector<float> blends(group.blendThresholds.begin(), group.blendThresholds.begin() + group.blendCount);
    auto kind = distance ? LodHeuristicKind::Distance : LodHeuristicKind::ScreenSize;
    auto last = group.levelCount > 0 ? group.levelCount - 1 : 0;
    auto fromFinest = pickLodLevel(kind, thresholds, blends, out.measure, group.levelCount, 0, hysteresis);
    auto fromCoarsest = pickLodLevel(kind, thresholds, blends, out.measure, group.levelCount, last, hysteresis);
    out.low = fromFinest.level;
    out.high = fromCoarsest.level;
    out.blend = fromCoarsest.blend;
    float coarserScale = distance ? 1.0f / std::sqrt(1.0f - hysteresis) : 1.0f - hysteresis;
    auto near = [&](float edge) {
        return std::abs(out.measure - edge) <= 1e-4f * std::abs(edge);
    };
    for (size_t t = 0; t < thresholds.size(); t++) {
        if (near(thresholds[t]) || near(thresholds[t] * coarserScale) || (t < blends.size() && near(blends[t]))) {
            out.onEdge = true;
        }
    }
    return out;
}

// The render-LOD groups (src/renderer/README.md, "Render LOD"): what is placed, what the GPU picked at the latest readback with states,
// and that pick checked against the view's heuristic at the camera the pick measured from; with `path`, every group under it. The view's
// own heuristic measures the same window (2 tan(fovY / 2) high, times the aspect wide).
auto renderLodRecord(const ViewContext& view, const std::optional<std::string>& path) -> Json {
    const auto& draws = *view.scene.snapshot().draws;
    const auto& groups = draws.drawn.lodGroups;
    const auto& cull = view.latestCull;
    std::array<uint64_t, RenderLodGroup::maxLevels> placedSlots = {};
    for (const auto& instance : draws.drawn.meshInstances) {
        if (instance.alive && instance.lodGroup != 0 && instance.lodLevel >= 1 && instance.lodLevel <= RenderLodGroup::maxLevels) {
            placedSlots[instance.lodLevel - 1]++;
        }
    }
    uint32_t alive = 0;
    uint32_t forced = 0;
    auto eye = cull.lodEye;
    auto windowArea = cull.lodWindowArea;
    auto fovY = glm::radians(cameraFovDegrees);
    auto viewWindowHeight = 2.0f * std::tan(fovY * 0.5f);
    auto viewWindowArea = viewWindowHeight * viewWindowHeight * view.viewAspect;
    uint32_t compared = 0;
    uint32_t differ = 0;
    uint32_t onEdge = 0;
    uint32_t fallbacks = 0;
    uint32_t flashing = 0;
    auto examples = Json::array();
    auto listed = Json::array();
    for (uint32_t g = 0; g < groups.size(); g++) {
        const auto& group = groups[g];
        if (!group.alive) {
            continue;
        }
        alive++;
        if (group.forcedLevel >= 0) {
            forced++;
        }
        auto expected = expectedLevels(group, eye, windowArea, view.scene.lodTuning().hysteresis);
        if (group.forcedLevel >= 0) {
            expected.low = std::min<uint32_t>((uint32_t) group.forcedLevel, group.levelCount - 1);
            expected.high = expected.low;
            expected.blend = 0.0f;
            expected.onEdge = false;
        }
        bool read = cull.lodHasStates && g < cull.lodWanted.size();
        uint32_t wanted = read ? cull.lodWanted[g] : 0xFFu;
        uint32_t drawn = read ? cull.lodDrawn[g] : 0xFFu;
        uint32_t fadeLevel = read && g < cull.lodFadeLevel.size() ? cull.lodFadeLevel[g] : 0xFFu;
        float fade = read && g < cull.lodFade.size() ? cull.lodFade[g] : 0.0f;
        float gpuMeasure = read && g < cull.lodMeasure.size() ? cull.lodMeasure[g] : 0.0f;
        uint8_t flags = read && g < cull.lodFlags.size() ? cull.lodFlags[g] : 0;
        if ((flags & LodStateFlags::flashing) != 0) {
            flashing++;
        }
        // The stage's units for a distance, as its thresholds are authored.
        float unit = group.kind == RenderLodKind::Distance ? (float) view.scene.lodMetersPerUnit() : 1.0f;
        const auto& groupPath = g < draws.lodGroupPaths.size() ? draws.lodGroupPaths[g] : std::string();
        if (read) {
            compared++;
            if (drawn != wanted) {
                fallbacks++;
            }
            // A fade the GPU shows only when the next level is resident and the wanted one is drawn: compared where it does.
            bool fadeDiffers = fadeLevel != 0xFFu && std::abs(fade - expected.blend) > 1e-3f;
            if (expected.onEdge) {
                onEdge++;
            } else if (wanted < expected.low || wanted > expected.high || fadeDiffers) {
                differ++;
                if (examples.size() < 10) {
                    examples.push_back({{"path", groupPath},
                                        {"measure", expected.measure / unit},
                                        {"wanted", wanted},
                                        {"expected", {expected.low, expected.high}},
                                        {"fade", fade},
                                        {"expectedFade", expected.blend},
                                        {"forced", group.forcedLevel}});
                }
            }
        }
        bool under = path && !groupPath.empty() && (*path == "/" || groupPath == *path || (groupPath.size() > path->size() && groupPath.starts_with(*path) && groupPath[path->size()] == '/'));
        if (under) {
            auto thresholds = Json::array();
            for (uint32_t t = 0; t < group.thresholdCount; t++) {
                thresholds.push_back(group.thresholds[t] / unit);
            }
            auto blends = Json::array();
            for (uint32_t t = 0; t < group.blendCount; t++) {
                blends.push_back(group.blendThresholds[t] / unit);
            }
            Json entry = {
                {"group", g},
                {"path", groupPath},
                {"kind", group.kind == RenderLodKind::Distance ? "distance" : "screen"},
                {"levels", group.levelCount},
                {"thresholds", thresholds},
                {"blendThresholds", blends},
                {"default", group.defaultLevel},
                {"forced", group.forcedLevel},
                {"measure", expected.measure / unit},
                {"gpuMeasure", read ? Json(gpuMeasure / unit) : Json(nullptr)},
                {"wanted", read ? Json(wanted) : Json(nullptr)},
                {"drawn", read ? Json(drawn) : Json(nullptr)},
                {"fadeLevel", read && fadeLevel != 0xFFu ? Json(fadeLevel) : Json(nullptr)},
                {"fade", read ? Json(fade) : Json(nullptr)},
                {"expectedFade", expected.blend},
                {"forcedFlag", read ? Json((flags & LodStateFlags::forced) != 0) : Json(nullptr)},
                {"flashing", read ? Json((flags & LodStateFlags::flashing) != 0) : Json(nullptr)},
                {"key", g < draws.lodGroupSources.size() && draws.lodGroupSources[g] != nullptr ? Json(draws.lodGroupSources[g]->key) : Json(nullptr)},
            };
            if (group.kind == RenderLodKind::Distance) {
                entry["center"] = vec3Json(group.point);
                if (group.hasBox) {
                    entry["box"] = {vec3Json(group.boxMin), vec3Json(group.boxMax)};
                }
            } else {
                entry["sphere"] = {{"center", vec3Json(group.center)}, {"radius", group.radius}};
            }
            listed.push_back(entry);
        }
    }
    auto picked = Json::array();
    for (auto count : cull.lodPicked) {
        picked.push_back(count);
    }
    auto placed = Json::array();
    for (auto count : placedSlots) {
        placed.push_back(count);
    }
    Json out = {
        {"frame", cull.lodFrame},
        {"groups", {{"alive", alive}, {"forced", forced}}},
        {"slotsPerLevel", placed},
        {"preference", lodPreferenceName(view.scene.lodPreference())},
        {"tuning", lodTuningJson(view.scene.lodTuning())},
        {"gpu",
         {{"groups", cull.lodGroups},
          {"drawingLevel", picked},
          {"fallbacks", cull.lodFallbacks},
          {"forced", cull.lodForced},
          {"fading", cull.lodFading},
          {"fadingInstances", cull.lodFadingInstances},
          {"popped", cull.lodPopped},
          {"flashing", flashing},
          {"popsSeen", view.lodPops},
          {"slotsOfLevelsNotDrawn", cull.lodDropped}}},
        {"check",
         {{"camera", {{"position", {eye.x, eye.y, eye.z}}, {"windowArea", windowArea}, {"viewWindowArea", viewWindowArea}}},
          {"compared", compared},
          {"differ", differ},
          {"onEdge", onEdge},
          {"fallbacks", fallbacks},
          {"examples", examples}}},
    };
    if (path) {
        out["path"] = *path;
        out["groupsUnder"] = listed;
    }
    return out;
}

auto answerRenderLodRequests(ViewContext& view) -> bool {
    auto& requests = view.renderLodRequests;
    for (auto it = requests.begin(); it != requests.end();) {
        if (view.latestCull.lodFrame > it->askedAt && view.latestCull.lodHasStates) {
            if (it->kind == RenderLodRequest::Kind::Inspect) {
                auto inspection = inspectLodRoot(view.scene, view.latestCull, *it->path, {.eye = view.cam.position, .fovY = glm::radians(cameraFovDegrees), .aspect = view.viewAspect});
                if (inspection) {
                    it->responder.respond(lodInspectionJson(*inspection, view.scene.lodMetersPerUnit()));
                } else {
                    it->responder.fail(rpc::invalidParams, std::format("no LOD root placed at {}", *it->path));
                }
            } else if (it->kind == RenderLodRequest::Kind::Tool) {
                it->responder.respond(lodToolRecord(view));
            } else {
                it->responder.respond(renderLodRecord(view, it->path));
            }
            it = requests.erase(it);
            continue;
        }
        ++it;
    }
    return !requests.empty();
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

// Where the time goes: every zone over the last frames, per thread lane (and the GPU's), by name: how often it ran, its total and longest
// time, and how deep it nests, the most expensive first.
auto zonesRecord() -> Json {
    constexpr uint64_t windowFrames = 60;
    auto latest = profile::lastFrame();
    if (!latest) {
        return {{"frames", 0}, {"lanes", Json::array()}};
    }
    uint64_t frames = 1;
    auto first = *latest;
    for (uint64_t offset = 1; offset < windowFrames; offset++) {
        auto frame = profile::frameAt(offset);
        if (!frame) {
            break;
        }
        first = *frame;
        frames++;
    }
    struct Totals {
        uint64_t count = 0;
        uint64_t totalNs = 0;
        uint64_t maxNs = 0;
        uint16_t depth = UINT16_MAX;
        std::vector<uint64_t> durations;
    };
    auto summarise = [&](const std::vector<profile::Zone>& zones) {
        std::map<uint32_t, Totals> byName;
        for (const auto& zone : zones) {
            auto& totals = byName[zone.nameId];
            auto duration = zone.endNs - zone.startNs;
            totals.count++;
            totals.totalNs += duration;
            totals.maxNs = std::max(totals.maxNs, duration);
            totals.depth = std::min(totals.depth, zone.depth);
            totals.durations.push_back(duration);
        }
        std::vector<std::pair<uint32_t, Totals>> sorted(byName.begin(), byName.end());
        std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.totalNs > b.second.totalNs; });
        auto out = Json::array();
        for (auto& [nameId, totals] : sorted) {
            // The median run, beside the total: one long run (a hitch) and a steady cost read differently.
            std::ranges::nth_element(totals.durations, totals.durations.begin() + (std::ptrdiff_t) (totals.durations.size() / 2));
            auto medianNs = totals.durations[totals.durations.size() / 2];
            out.push_back({
                {"medianMs", (double) medianNs / 1e6},
                {"name", profile::nameOf(nameId)},
                {"depth", totals.depth},
                {"count", totals.count},
                {"totalMs", (double) totals.totalNs / 1e6},
                {"perFrameMs", (double) totals.totalNs / 1e6 / (double) frames},
                {"maxMs", (double) totals.maxNs / 1e6},
            });
        }
        return out;
    };
    std::vector<profile::LaneInfo> laneInfos;
    profile::lanes(laneInfos);
    auto lanes = Json::array();
    std::vector<profile::Zone> zones;
    for (const auto& lane : laneInfos) {
        zones.clear();
        profile::zonesIn(lane.index, first.startNs, latest->endNs, zones);
        lanes.push_back({{"lane", lane.name}, {"zones", summarise(zones)}});
    }
    zones.clear();
    profile::gpuZonesIn(first.startNs, latest->endNs, zones);
    lanes.push_back({{"lane", "GPU"}, {"zones", summarise(zones)}});
    return {
        {"frames", frames},
        {"windowMs", (double) (latest->endNs - first.startNs) / 1e6},
        {"lanes", lanes},
    };
}

} // namespace

auto startFlight(ViewContext& view, const std::string& prefix, float speed, std::optional<RpcResponder> responder) -> std::optional<std::string> {
    std::vector<std::pair<std::string, glm::mat4>> found;
    for (const auto& camera : view.scene.cameras()) {
        if (camera.path.starts_with(prefix)) {
            found.emplace_back(camera.path, camera.worldTransform);
        }
    }
    std::ranges::sort(found, {}, [](const auto& camera) { return camera.first; });
    if (found.size() < 2) {
        return std::format("{} cameras start with '{}': a flight needs two", found.size(), prefix);
    }
    stopFlight(view);
    view.flight.speed = speed;
    for (const auto& [path, transform] : found) {
        Camera placed = view.cam;
        placed.place(transform);
        view.flight.cameras.push_back(path);
        view.flight.eyes.push_back(placed.position);
        view.flight.yaws.push_back(placed.yaw);
        view.flight.pitches.push_back(placed.pitch);
    }
    view.flight.responder = std::move(responder);
    view.cam.position = view.flight.eyes.front();
    view.cam.yaw = view.flight.yaws.front();
    view.cam.pitch = view.flight.pitches.front();
    TRACE_EVENT("Engine", "FlightStarted", prefix).text(std::format("flying {} cameras from {} at {:g} m/s", found.size(), found.front().first, speed));
    return std::nullopt;
}

auto stopFlight(ViewContext& view) -> void {
    if (view.flight.responder) {
        view.flight.responder->respond({{"cameras", view.flight.cameras.size()}, {"metres", view.flight.metres}, {"seconds", view.flight.seconds}, {"stopped", true}});
    }
    view.flight = {};
}

auto advanceFlight(ViewContext& view, float dt) -> void {
    auto& flight = view.flight;
    if (!flight.flying()) {
        return;
    }
    flight.seconds += dt;
    float step = flight.speed * dt;
    while (step > 0.0f && flight.flying()) {
        float length = glm::distance(flight.eyes[flight.segment], flight.eyes[flight.segment + 1]);
        if (flight.along + step < length) {
            flight.along += step;
            flight.metres += step;
            step = 0.0f;
        } else {
            flight.metres += length - flight.along;
            step -= length - flight.along;
            flight.segment++;
            flight.along = 0.0f;
        }
    }
    if (flight.flying()) {
        auto from = flight.segment;
        float length = glm::distance(flight.eyes[from], flight.eyes[from + 1]);
        float t = length > 0.0f ? flight.along / length : 1.0f;
        // Yaw the short way round.
        float turn = std::remainder(flight.yaws[from + 1] - flight.yaws[from], 360.0f);
        view.cam.position = glm::mix(flight.eyes[from], flight.eyes[from + 1], t);
        view.cam.yaw = flight.yaws[from] + (turn * t);
        view.cam.pitch = glm::mix(flight.pitches[from], flight.pitches[from + 1], t);
        return;
    }
    view.cam.position = flight.eyes.back();
    view.cam.yaw = flight.yaws.back();
    view.cam.pitch = flight.pitches.back();
    TRACE_EVENT("Engine", "FlightDone", flight.cameras.back())
        .text(std::format("flew {} cameras, {:.0f} m in {:.1f} s", flight.cameras.size(), flight.metres, flight.seconds))
        .field("metres", flight.metres)
        .field("seconds", flight.seconds);
    if (flight.responder) {
        flight.responder->respond({{"cameras", flight.cameras.size()}, {"metres", flight.metres}, {"seconds", flight.seconds}, {"stopped", false}});
        flight.responder.reset();
    }
}

auto serviceRenderLodRequests(ViewContext& view) -> bool {
    return answerRenderLodRequests(view);
}

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
    stats["scene.lod"] = {std::format("{} roots, heuristic {}", t.lodRootsEvaluated, view.scene.lodHeuristic() ? "on" : "off"),
                          std::format("last: {} finer, {} coarser", t.lodFiner, t.lodCoarser),
                          std::format("unloaded: {} levels, {} contents, {} mesh packs ({:.1f} MiB)", t.releasedLevels, t.releasedContents, t.releasedMeshes, mib(t.releasedMeshBytes))};
    stats["scene.residency"] = {std::format("{} loads, {} reloads, {} released", view.scene.streaming().loads, view.scene.streaming().reloads, view.scene.streaming().released),
                                std::format("{} roots late, {} drawing nothing, {} findings", view.scene.streaming().lateNow, view.scene.streaming().holesNow, view.scene.streaming().findings)};
    stats["scene.compose"] = {std::format("{} composes, last {:.1f} ms", t.composes, t.lastComposeMs),
                              std::format("{} layer packs, {} in flight", t.layerPacks, t.inFlightLayers),
                              std::format("{} contents held, {} released", t.prototypesHeld + t.levelsHeld, t.releasedContents)};
    stats["scene.placement"] = {std::format("{} placements ({} drawn), last change {:.1f} ms", t.placements, t.placementsDrawn, t.lastPlaceMs),
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
    {
        const auto& cull = view.latestCull;
        std::string drawing;
        for (size_t level = 0; level < cull.lodPicked.size(); level++) {
            if (cull.lodPicked[level] > 0) {
                drawing += std::format("{}L{} {}", drawing.empty() ? "" : ", ", level, cull.lodPicked[level]);
            }
        }
        stats["render.lodselect"] = {std::format("{} groups: {}", cull.lodGroups, drawing.empty() ? std::string("none drawing") : drawing),
                                     std::format("{} falling back, {} forced; {} slots of levels not drawn", cull.lodFallbacks, cull.lodForced, cull.lodDropped)};
    }
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
    records.add("scenethread", "the scene thread: requests, composes and placement passes, assets in flight and held, and residency (the script's hold, the load's longest frame)", [&view](RpcResponder responder) {
        responder.respond(sceneThreadRecord(view));
    });
    records.add("gpuscene", "the GPU scene's flow: change lists taken and drawn, the upload queue and its budget, the geometry pool and what waits for room", [&view](RpcResponder responder) {
        responder.respond(gpuSceneRecord(view));
    });
    records.add("streaming", "residency: per streaming root the level selected to draw and why, its streaming radii and state, the levels resident and why, drawn, late, per level requested, loaded, ready and lead, authoring findings; streaming measured (late, holes, loads, reloads, lead)", [&view](RpcResponder responder) {
        responder.respond(streamingRecord(view));
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
    records.add("lod", "every LOD root drawn: its levels, the level it defaults to, wants and draws, the levels loaded, and whether set by hand", [&view](RpcResponder responder) {
        responder.respond(lodRecord(view));
    });
    records.add("streamingtool",
                "the Streaming tool: the inspection of the streaming root at or above the selection (residency and why, heuristics by domain, levels with lead, findings), the totals and the viewport's layers",
                [&view](RpcResponder responder) {
                    // The streaming root at or above the selection: the longest root path that holds it.
                    std::string root;
                    for (const auto& placed : view.scene.lodRoots()) {
                        bool atOrAbove = view.selectedPath == placed.path || view.selectedPath.starts_with(placed.path + "/");
                        if (atOrAbove && placed.path.size() > root.size() && view.scene.lodChoices().contains(placed.path)) {
                            root = placed.path;
                        }
                    }
                    auto inspection = root.empty() ? std::nullopt : inspectStreamingRoot(view.scene, root, view.cam.position, lodWindowArea(glm::radians(cameraFovDegrees), view.viewAspect));
                    const auto& state = view.ui.streamingWindow();
                    const auto& t = state.totals;
                    responder.respond({
                        {"selected", view.selectedPath},
                        {"inspection", inspection ? streamingInspectionJson(*inspection) : Json(nullptr)},
                        {"totals", {{"roots", t.roots}, {"residentLevels", t.residentLevels}, {"inFlight", t.inFlight}, {"lateNow", t.lateNow}, {"holesNow", t.holesNow}}},
                        {"layers", {{"streamingView", state.streamingView}, {"bounds", state.bounds}, {"rings", state.rings}, {"ringsAll", state.ringsAll}, {"labels", state.labels}}},
                        {"window", view.ui.wantsStreamingWindow()},
                    });
                });
    records.add("lodtool",
                "the LOD tool: the inspection of the LOD root at or above the selection (heuristics with their measures and picks, levels with triangles), the LOD tuning, the viewport's layers and the pops seen (next frames)",
                [&view](RpcResponder responder) { view.renderLodRequests.push_back({.responder = std::move(responder), .askedAt = view.latestCull.lodFrame, .kind = RenderLodRequest::Kind::Tool}); });
    records.add("assets", "what the view asked the asset server for and received: totals, and every asset with its state, size and times", [&view](RpcResponder responder) {
        responder.respond(assetsRecord(view));
    });
    records.add("culling", "GPU culling's latest readback: instances and triangles drawn and culled, per view", [&view](RpcResponder responder) {
        responder.respond(cullingRecord(view));
    });
    records.add("renderlod",
                "render LOD: the groups placed and their slots per level, what the GPU picked (levels drawn, fallbacks, forced), and that pick checked against the view's heuristic at the view's camera (next frames)",
                [&view](RpcResponder responder) { view.renderLodRequests.push_back({.responder = std::move(responder), .askedAt = view.latestCull.lodFrame, .path = std::nullopt}); });
    records.add("pipelines", "every pipeline the renderer registered: start-up or first use, state, shaders and their versions, the last error", [&view](RpcResponder responder) {
        responder.respond(pipelinesRecord(view));
    });
    records.add("profile", "the profiler's frame history: CPU and GPU time per frame", [](RpcResponder responder) {
        responder.respond(profileRecord());
    });
    records.add("zones", "where the time goes over the last 60 frames: every profiler zone per thread lane and on the GPU, its count, total, per frame, median and longest time", [](RpcResponder responder) {
        responder.respond(zonesRecord());
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
