#include "streamingtool.h"

#include "debugdraw.h"
#include "viewscene.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace {

auto isUnderPath(const std::string& path, const std::string& base) -> bool {
    return base == "/" || path == base || (path.size() > base.size() && path.starts_with(base) && path[base.size()] == '/');
}

auto kindName(const LodHeuristic& heuristic) -> std::string {
    if (heuristic.kind == LodHeuristicKind::Distance) {
        return "distance";
    }
    if (heuristic.kind == LodHeuristicKind::ScreenSize) {
        return "screen";
    }
    return heuristic.type;
}

// A root's state for the colours, worst first: drawing nothing, drawing coarser than selected, a resident level still on its way, a finer
// level resident ahead of being drawn, drawing what it selects.
auto rootStateOf(const SceneLodRoot& root, const SceneLodChoice* choice, const SceneStreamingRoot* measured) -> StreamingRootState {
    if (measured != nullptr && measured->hole) {
        return StreamingRootState::Hole;
    }
    if (measured != nullptr && measured->late) {
        return StreamingRootState::Late;
    }
    if (choice != nullptr && measured != nullptr) {
        for (auto level : choice->resident) {
            if (level >= measured->ready.size() || !measured->ready[level]) {
                return StreamingRootState::Requested;
            }
        }
        if (root.drawn) {
            for (uint32_t level = 0; level < *root.drawn && level < measured->ready.size(); level++) {
                if (measured->ready[level]) {
                    return StreamingRootState::Prefetched;
                }
            }
        }
    }
    return root.drawn ? StreamingRootState::Drawn : StreamingRootState::None;
}

// The root's streaming bounds in world metres: its streaming heuristic's (else its first usable one's) box or extent, placed by its transform.
auto boundsOf(const SceneLodRoot& root, int32_t heuristicIndex, glm::vec3& outMin, glm::vec3& outMax) -> bool {
    const LodHeuristic* heuristic = heuristicIndex >= 0 ? &root.heuristics[(size_t) heuristicIndex] : nullptr;
    if (heuristic == nullptr) {
        for (const auto& candidate : root.heuristics) {
            if (candidate.usable()) {
                heuristic = &candidate;
                break;
            }
        }
    }
    if (heuristic == nullptr) {
        return false;
    }
    std::array<glm::vec3, 2> box;
    if (heuristic->kind == LodHeuristicKind::Distance && heuristic->box) {
        box = *heuristic->box;
    } else if (heuristic->kind == LodHeuristicKind::ScreenSize) {
        box = heuristic->extent;
    } else {
        return false;
    }
    outMin = glm::vec3(1e30f);
    outMax = glm::vec3(-1e30f);
    for (int corner = 0; corner < 8; corner++) {
        glm::vec3 local((corner & 1) != 0 ? box[1].x : box[0].x, (corner & 2) != 0 ? box[1].y : box[0].y, (corner & 4) != 0 ? box[1].z : box[0].z);
        auto world = glm::vec3(root.worldTransform * glm::vec4(local, 1.0f));
        outMin = glm::min(outMin, world);
        outMax = glm::max(outMax, world);
    }
    return true;
}

auto rowOf(const ViewScene& scene, const SceneLodRoot& root) -> StreamingRow {
    const auto& choices = scene.lodChoices();
    const auto& streaming = scene.streaming();
    const auto& tuning = scene.lodTuning();
    double metersPerUnit = scene.lodMetersPerUnit();
    auto choiceIt = choices.find(root.path);
    const SceneLodChoice* choice = choiceIt != choices.end() ? &choiceIt->second : nullptr;
    auto measuredIt = streaming.roots.find(root.path);
    const SceneStreamingRoot* measured = measuredIt != streaming.roots.end() ? &measuredIt->second : nullptr;

    StreamingRow row;
    row.path = root.path;
    auto slash = root.path.find_last_of('/');
    row.name = slash == std::string::npos ? root.path : root.path.substr(slash + 1);
    row.slot = root.streamSlot;
    row.key = root.key;
    row.tuned = tuning.overrides.contains(root.key);
    row.drawn = root.drawn ? (int32_t) *root.drawn : -1;
    row.selected = (int32_t) root.wanted;
    if (choice != nullptr) {
        row.selected = (int32_t) choice->level;
        row.state = (int32_t) choice->state;
        row.resident = choice->resident;
        row.why = choice->reason;
        row.residentWhy = choice->residentReason;
        for (const auto& finding : choice->findings) {
            row.findings.push_back(std::format("{} at boundary {}: {}", finding.rule, finding.boundary, finding.detail));
        }
    }
    for (size_t level = 0; level < root.levels.size(); level++) {
        StreamingLevel info;
        info.name = root.levels[level];
        info.requested = std::ranges::contains(row.resident, (uint32_t) level);
        info.loaded = std::ranges::contains(root.loaded, (uint32_t) level);
        info.ready = measured != nullptr && level < measured->ready.size() && measured->ready[level];
        info.drawn = row.drawn == (int32_t) level;
        info.lead = measured != nullptr && level < measured->lead.size() ? measured->lead[level] : -1.0;
        row.levels.push_back(std::move(info));
    }
    row.streamingHeuristic = chooseLodHeuristic(root.heuristics, LodDomain::Streaming, scene.lodPreference());
    if (row.streamingHeuristic >= 0) {
        auto radii = tunedLodHeuristic(root.heuristics[(size_t) row.streamingHeuristic], lodOverrideOf(tuning, root.key, (uint32_t) row.streamingHeuristic), tuning);
        bool distance = radii.kind == LodHeuristicKind::Distance;
        // A distance in metres, as the map and the overlays draw it.
        float scale = distance ? (float) metersPerUnit : 1.0f;
        row.kind = distance ? "distance" : "screen";
        row.load = radii.thresholds;
        row.unload = radii.unloadThresholds;
        for (auto& radius : row.load) {
            radius *= scale;
        }
        for (auto& radius : row.unload) {
            radius *= scale;
        }
        if (choice != nullptr && choice->streamingMeasure >= 0.0f) {
            row.measure = choice->streamingMeasure * scale;
        }
    }
    row.rootState = rootStateOf(root, choice, measured);
    row.late = measured != nullptr && measured->late;
    row.hole = measured != nullptr && measured->hole;
    row.reloads = measured != nullptr ? measured->reloads : 0;
    row.hasBounds = boundsOf(root, row.streamingHeuristic, row.boundsMin, row.boundsMax);
    return row;
}

// Rings around a box in the plane across `up` through its middle: the box grown by `radius`, its corners rounded, as a distance to the box
// reaches it.
auto drawRing(DebugDraw& draw, const glm::vec3& boxMin, const glm::vec3& boxMax, const glm::vec3& up, float radius, const glm::vec4& color) -> void {
    auto a = glm::vec3(1.0f, 0.0f, 0.0f);
    auto b = std::abs(up.z) > 0.5f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
    auto middle = (boxMin + boxMax) * 0.5f;
    auto half = (boxMax - boxMin) * 0.5f;
    float ha = std::abs(glm::dot(half, a));
    float hb = std::abs(glm::dot(half, b));
    constexpr int arcSegments = 8;
    // The four corners, counter-clockwise from +a+b, each with its quarter arc.
    std::array<glm::vec2, 4> corners = {glm::vec2(ha, hb), glm::vec2(-ha, hb), glm::vec2(-ha, -hb), glm::vec2(ha, -hb)};
    auto at = [&](glm::vec2 p) {
        return middle + (a * p.x) + (b * p.y);
    };
    std::vector<glm::vec2> points;
    for (int c = 0; c < 4; c++) {
        float start = (float) c * std::numbers::pi_v<float> * 0.5f;
        for (int s = 0; s <= arcSegments; s++) {
            float angle = start + ((float) s / (float) arcSegments * std::numbers::pi_v<float> * 0.5f);
            points.push_back(corners[(size_t) c] + (glm::vec2(std::cos(angle), std::sin(angle)) * radius));
        }
    }
    for (size_t i = 0; i < points.size(); i++) {
        draw.line(at(points[i]), at(points[(i + 1) % points.size()]), color);
    }
}

} // namespace

auto buildStreamingRows(const ViewScene& scene, StreamingToolState& state) -> void {
    state.rows.clear();
    state.slotStates.clear();
    uint32_t residentLevels = 0;
    for (const auto& root : scene.lodRoots()) {
        if (scene.lodChoices().find(root.path) == scene.lodChoices().end()) {
            continue;
        }
        auto row = rowOf(scene, root);
        residentLevels += (uint32_t) row.resident.size();
        if (row.slot > 0) {
            if (state.slotStates.size() < row.slot) {
                state.slotStates.resize(row.slot, 0);
            }
            state.slotStates[row.slot - 1] = (uint32_t) row.rootState | (row.findings.empty() ? 0u : streamingStateFinding);
        }
        state.rows.push_back(std::move(row));
    }
    const auto& measured = scene.streaming();
    auto& totals = state.totals;
    totals.roots = (uint32_t) state.rows.size();
    totals.residentLevels = residentLevels;
    totals.lateNow = measured.lateNow;
    totals.holesNow = measured.holesNow;
    totals.lateSwitches = measured.lateSwitches;
    totals.holes = measured.holes;
    totals.lateSeconds = measured.lateSeconds;
    totals.loads = measured.loads;
    totals.reloads = measured.reloads;
    totals.released = measured.released;
    totals.arrivedUnwanted = measured.arrivedUnwanted;
    totals.findings = measured.findings;
    auto leads = measured.leads;
    std::ranges::sort(leads);
    totals.leads = leads.size();
    totals.leadMin = leads.empty() ? 0.0 : leads.front();
    totals.leadMedian = leads.empty() ? 0.0 : leads[leads.size() / 2];
}

auto inspectStreamingRoot(const ViewScene& scene, const std::string& path, const glm::vec3& eye, float windowArea) -> std::optional<StreamingInspection> {
    const auto& roots = scene.lodRoots();
    auto found = std::ranges::find(roots, path, &SceneLodRoot::path);
    if (found == roots.end() || scene.lodChoices().find(path) == scene.lodChoices().end()) {
        return std::nullopt;
    }
    const auto& root = *found;
    const auto& tuning = scene.lodTuning();
    StreamingInspection out;
    out.row = rowOf(scene, root);
    const auto& row = out.row;
    auto imaging = chooseLodHeuristic(root.heuristics, LodDomain::Imaging, scene.lodPreference());
    double metersPerUnit = scene.lodMetersPerUnit();
    for (size_t h = 0; h < root.heuristics.size(); h++) {
        const auto& authored = root.heuristics[h];
        auto tuned = tunedLodHeuristic(authored, lodOverrideOf(tuning, root.key, (uint32_t) h), tuning);
        StreamingInspection::Heuristic entry;
        entry.domain = authored.domain.empty() ? std::string("(no domain)") : authored.domain;
        entry.kind = kindName(authored);
        entry.path = authored.path;
        entry.used = (int32_t) h == imaging || (int32_t) h == row.streamingHeuristic;
        entry.authored = authored.thresholds;
        entry.authoredUnload = authored.unloadThresholds;
        entry.thresholds = tuned.thresholds;
        entry.unload = tuned.unloadThresholds;
        if (!authored.usable()) {
            entry.why = std::format("unusable: {}", authored.unusable);
        } else if (entry.used) {
            entry.why = authored.domainKind == LodDomain::Streaming ? "used: decides what is resident" : "used: decides what is drawn";
        } else {
            entry.why = std::format("not used: the preference is {} first", lodPreferenceName(scene.lodPreference()));
        }
        if (authored.usable()) {
            entry.measure = lodMeasure(authored, root.worldTransform, metersPerUnit, eye, windowArea);
        }
        out.heuristics.push_back(std::move(entry));
    }
    // How the state came about: which radius the camera is inside or past, and the previous state holding it finer.
    if (row.streamingHeuristic >= 0 && row.measure >= 0.0f && row.state >= 0) {
        bool distance = row.kind == "distance";
        auto past = [&](float edge) {
            return distance ? row.measure >= edge : row.measure <= edge;
        };
        uint32_t byLoad = 0;
        while (byLoad < row.load.size() && byLoad + 1 < row.levels.size() && past(row.load[byLoad])) {
            byLoad++;
        }
        auto unit = distance ? " m" : "";
        if ((uint32_t) row.state < byLoad) {
            auto unload = (size_t) row.state < row.unload.size() ? row.unload[(size_t) row.state] : row.load[(size_t) row.state];
            out.stateWhy = std::format("held at {} since it was finer: past load radius {:g}{} but not unload radius {:g}{} (measure {:.0f}{})", row.state, row.load[(size_t) row.state], unit, unload, unit, row.measure, unit);
        } else if (byLoad == 0) {
            out.stateWhy = std::format("inside load radius {:g}{} (measure {:.0f}{}): the finest level is resident", row.load.empty() ? 0.0f : row.load[0], unit, row.measure, unit);
        } else if (byLoad < row.load.size()) {
            out.stateWhy = std::format("between load radii {:g}{} and {:g}{} (measure {:.0f}{})", row.load[byLoad - 1], unit, row.load[byLoad], unit, row.measure, unit);
        } else {
            out.stateWhy = std::format("past the last load radius {:g}{} (measure {:.0f}{})", row.load[byLoad - 1], unit, row.measure, unit);
        }
    } else {
        out.stateWhy = row.residentWhy;
    }
    // Its parent: the nearest streaming root placed above it; its children: those placed under it.
    for (const auto& other : roots) {
        if (other.path == path || scene.lodChoices().find(other.path) == scene.lodChoices().end()) {
            continue;
        }
        if (isUnderPath(path, other.path) && other.path.size() > out.parent.size()) {
            out.parent = other.path;
        }
        if (isUnderPath(other.path, path)) {
            out.children.push_back(other.path);
        }
    }
    return out;
}

auto drawStreamingOverlays(const StreamingToolState& state, DebugDraw& draw, const glm::vec3& up) -> void {
    if (!toolsconfig::streamingBounds.get() && !toolsconfig::streamingRings.get() && !toolsconfig::streamingRingsAll.get()) {
        return;
    }
    for (const auto& row : state.rows) {
        if (!row.hasBounds || !streamingRowShown(state, row)) {
            continue;
        }
        auto color = streamingStateColor(row.rootState);
        if (toolsconfig::streamingBounds.get()) {
            draw.box(AABB{.min = row.boundsMin, .max = row.boundsMax}, row.findings.empty() ? color : glm::vec4(1.0f, 0.85f, 0.25f, 1.0f));
        }
        bool selected = row.path == state.selectedRoot;
        if ((toolsconfig::streamingRingsAll.get() || (toolsconfig::streamingRings.get() && selected)) && row.kind == "distance") {
            for (size_t k = 0; k < row.load.size(); k++) {
                drawRing(draw, row.boundsMin, row.boundsMax, up, row.load[k], glm::vec4(1.0f, 0.75f, 0.3f, 1.0f));
                if (k < row.unload.size()) {
                    drawRing(draw, row.boundsMin, row.boundsMax, up, row.unload[k], glm::vec4(1.0f, 0.75f, 0.3f, 0.35f));
                }
            }
        }
    }
}

namespace {

// Under the selection: the root at or under the view's selected prim.
auto underPath(const std::string& path, const std::string& base) -> bool {
    return !base.empty() && (base == "/" || path == base || (path.size() > base.size() && path.starts_with(base) && path[base.size()] == '/'));
}

} // namespace

auto streamingStateColor(StreamingRootState state) -> glm::vec4 {
    switch (state) {
        case StreamingRootState::Drawn:
            return {0.30f, 0.75f, 0.35f, 1.0f};
        case StreamingRootState::Prefetched:
            return {0.20f, 0.70f, 0.70f, 1.0f};
        case StreamingRootState::Requested:
            return {0.30f, 0.45f, 0.95f, 1.0f};
        case StreamingRootState::Refused:
            return {0.60f, 0.40f, 0.85f, 1.0f};
        case StreamingRootState::Late:
            return {0.95f, 0.60f, 0.15f, 1.0f};
        case StreamingRootState::Hole:
            return {0.90f, 0.20f, 0.20f, 1.0f};
        case StreamingRootState::None:
            break;
    }
    return {0.35f, 0.35f, 0.35f, 1.0f};
}

auto streamingStateName(StreamingRootState state) -> const char* {
    switch (state) {
        case StreamingRootState::Drawn:
            return "drawn";
        case StreamingRootState::Prefetched:
            return "prefetched";
        case StreamingRootState::Requested:
            return "requested";
        case StreamingRootState::Refused:
            return "refused";
        case StreamingRootState::Late:
            return "late";
        case StreamingRootState::Hole:
            return "hole";
        case StreamingRootState::None:
            break;
    }
    return "no root";
}

auto streamingRowShown(const StreamingToolState& state, const StreamingRow& row) -> bool {
    if (state.filter[0] != '\0' && !row.path.contains(state.filter)) {
        return false;
    }
    if (state.onlyLate && !row.late) {
        return false;
    }
    if (state.onlyHoles && !row.hole) {
        return false;
    }
    if (state.onlyPrefetching && row.rootState != StreamingRootState::Prefetched) {
        return false;
    }
    if (state.onlyFindings && row.findings.empty()) {
        return false;
    }
    if (state.onlyUnderSelection && !underPath(row.path, state.selectedPath)) {
        return false;
    }
    return true;
}

namespace toolsconfig {

config::Bool streamingBounds{"tools.streaming.bounds", false, "Each streaming root's bounds in the viewport, in its state's colour."};
config::Bool streamingRings{"tools.streaming.rings", false, "The selected streaming root's load (solid) and unload (faint) radii in the viewport."};
config::Bool streamingRingsAll{"tools.streaming.ringsAll", false, "The load and unload radii of every streaming root the Streaming window lists."};
config::Bool streamingLabels{"tools.streaming.labels", false, "Over each streaming root: its name, drawn and selected levels and state."};

} // namespace toolsconfig
