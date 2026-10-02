#include "viewcommands.h"

#include "camera.h"
#include "debugview.h"
#include "editorui.h"
#include "profile.h"
#include "renderdoccapture.h"
#include "renderer.h"
#include "renderthread.h"
#include "rpcdiscovery.h"
#include "scenequery.h"
#include "sceneupdater.h"
#include "shadowcascades.h"
#include "usdscene.h"
#include "viewdumps.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <print>
#include <sstream>
#include <string_view>
#include <unistd.h>

namespace {

using rpc::Json;

constexpr std::array<const char*, 10> viewModeNames = {"lit", "albedo", "normals", "depth", "shadowfactor", "shadowmap", "shadowuv", "worldpos", "miplevel", "cascades"};

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
    int material = 0;
    int level = 0;
    if (!(in >> material >> level)) {
        return std::unexpected(std::format("expected '<material> <level>' or 'off', got '{}'", args));
    }
    return Json{{"enabled", true}, {"material", material}, {"level", level}};
}

auto parseDumpTexture(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    int material = 0;
    int level = 0;
    std::string path;
    if (!(in >> material >> level >> path)) {
        return std::unexpected(std::format("expected '<material> <level> <path>', got '{}'", args));
    }
    return Json{{"material", material}, {"level", level}, {"path", path}};
}

auto parsePath(const std::string& args) -> std::expected<Json, std::string> {
    return Json{{"path", args}};
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
    ScriptVerb{"select", "view.select", parsePrim},
    ScriptVerb{"translate", "view.translate", parseTranslate},
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
    ScriptVerb{"screenshot", "view.screenshot", parsePath},
    ScriptVerb{"dump-render-debug", "introspect.render", parsePath},
    ScriptVerb{"renderdoc-capture", "renderdoc.capture", parseNothing},
    ScriptVerb{"capture", "capture.request", parseCapture},
    ScriptVerb{"dump-frame", "introspect.frame", parseDir},
    ScriptVerb{"dump-gpuscene", "introspect.gpuscene", parseDir},
    ScriptVerb{"dump-counters", "introspect.counters", parsePath},
    ScriptVerb{"dump-memory", "introspect.memory", parsePath},
    ScriptVerb{"dump-profile", "introspect.profile", parsePath},
    ScriptVerb{"quit", "view.quit", parseNothing},
};

// ---- Helpers for handlers.

auto findPrimOrFail(ViewContext& view, const std::string& path, RpcResponder& responder) -> PrimHandle {
    auto prim = view.usdScene.isOpen() ? view.usdScene.findPrim(path.c_str()) : PrimHandle{};
    if (!prim) {
        responder.fail(rpc::appError, std::format("prim '{}' not found", path));
    }
    return prim;
}

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
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.cam.position = glm::vec3(p["x"].get<float>(), p["y"].get<float>(), p["z"].get<float>());
            view.cam.yaw = p["yaw"].get<float>();
            view.cam.pitch = p["pitch"].get<float>();
            responder.respond(Json::object());
        });
    registry.add({.name = "view.camera.get", .summary = "The camera's position and yaw/pitch.", .result = "{x, y, z, yaw, pitch}"}, [&view](const Json&, RpcResponder responder) {
        responder.respond({{"x", view.cam.position.x}, {"y", view.cam.position.y}, {"z", view.cam.position.z}, {"yaw", view.cam.yaw}, {"pitch", view.cam.pitch}});
    });
    registry.add(
        {
            .name = "view.camera.frame",
            .summary = "Frame the whole scene, or one prim's bounds.",
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
            auto prim = findPrimOrFail(view, target, responder);
            if (!prim) {
                return;
            }
            auto bb = view.sceneQuery.anchorBounds(view.usdScene, prim);
            if (bb.valid()) {
                view.cam.frame(bb, glm::radians(45.0f));
            }
            responder.respond(Json::object());
        });
}

auto registerScene(RpcRegistry& registry, ViewContext& view) -> void {
    registry.add(
        {
            .name = "view.select",
            .summary = "Select a prim, as clicking it does.",
            .params = {requiredField("prim", RpcType::String, "prim path")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto prim = findPrimOrFail(view, p["prim"].get<std::string>(), responder);
            if (prim) {
                view.selectedPrim = prim;
                responder.respond(Json::object());
            }
        });
    registry.add(
        {
            .name = "view.translate",
            .summary = "Offset a prim's local position: a preview edit, as a gizmo drag submits.",
            .params = {
                requiredField("prim", RpcType::String, "prim path"),
                requiredField("offset", RpcType::Vec3, "added to the local position"),
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto path = p["prim"].get<std::string>();
            auto prim = view.usdScene.isOpen() ? view.usdScene.findPrim(path.c_str()) : PrimHandle{};
            const auto* xf = prim ? view.usdScene.getTransform(prim) : nullptr;
            if (xf == nullptr) {
                responder.fail(rpc::appError, std::format("prim '{}' not found or has no transform", path));
                return;
            }
            const auto& o = p["offset"];
            auto local = xf->local;
            local.position += glm::vec3(o[0].get<float>(), o[1].get<float>(), o[2].get<float>());
            view.sceneUpdater.addEdit({.type = SceneEditCommand::Type::SetTransform, .prim = prim, .transform = local, .purpose = SceneEditRequestContext::Purpose::Preview});
            responder.respond(Json::object());
        });
    registry.add({.name = "view.status", .summary = "Frame counter, scene and selection.", .result = "{frame, scene, selected, camera}"}, [&view](const Json&, RpcResponder responder) {
        std::string selected;
        if (view.selectedPrim && view.usdScene.isOpen()) {
            if (const auto* rec = view.usdScene.getPrimRecord(view.selectedPrim)) {
                selected = rec->path;
            }
        }
        responder.respond({
            {"frame", view.frameCounter},
            {"scene", view.sceneLabel},
            {"selected", selected},
            {"camera", {{"position", vec3Json(view.cam.position)}, {"yaw", view.cam.yaw}, {"pitch", view.cam.pitch}}},
        });
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
            view.editorUI.setGBufferViewMode((int) (it - viewModeNames.begin()));
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
            view.editorUI.setDebugView((int) (it - debugViewNames.begin()));
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.window",
            .summary = "Open or close an introspection window.",
            .params = {
                requiredField("name", RpcType::String, "memory, capture, framedebugger, gpuscene, counters, shaders, culling"),
                optionalField("open", RpcType::Bool, "default true"),
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto name = p["name"].get<std::string>();
            if (!view.editorUI.setIntrospectionWindow(name, p.value("open", true))) {
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
                if (!on.is_boolean() || !view.editorUI.setOverlay(name, on.get<bool>())) {
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
                view.editorUI.setCullEnabled(p["enabled"].get<bool>());
            }
            if (p.contains("frozen")) {
                view.editorUI.setCullFrozen(p["frozen"].get<bool>());
            }
            if (p.contains("showCulled")) {
                view.editorUI.setShowCulled(p["showCulled"].get<bool>());
            }
            if (p.contains("overlayView")) {
                view.editorUI.setCullOverlayView(p["overlayView"].get<int>());
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
            auto& settings = view.editorUI.samplerSettingsMutable();
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
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto& settings = view.editorUI.shadowSettingsMutable();
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
            view.editorUI.setDepthPrepass(p["enabled"].get<bool>());
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.inspect",
            .summary = "The texture inspector's capture of one material texture level.",
            .params = {
                requiredField("enabled", RpcType::Bool, "false turns it off"),
                optionalField("material", RpcType::Int, "material index"),
                optionalField("level", RpcType::Int, "mip level"),
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            if (!p["enabled"].get<bool>()) {
                view.renderThread.setTextureInspect({});
            } else {
                view.renderThread.setTextureInspect({.enabled = true, .material = p.value("material", 0u), .level = p.value("level", 0u)});
            }
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.dumpTexture",
            .summary = "Write one mip level of a material texture as PNG.",
            .params = {
                requiredField("material", RpcType::Int, "material index"),
                requiredField("level", RpcType::Int, "mip level"),
                requiredField("path", RpcType::String, "PNG path"),
            },
            .result = "{} once requested; the file is written after the frame",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.renderer.requestTextureDump(p["material"].get<uint32_t>(), p["level"].get<uint32_t>(), p["path"].get<std::string>());
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
            .name = "introspect.render",
            .summary = "The render debug snapshot: device, swapchain, scene tables, per-pass stats, draw log.",
            .params = {optionalField("path", RpcType::String, "also write it here")},
            .result = "the render debug JSON",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.dumps.requestRenderDebug(dumpTarget(p, "path", std::move(responder)));
        });
    registry.add(
        {
            .name = "introspect.memory",
            .summary = "Every allocation and memory heap.",
            .params = {optionalField("path", RpcType::String, "also write it here")},
            .result = "{heaps, allocations, categories, totalBytes}",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.dumps.requestMemory(dumpTarget(p, "path", std::move(responder)));
        });
    registry.add(
        {
            .name = "introspect.counters",
            .summary = "One frame's GPU zones and pipeline statistics per pass.",
            .params = {optionalField("path", RpcType::String, "also write it here")},
            .result = "{frame, gpuFrameMs, zones, passes}",
        },
        [&view](const Json& p, RpcResponder responder) {
            view.dumps.requestCounters(dumpTarget(p, "path", std::move(responder)));
        });
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
            std::println("Profile trace written: {}", path);
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

} // namespace

auto registerViewMethods(RpcRegistry& registry, ViewContext& view) -> void {
    registerCamera(registry, view);
    registerScene(registry, view);
    registerDisplay(registry, view);
    registerIntrospection(registry, view);
}

auto runViewScriptCommand(const RpcRegistry& registry, const SessionCommand& command) -> void {
    const auto* verb = std::ranges::find_if(scriptVerbs, [&](const ScriptVerb& v) { return command.verb == v.verb; });
    if (verb == scriptVerbs.end()) {
        std::println(stderr, "unknown session command '{}'", command.verb);
        return;
    }
    auto params = verb->parse(command.args);
    if (!params) {
        std::println(stderr, "{}: {}", command.verb, params.error());
        return;
    }
    std::string name = verb->verb;
    RpcResponder responder([name](const RpcReply& reply) {
        if (!reply.ok) {
            std::println(stderr, "{}: {}", name, reply.message);
        }
    });
    registry.invoke(verb->method, *params, responder);
}
