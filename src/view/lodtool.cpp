#include "lodtool.h"

#include "debugdraw.h"
#include "drawlists.h"
#include "lodgroups.h"
#include "viewscene.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace {

auto kindName(const LodHeuristic& heuristic) -> std::string {
    if (heuristic.kind == LodHeuristicKind::Distance) {
        return "distance";
    }
    if (heuristic.kind == LodHeuristicKind::ScreenSize) {
        return "screen";
    }
    return heuristic.type;
}

auto isUnderPath(const std::string& path, const std::string& base) -> bool {
    return base == "/" || path == base || (path.size() > base.size() && path.starts_with(base) && path[base.size()] == '/');
}

// Every heuristic a root names, the one used marked: "distance [49.3, 70.8]* | screen [0.25]"; the thresholds as the view picks with them,
// before the bias.
auto heuristicText(const std::vector<LodHeuristic>& heuristics, int32_t used, const std::string& key, const LodTuning& tuning) -> std::string {
    std::string text;
    for (size_t h = 0; h < heuristics.size(); h++) {
        const auto* override = lodOverrideOf(tuning, key, (uint32_t) h);
        const auto& thresholds = override != nullptr && override->thresholds ? *override->thresholds : heuristics[h].thresholds;
        std::string list;
        for (float threshold : thresholds) {
            list += std::format("{}{:g}", list.empty() ? "" : ", ", threshold);
        }
        text += std::format("{}{} [{}]{}{}", text.empty() ? "" : " | ", kindName(heuristics[h]), list, override != nullptr ? " tuned" : "", (int32_t) h == used ? "*" : "");
    }
    return text;
}

// A streaming root's shape: its used heuristic, tuned, placed by its transform; thresholds in metres for a distance.
auto shapeOf(const LodHeuristic& heuristic, const glm::mat4& toWorld, double metersPerUnit) -> LodShape {
    LodShape shape;
    shape.toWorld = toWorld;
    shape.thresholds = heuristic.thresholds;
    shape.blendThresholds = heuristic.blendThresholds;
    if (heuristic.kind == LodHeuristicKind::Distance) {
        shape.kind = LodShape::Kind::Distance;
        shape.point = heuristic.center;
        if (heuristic.box) {
            shape.hasBox = true;
            shape.boxMin = (*heuristic.box)[0];
            shape.boxMax = (*heuristic.box)[1];
        }
        for (auto& threshold : shape.thresholds) {
            threshold *= (float) metersPerUnit;
        }
        for (auto& threshold : shape.blendThresholds) {
            threshold *= (float) metersPerUnit;
        }
    } else if (heuristic.kind == LodHeuristicKind::ScreenSize) {
        shape.kind = LodShape::Kind::ScreenSize;
        auto worldMin = glm::vec3(toWorld * glm::vec4(heuristic.extent[0], 1.0f));
        auto worldMax = glm::vec3(toWorld * glm::vec4(heuristic.extent[1], 1.0f));
        shape.center = glm::vec3(toWorld * glm::vec4((heuristic.extent[0] + heuristic.extent[1]) * 0.5f, 1.0f));
        shape.radius = glm::length(worldMax - worldMin) * 0.5f;
    }
    return shape;
}

// A render-LOD group's shape, from its record (thresholds already tuned, a distance's in metres).
auto shapeOf(const RenderLodGroup& group) -> LodShape {
    LodShape shape;
    shape.toWorld = group.toWorld;
    shape.thresholds.assign(group.thresholds.begin(), group.thresholds.begin() + group.thresholdCount);
    shape.blendThresholds.assign(group.blendThresholds.begin(), group.blendThresholds.begin() + group.blendCount);
    if (group.thresholdCount == 0 && group.forcedLevel >= 0) {
        // No usable heuristic: placed, without rings.
        shape.kind = LodShape::Kind::Distance;
        return shape;
    }
    if (group.kind == RenderLodKind::Distance) {
        shape.kind = LodShape::Kind::Distance;
        shape.point = group.point;
        shape.hasBox = group.hasBox;
        shape.boxMin = group.boxMin;
        shape.boxMax = group.boxMax;
    } else {
        shape.kind = LodShape::Kind::ScreenSize;
        shape.center = group.center;
        shape.radius = group.radius;
    }
    return shape;
}

auto groupWhy(const RenderLodGroup& group, bool hand, bool heuristicOn) -> std::string {
    if (hand) {
        return "hand-set";
    }
    if (group.forcedLevel >= 0) {
        return heuristicOn ? "no usable heuristic" : "heuristic off";
    }
    return group.kind == RenderLodKind::Distance ? "distance" : "screen size";
}

// The world distance at which a screen size is reached, for a sphere of `radius` metres in a window of `windowArea` at depth 1.
auto distanceForScreenSize(float radius, float size, float windowArea) -> float {
    if (size <= 0.0f || windowArea <= 0.0f) {
        return 0.0f;
    }
    return radius * std::sqrt(glm::pi<float>() / (size * windowArea));
}

// Two unit axes across `up`.
auto planeAxes(const glm::vec3& up) -> std::pair<glm::vec3, glm::vec3> {
    auto normal = glm::normalize(up);
    auto helper = std::abs(normal.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
    auto a = glm::normalize(glm::cross(normal, helper));
    auto b = glm::cross(normal, a);
    return {a, b};
}

// The points at `distance` metres from a box (in the root's space) in the plane through its middle across the root axis nearest `up`: a
// rectangle with rounded corners, drawn in the root's space and placed by `toWorld` (its scale taken as uniform).
auto roundedRing(DebugDraw& draw, const LodShape& shape, const glm::vec3& up, float distance, const glm::vec4& color) -> void {
    auto linear = glm::mat3(shape.toWorld);
    float scale = std::max(1e-6f, glm::length(linear[0]));
    float offset = distance / scale;
    auto upRoot = glm::inverse(linear) * up;
    int axisUp = 0;
    for (int axis = 1; axis < 3; axis++) {
        if (std::abs(upRoot[axis]) > std::abs(upRoot[axisUp])) {
            axisUp = axis;
        }
    }
    int axisA = (axisUp + 1) % 3;
    int axisB = (axisUp + 2) % 3;
    float middle = (shape.boxMin[axisUp] + shape.boxMax[axisUp]) * 0.5f;
    auto toWorld = [&](float a, float b) {
        glm::vec3 local(0.0f);
        local[axisUp] = middle;
        local[axisA] = a;
        local[axisB] = b;
        return glm::vec3(shape.toWorld * glm::vec4(local, 1.0f));
    };
    // The corners' centres, counter-clockwise, and each corner's arc from its start angle.
    const std::array<glm::vec2, 4> corners = {
        glm::vec2(shape.boxMax[axisA], shape.boxMax[axisB]),
        glm::vec2(shape.boxMin[axisA], shape.boxMax[axisB]),
        glm::vec2(shape.boxMin[axisA], shape.boxMin[axisB]),
        glm::vec2(shape.boxMax[axisA], shape.boxMin[axisB]),
    };
    constexpr int arcSegments = 12;
    std::optional<glm::vec3> previous;
    std::optional<glm::vec3> first;
    for (size_t c = 0; c < corners.size(); c++) {
        float start = (float) c * glm::half_pi<float>();
        for (int s = 0; s <= arcSegments; s++) {
            float angle = start + ((float) s / (float) arcSegments * glm::half_pi<float>());
            auto point = toWorld(corners[c].x + (std::cos(angle) * offset), corners[c].y + (std::sin(angle) * offset));
            if (previous) {
                draw.line(*previous, point, color);
            } else {
                first = point;
            }
            previous = point;
        }
    }
    draw.line(*previous, *first, color);
}

auto drawGizmo(const LodWindowRow& row, DebugDraw& draw, const glm::vec3& up, float windowArea, bool selected) -> void {
    const auto& shape = row.shape;
    float alpha = selected ? 1.0f : 0.6f;
    auto drawnColor = row.drawn >= 0 ? lodLevelColor((uint32_t) row.drawn, row.depth, false) : glm::vec4(0.7f, 0.7f, 0.7f, 1.0f);
    drawnColor.a = alpha;
    auto [axisA, axisB] = planeAxes(up);
    auto anchor = shape.anchor();
    // The distance at which a threshold is reached: a distance's own, a screen size's from the window.
    auto distanceOf = [&](float threshold) {
        if (shape.kind == LodShape::Kind::Distance) {
            return threshold;
        }
        return distanceForScreenSize(shape.radius, threshold, windowArea);
    };
    if (shape.kind == LodShape::Kind::Distance) {
        if (shape.hasBox) {
            draw.orientedBox(shape.toWorld, shape.boxMin, shape.boxMax, drawnColor);
        } else {
            float mark = 0.1f * std::max(1e-3f, glm::length(glm::mat3(shape.toWorld)[0]));
            draw.line(anchor - (up * mark), anchor + (up * mark), drawnColor);
            draw.line(anchor - (axisA * mark), anchor + (axisA * mark), drawnColor);
            draw.line(anchor - (axisB * mark), anchor + (axisB * mark), drawnColor);
        }
    } else if (shape.kind == LodShape::Kind::ScreenSize) {
        draw.sphere(shape.center, shape.radius, drawnColor);
    }
    auto ring = [&](float distance, glm::vec4 color) {
        if (distance <= 0.0f || !std::isfinite(distance)) {
            return;
        }
        if (shape.kind == LodShape::Kind::Distance && shape.hasBox) {
            roundedRing(draw, shape, up, distance, color);
        } else {
            draw.circle(anchor, axisA, axisB, distance, color);
        }
    };
    for (size_t t = 0; t < shape.thresholds.size(); t++) {
        auto color = lodLevelColor((uint32_t) t + 1, row.depth, false);
        color.a = alpha;
        ring(distanceOf(shape.thresholds[t]), color);
        if (t < shape.blendThresholds.size()) {
            color.a = alpha * 0.4f;
            ring(distanceOf(shape.blendThresholds[t]), color);
        }
    }
}

} // namespace

auto lodUnitName(double metersPerUnit) -> const char* {
    if (std::abs(metersPerUnit - 1.0) < 1e-9) {
        return "m";
    }
    if (std::abs(metersPerUnit - 0.01) < 1e-9) {
        return "cm";
    }
    return "units";
}

auto buildLodRows(const ViewScene& scene, LodWindowState& state) -> void {
    const auto& snapshot = scene.snapshot();
    LodWindowState::GroupRowsSource source = {
        .drawsRevision = snapshot.revision,
        .tuning = snapshot.lodTuning,
        .handLevels = snapshot.lodHandLevels,
        .preference = (int) snapshot.lodPreference,
        .heuristic = snapshot.lodHeuristic,
    };
    // The LOD roots' rows change with every snapshot (their measures and choices follow the camera); the render-LOD groups' rows only with
    // what they are built from, and their live state comes from the GPU each frame (updateLodRows). The roots come with the draws, so
    // while the group rows are kept the roots' count is too, and their rows are replaced in place.
    bool groupsKept = source == state.groupRowsSource && state.rootRowCount == scene.lodRoots().size();
    std::vector<LodWindowRow> rootRows;
    auto& rows = groupsKept ? rootRows : state.rows;
    rows.clear();
    const auto& tuning = scene.lodTuning();
    auto preference = scene.lodPreference();
    double metersPerUnit = scene.lodMetersPerUnit();
    const char* unit = lodUnitName(metersPerUnit);
    const auto& choices = scene.lodChoices();
    for (const auto& root : scene.lodRoots()) {
        auto used = chooseLodHeuristic(root.heuristics, LodDomain::Imaging, preference);
        auto choice = choices.find(root.path);
        std::string waiting;
        if (root.waiting) {
            waiting = std::format("Waits for {} meshes, {} materials, {} textures and {} levels still composing.",
                                  root.waiting->meshes,
                                  root.waiting->materials,
                                  root.waiting->textures,
                                  root.waiting->composing);
        }
        LodShape shape;
        shape.toWorld = root.worldTransform;
        if (used >= 0) {
            auto tuned = tunedLodHeuristic(root.heuristics[(size_t) used], lodOverrideOf(tuning, root.key, (uint32_t) used), tuning);
            shape = shapeOf(tuned, root.worldTransform, metersPerUnit);
        }
        rows.push_back({
            .path = root.path,
            .depth = root.depth,
            .levels = root.levels,
            .levelPayloads = root.levelPayloads,
            .heuristic = heuristicText(root.heuristics, used, root.key, tuning),
            .key = root.key,
            .overridden = tuning.overrides.contains(root.key),
            .defaultIndex = root.defaultIndex,
            .wanted = root.wanted,
            .drawn = root.drawn ? (int32_t) *root.drawn : -1,
            .loaded = root.loaded,
            .setByHand = root.setByHand,
            .measure = choice != choices.end() ? choice->second.measure : -1.0f,
            .measureUnit = choice != choices.end() && choice->second.kind == LodHeuristicKind::Distance ? unit : "",
            .why = choice != choices.end() ? choice->second.reason : std::string("not evaluated yet"),
            .waiting = waiting,
            .state = choice != choices.end() ? std::optional<uint32_t>(choice->second.state) : std::nullopt,
            .residentWhy = choice != choices.end() ? choice->second.residentReason : std::string(),
            .shape = std::move(shape),
        });
    }
    if (groupsKept) {
        std::ranges::move(rootRows, state.rows.begin());
        return;
    }
    state.groupRowsSource = std::move(source);
    state.rootRowCount = rows.size();
    const auto& draws = *snapshot.draws;
    const auto& hand = scene.lodHandLevels();
    for (uint32_t g = 0; g < draws.drawn.lodGroups.size(); g++) {
        const auto& group = draws.drawn.lodGroups[g];
        if (!group.alive || g >= draws.lodGroupSources.size() || draws.lodGroupSources[g] == nullptr) {
            continue;
        }
        const auto& source = *draws.lodGroupSources[g];
        const auto& path = draws.lodGroupPaths[g];
        auto used = chooseLodHeuristic(source.heuristics, LodDomain::Imaging, preference);
        bool byHand = hand.contains(path);
        rows.push_back({
            .path = path,
            .group = (int32_t) g,
            .depth = source.depth,
            .levels = source.levels,
            .heuristic = heuristicText(source.heuristics, used, source.key, tuning),
            .key = source.key,
            .overridden = tuning.overrides.contains(source.key),
            .defaultIndex = group.defaultLevel,
            .wanted = group.forcedLevel >= 0 ? (uint32_t) group.forcedLevel : group.defaultLevel,
            .drawn = -1,
            .setByHand = byHand,
            .measureUnit = group.kind == RenderLodKind::Distance ? unit : "",
            .why = groupWhy(group, byHand, scene.lodHeuristic()),
            .shape = shapeOf(group),
        });
    }
}

auto updateLodRows(const ViewScene& scene, const CullResult& cull, LodWindowState& state) -> void {
    if (!cull.lodHasStates) {
        return;
    }
    const auto& groups = scene.snapshot().draws->drawn.lodGroups;
    double metersPerUnit = scene.lodMetersPerUnit();
    for (auto& row : state.rows) {
        if (row.group < 0) {
            continue;
        }
        auto g = (size_t) row.group;
        if (g >= cull.lodWanted.size() || g >= groups.size()) {
            continue;
        }
        bool distance = groups[g].kind == RenderLodKind::Distance;
        row.wanted = cull.lodWanted[g];
        row.drawn = cull.lodDrawn[g] == 0xFFu ? -1 : (int32_t) cull.lodDrawn[g];
        row.fadeLevel = g < cull.lodFadeLevel.size() && cull.lodFadeLevel[g] != 0xFFu ? (int32_t) cull.lodFadeLevel[g] : -1;
        row.fade = g < cull.lodFade.size() ? cull.lodFade[g] : 0.0f;
        if (g < cull.lodMeasure.size()) {
            // The GPU measures a distance in metres; the list shows the stage's units, as the thresholds are authored.
            row.measure = distance ? (float) (cull.lodMeasure[g] / metersPerUnit) : cull.lodMeasure[g];
        }
        row.flashing = g < cull.lodFlags.size() && (cull.lodFlags[g] & LodStateFlags::flashing) != 0;
    }
}

auto inspectLodRoot(const ViewScene& scene, const CullResult& cull, const std::string& path, const LodToolCamera& camera) -> std::optional<LodInspection> {
    if (path.empty()) {
        return std::nullopt;
    }
    const auto& draws = *scene.snapshot().draws;
    const auto& tuning = scene.lodTuning();
    auto preference = scene.lodPreference();
    double metersPerUnit = scene.lodMetersPerUnit();
    LodInspection out;
    out.path = path;
    out.metersPerUnit = (float) metersPerUnit;
    const std::vector<LodHeuristic>* heuristics = nullptr;
    glm::mat4 toWorld(1.0f);
    std::vector<std::string> levelNames;
    for (uint32_t g = 0; g < draws.lodGroupPaths.size(); g++) {
        if (draws.lodGroupPaths[g] != path || g >= draws.drawn.lodGroups.size() || !draws.drawn.lodGroups[g].alive || draws.lodGroupSources[g] == nullptr) {
            continue;
        }
        const auto& group = draws.drawn.lodGroups[g];
        const auto& source = *draws.lodGroupSources[g];
        out.renderLod = true;
        out.group = (int32_t) g;
        out.key = source.key;
        out.depth = source.depth;
        out.defaultIndex = group.defaultLevel;
        heuristics = &source.heuristics;
        toWorld = group.toWorld;
        levelNames = source.levels;
        out.setByHand = scene.lodHandLevels().contains(path);
        out.forced = group.forcedLevel >= 0;
        out.why = groupWhy(group, out.setByHand, scene.lodHeuristic());
        if (cull.lodHasStates && g < cull.lodWanted.size()) {
            out.wanted = cull.lodWanted[g];
            out.drawn = cull.lodDrawn[g] == 0xFFu ? -1 : (int32_t) cull.lodDrawn[g];
            out.fadeLevel = g < cull.lodFadeLevel.size() && cull.lodFadeLevel[g] != 0xFFu ? (int32_t) cull.lodFadeLevel[g] : -1;
            out.fade = g < cull.lodFade.size() ? cull.lodFade[g] : 0.0f;
        }
        break;
    }
    const SceneLodRoot* streaming = nullptr;
    if (!out.renderLod) {
        for (const auto& root : scene.lodRoots()) {
            if (root.path == path) {
                streaming = &root;
                break;
            }
        }
        if (streaming == nullptr) {
            return std::nullopt;
        }
        out.key = streaming->key;
        out.depth = streaming->depth;
        out.defaultIndex = streaming->defaultIndex;
        heuristics = &streaming->heuristics;
        toWorld = streaming->worldTransform;
        levelNames = streaming->levels;
        out.setByHand = streaming->setByHand;
        out.wanted = (int32_t) streaming->wanted;
        out.drawn = streaming->drawn ? (int32_t) *streaming->drawn : -1;
        const auto& choices = scene.lodChoices();
        auto choice = choices.find(path);
        out.why = choice != choices.end() ? choice->second.reason : std::string("not evaluated yet");
        out.forced = out.setByHand || out.why == "heuristic off" || out.why == "no usable heuristic";
    }

    auto used = chooseLodHeuristic(*heuristics, LodDomain::Imaging, preference);
    auto windowArea = lodWindowArea(camera.fovY, camera.aspect);
    auto levelCount = (uint32_t) levelNames.size();
    for (size_t h = 0; h < heuristics->size(); h++) {
        const auto& authored = (*heuristics)[h];
        const auto* override = lodOverrideOf(tuning, out.key, (uint32_t) h);
        LodInspection::Heuristic entry;
        entry.kind = kindName(authored);
        entry.path = authored.path;
        entry.used = (int32_t) h == used;
        entry.authored = authored.thresholds;
        entry.authoredBlends = authored.blendThresholds;
        entry.thresholds = override != nullptr && override->thresholds ? *override->thresholds : authored.thresholds;
        entry.blendThresholds = override != nullptr && override->blendThresholds ? *override->blendThresholds : authored.blendThresholds;
        entry.overridden = override != nullptr;
        if (!authored.usable()) {
            entry.why = std::format("unusable: {}", authored.unusable);
        } else if (entry.used) {
            entry.why = std::format("used: the preference is {} first", lodPreferenceName(preference));
        } else {
            entry.why = std::format("not used: the preference ({} first) chose heuristic {}", lodPreferenceName(preference), used);
        }
        if (authored.usable() && levelCount > 0) {
            entry.measure = lodMeasure(authored, toWorld, metersPerUnit, camera.eye, windowArea);
            auto tuned = tunedLodHeuristic(authored, override, tuning);
            auto pick = pickLodLevel(tuned.kind, tuned.thresholds, tuned.blendThresholds, entry.measure, levelCount, 0, tuning.hysteresis);
            entry.picks = (int32_t) pick.level;
            entry.blend = pick.blend;
        }
        out.heuristics.push_back(std::move(entry));
    }

    out.levels.resize(levelNames.size());
    for (size_t level = 0; level < levelNames.size(); level++) {
        out.levels[level].name = levelNames[level];
        out.levels[level].known = out.renderLod;
        out.levels[level].loaded = out.renderLod;
    }
    if (streaming != nullptr) {
        for (auto level : streaming->loaded) {
            if (level < out.levels.size()) {
                out.levels[level].loaded = true;
            }
        }
    }
    // The draws: a group's slots by level; a streaming root's drawn level, its slots under the root's path.
    for (const auto& instance : draws.drawn.meshInstances) {
        if (!instance.alive || instance.lodLevel == 0) {
            continue;
        }
        if (out.renderLod) {
            if (instance.lodGroup != (uint32_t) out.group + 1 || instance.lodLevel > out.levels.size()) {
                continue;
            }
            out.levels[instance.lodLevel - 1].triangles += instance.indexCount / 3;
            out.levels[instance.lodLevel - 1].draws++;
        } else if (out.drawn >= 0 && instance.lodGroup == 0 && instance.lodLevel == (uint32_t) out.drawn + 1 && isUnderPath(scene.primPath(instance.prim), path)) {
            auto& level = out.levels[(size_t) out.drawn];
            level.known = true;
            level.triangles += instance.indexCount / 3;
            level.draws++;
        }
    }
    return out;
}

auto drawLodGizmos(const LodWindowState& state, DebugDraw& draw, const glm::vec3& up, float windowArea) -> void {
    if (!state.gizmo && !state.gizmoAll) {
        return;
    }
    for (const auto& row : state.rows) {
        bool selected = row.path == state.selectedRoot;
        bool wanted = (state.gizmo && selected) || (state.gizmoAll && lodRowShown(state, row));
        if (!wanted || row.shape.kind == LodShape::Kind::None) {
            continue;
        }
        drawGizmo(row, draw, up, windowArea, selected);
    }
}
