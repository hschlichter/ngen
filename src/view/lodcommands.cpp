#include "viewcommands.h"

#include "camera.h"
#include "culling.h"
#include "drawlists.h"
#include "lodgroups.h"
#include "primpath.h"
#include "projection.h"
#include "viewscene.h"

#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <expected>
#include <format>
#include <sstream>

// LOD and streaming's commands: the level a root wants, the heuristic and its preference, threshold overrides and the tuning; the records of
// every LOD root, of residency per streaming root, and of render LOD (what the GPU picked, checked against the view's heuristic).

namespace {

using rpc::Json;

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

auto parseOnOff(const std::string& args) -> std::expected<Json, std::string> {
    if (args != "on" && args != "off") {
        return std::unexpected(std::format("expected on or off, got '{}'", args));
    }
    return Json{{"on", args == "on"}};
}

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
            entry["center"] = Json(heuristic.center);
            if (heuristic.box) {
                entry["box"] = {Json((*heuristic.box)[0]), Json((*heuristic.box)[1])};
            }
        } else if (heuristic.kind == LodHeuristicKind::ScreenSize) {
            entry["kind"] = "screen";
            entry["extent"] = {Json(heuristic.extent[0]), Json(heuristic.extent[1])};
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
        bool under = path && !groupPath.empty() && primpath::isAtOrUnder(groupPath, *path);
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
                entry["center"] = Json(group.point);
                if (group.hasBox) {
                    entry["box"] = {Json(group.boxMin), Json(group.boxMax)};
                }
            } else {
                entry["sphere"] = {{"center", Json(group.center)}, {"radius", group.radius}};
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

} // namespace

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

auto serviceRenderLodRequests(ViewContext& view) -> bool {
    // The answers due are taken out before any runs, so an answer may ask for another readback.
    std::vector<std::function<void()>> due;
    std::erase_if(view.renderLodRequests, [&](RenderLodRequest& request) {
        if (view.latestCull.lodFrame > request.askedAt && view.latestCull.lodHasStates) {
            due.push_back(std::move(request.answer));
            return true;
        }
        return false;
    });
    for (auto& answer : due) {
        answer();
    }
    return !view.renderLodRequests.empty();
}

auto registerLodCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records, ViewContext& view) -> void {
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
            .name = "view.renderlod",
            .summary = "The render-LOD groups under a path (each render-LOD root where it is placed): sphere, thresholds, default and forced level, the screen size, and the level the GPU wants and draws; with the `renderlod` record's totals and check. Answers frames later, once the states are read back.",
            .params = {requiredField("path", RpcType::String, "prim path; / for every group")},
            .result = "the renderlod record, with groupsUnder",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto path = p["path"].get<std::string>();
            view.renderLodRequests.push_back({.askedAt = view.latestCull.lodFrame, .answer = [&view, responder, path]() mutable { responder.respond(renderLodRecord(view, path)); }});
        });
    registry.add({.name = "view.streaming.reset", .summary = "Zeroes the streaming counters (late switches, root-seconds coarser, loads, reloads, leads, arrivals unwanted), for a run measured from a known start.", .result = "{}"},
                 [&view](const Json&, RpcResponder responder) {
                     view.scene.resetStreamingCounters();
                     responder.respond(Json::object());
                 });

    verbs.add("lod", "view.lod.set", parseLod);
    verbs.add("lod-reset", "view.lod.reset", parseLodRoot);
    verbs.add("lod-heuristic", "view.lod.heuristic", parseOnOff);
    verbs.add("lod-prefer", "view.lod.heuristic", parseLodPreference);
    verbs.add("lod-thresholds", "view.lod.override", parseLodThresholdList);
    verbs.add("lod-blend-thresholds", "view.lod.override", parseLodBlendList);
    verbs.add("lod-unload-thresholds", "view.lod.override", parseLodUnloadList);
    verbs.add("lod-override-reset", "view.lod.override.reset", parseLodOverrideReset);
    verbs.add("lod-bias", "view.lod.tuning", parseLodBias);
    verbs.add("lod-hysteresis", "view.lod.tuning", parseLodHysteresis);
    verbs.add("lod-blending", "view.lod.tuning", parseLodBlending);
    verbs.add("streaming-reset", "view.streaming.reset", RpcVerbs::none);

    records.add("lod", "every LOD root drawn: its levels, the level it defaults to, wants and draws, the levels loaded, and whether set by hand", [&view](RpcResponder responder) {
        responder.respond(lodRecord(view));
    });
    records.add("streaming", "residency: per streaming root the level selected to draw and why, its streaming radii and state, the levels resident and why, drawn, late, per level requested, loaded, ready and lead, authoring findings; streaming measured (late, holes, loads, reloads, lead)", [&view](RpcResponder responder) {
        responder.respond(streamingRecord(view));
    });
    records.add("renderlod",
                "render LOD: the groups placed and their slots per level, what the GPU picked (levels drawn, fallbacks, forced), and that pick checked against the view's heuristic at the view's camera (next frames)",
                [&view](RpcResponder responder) {
                    view.renderLodRequests.push_back({.askedAt = view.latestCull.lodFrame, .answer = [&view, responder]() mutable { responder.respond(renderLodRecord(view, std::nullopt)); }});
                });
}
