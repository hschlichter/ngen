#include "viewcommands.h"

#include "culling.h"
#include "drawlists.h"
#include "renderconfig.h"
#include "renderdoccapture.h"
#include "renderer.h"
#include "renderthread.h"
#include "rendertools.h"
#include "viewdumps.h"

#include <nlohmann/json.hpp>

#include <expected>
#include <filesystem>
#include <format>
#include <sstream>
#include <unistd.h>

// The renderer's display and captures: the texture inspector and dumps, screenshots, frame-graph resource captures, the frame debug and GPU
// scene dumps, RenderDoc; and the records of culling, the pipelines, the render debug snapshot, memory and the GPU counters.

namespace {

using rpc::Json;

// A target from optional "path"/"dir" parameters and the call's responder.
auto dumpTarget(const Json& params, const char* key, RpcResponder responder) -> DumpTarget {
    return {.path = params.value(key, std::string{}), .responder = std::move(responder)};
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

auto parseDir(const std::string& args) -> std::expected<Json, std::string> {
    return Json{{"dir", args}};
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
        return std::unexpected(std::format("expected '<pass|-|+> <resource> <path> [x y w h]', got '{}'", args));
    }
    Json params = {{"pass", pass}, {"resource", resource}, {"path", path}};
    if (width > 0) {
        params["region"] = {{"x", x}, {"y", y}, {"width", width}, {"height", height}};
    }
    return params;
}

auto meshletCountsJson(const MeshletCounts& counts) -> Json {
    return {
        {"tested", counts.tested},
        {"frustumCulled", counts.frustumCulled},
        {"coneCulled", counts.coneCulled},
        {"occluded", counts.occluded},
        {"drawn", counts.drawn},
        {"triangles", counts.triangles},
    };
}

auto regionsJson(const CullResult& cull) -> Json {
    auto regions = Json::array();
    for (uint32_t region = 0; region < cull.regionDraws.size(); region++) {
        if (cull.regionDraws[region] == 0) {
            continue;
        }
        regions.push_back({
            {"region", DrawLists::regionName(region)},
            {"draws", cull.regionDraws[region]},
            {"meshlets", region < cull.regionMeshlets.size() ? cull.regionMeshlets[region] : 0u},
            {"instanceTriangles", cull.regionTriangles[region]},
        });
    }
    return regions;
}

auto cullingRecord(const ViewContext& view) -> Json {
    const auto& cull = view.latestCull;
    auto cascades = Json::array();
    for (uint32_t i = 0; i < cull.cascadeCount; i++) {
        cascades.push_back({
            {"cascade", i},
            {"drawn", cull.cascadeDrawn[i]},
            {"culled", cull.cascadeCulled[i]},
            {"instanceTriangles", cull.cascadeInstanceTriangles[i]},
            {"triangles", cull.cascadeTriangles[i]},
            {"meshlets", meshletCountsJson(cull.cascadeMeshlets[i])},
        });
    }
    return {
        {"frame", cull.frame},
        {"instances", cull.instances},
        {"camera",
         {{"drawn", cull.cameraDrawn},
          {"culled", cull.cameraCulled},
          {"occluded", cull.occluded},
          {"instanceTriangles", cull.cameraInstanceTriangles},
          {"triangles", cull.cameraTriangles},
          {"meshlets", meshletCountsJson(cull.cameraMeshlets)}}},
        {"regions", regionsJson(cull)},
        {"occlusion",
         {{"ran", cull.occlusion},
          {"enabled", renderconfig::cullingOcclusion.get()},
          {"frozen", renderconfig::cullingFrozen.get()},
          {"depthFrom", cull.occlusionCullCamera ? "culling camera" : "rendered camera"},
          {"drawnFirst", cull.earlyDrawn},
          {"drawnSecond", cull.lateDrawn},
          {"occluded", cull.occluded}}},
        {"lodLevelNotDrawn", cull.lodDropped},
        {"regionOverflow", cull.regionOverflow},
        {"sceneTriangles", cull.sceneTriangles},
        {"sceneMeshlets", cull.sceneMeshlets},
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

} // namespace

auto registerDisplayCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records, ViewContext& view) -> void {
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
                view.renderTools.setTextureInspect({});
            } else {
                view.renderTools.setTextureInspect({.enabled = true, .texture = p.value("texture", 0u), .level = p.value("level", 0u)});
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
            view.renderTools.requestTextureDump(p["texture"].get<uint32_t>(), p["level"].get<uint32_t>(), p["path"].get<std::string>());
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
            view.renderTools.requestScreenshot(path);
            view.dumps.trackScreenshot(path, responder, p.value("inline", false));
        });
    registry.add(
        {
            .name = "capture.request",
            .summary = "Capture a frame-graph resource right after a pass (\"\" or \"-\" = end of frame, also static buffers; \"+\" = after "
                       "the pass that writes it last this frame, or the end of the frame when none does).",
            .params = {
                requiredField("pass", RpcType::String, "pass name, \"\" or \"-\", or \"+\""),
                requiredField("resource", RpcType::String, "resource name"),
                optionalField("path", RpcType::String, "also write it: PNG plus .json for textures, rows JSON for buffers"),
                optionalField("region", RpcType::Object, "{x, y, width, height} of a texture"),
            },
            .result = "texture: format, size, channel ranges, texels of small regions; buffer: schema and rows",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto pass = p["pass"].get<std::string>();
            CaptureWatch watch = {.pass = pass == "-" || pass == "+" ? "" : pass, .resource = p["resource"].get<std::string>(), .afterLastWrite = pass == "+", .trigger = 1};
            if (p.contains("region")) {
                const auto& r = p["region"];
                watch.regionX = r.value("x", 0u);
                watch.regionY = r.value("y", 0u);
                watch.regionWidth = r.value("width", 0u);
                watch.regionHeight = r.value("height", 0u);
            }
            view.dumps.requestCapture(watch, dumpTarget(p, "path", std::move(responder)));
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
    registry.add({.name = "renderdoc.capture", .summary = "A RenderDoc capture of the next frame (start with --renderdoc).", .result = "{}"}, [&view](const Json&, RpcResponder responder) {
        if (!view.renderDoc.triggerCapture()) {
            responder.fail(rpc::appError, "RenderDoc is not loaded (start with --renderdoc)");
            return;
        }
        responder.respond(Json::object());
    });

    verbs.add("inspect", "view.inspect", parseInspect);
    verbs.add("dump-texture", "view.dumpTexture", parseDumpTexture);
    verbs.add("screenshot", "view.screenshot", parsePath);
    verbs.add("capture", "capture.request", parseCapture);
    verbs.add("dump-frame", "introspect.frame", parseDir);
    verbs.add("dump-gpuscene", "introspect.gpuscene", parseDir);
    verbs.add("renderdoc-capture", "renderdoc.capture", RpcVerbs::none);

    records.add("culling",
                "GPU culling's latest readback, per view: instances drawn and culled, meshlets tested and culled by frustum, cone and depth, triangles "
                "drawn after meshlet culling (instanceTriangles: before it)",
                [&view](RpcResponder responder) {
                    responder.respond(cullingRecord(view));
                });
    records.add("pipelines", "every pipeline the renderer registered: start-up or first use, state, shaders and their versions, the last error", [&view](RpcResponder responder) {
        responder.respond(pipelinesRecord(view));
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
}
