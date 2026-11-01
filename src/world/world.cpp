#include "world.h"

#include "primpath.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace {

struct VisitState {
    std::string path;
    glm::dmat4 worldTransform = glm::dmat4(1.0);
    bool visible = true;
    Purpose purpose = Purpose::Default;
    std::string materialAsset;
    bool strongBinding = false;
    bool insideInstance = false;
    bool inUndrawnLevel = false;
    bool inInstancerPrototype = false;
};

} // namespace

auto materialKey(const Entity& material) -> std::string {
    if (material.materialInputs.empty() || material.materialAsset.empty()) {
        return material.materialAsset;
    }
    auto inputs = material.materialInputs;
    std::ranges::sort(inputs);
    std::string key = material.materialAsset;
    char separator = '?';
    for (const auto& [name, value] : inputs) {
        key += std::format("{}{}={}", separator, name, value);
        separator = '&';
    }
    return key;
}

auto parseMaterialKey(const std::string& key) -> MaterialUse {
    MaterialUse use;
    auto query = key.find('?');
    use.asset = key.substr(0, query);
    if (query == std::string::npos) {
        return use;
    }
    size_t at = query + 1;
    while (at < key.size()) {
        auto end = key.find('&', at);
        if (end == std::string::npos) {
            end = key.size();
        }
        auto pair = key.substr(at, end - at);
        auto equals = pair.find('=');
        if (equals != std::string::npos) {
            use.inputs.emplace_back(pair.substr(0, equals), pair.substr(equals + 1));
        }
        at = end + 1;
    }
    return use;
}

auto MaterialUse::input(const std::string& name) const -> std::optional<std::string> {
    for (const auto& [inputName, value] : inputs) {
        if (inputName == name) {
            return value;
        }
    }
    return std::nullopt;
}

auto WorldContent::isInstancerPrototype(EntityId id) const -> bool {
    for (const auto& [instancer, data] : pointInstancers) {
        if (std::ranges::find(data.prototypes, id) == data.prototypes.end()) {
            continue;
        }
        for (EntityId at = entities[id].parent; at != noEntity; at = entities[at].parent) {
            if (at == instancer) {
                return true;
            }
        }
    }
    return false;
}

auto WorldContent::path(EntityId id) const -> std::string {
    if (id == 0) {
        return "/";
    }
    std::vector<const std::string*> names;
    for (EntityId at = id; at != 0 && at != noEntity; at = entities[at].parent) {
        names.push_back(&entities[at].name);
    }
    std::string out;
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        out += "/";
        out += **it;
    }
    return out;
}

auto World::wantedLevel(const std::string& rootPath, const LodRoot& root) const -> uint32_t {
    if (auto it = lodLevels.find(rootPath); it != lodLevels.end()) {
        if (!root.levels.empty()) {
            return std::min(it->second, (uint32_t) root.levels.size() - 1);
        }
        return 0;
    }
    return root.defaultIndex;
}

auto World::drawnLevel(const std::string& rootPath, const LodRoot& root) const -> std::optional<uint32_t> {
    if (root.loaded.empty()) {
        return std::nullopt;
    }
    uint32_t wanted = wantedLevel(rootPath, root);
    // Levels are finest first: the finest loaded level at or after the wanted one; when none is, the coarsest loaded one before it.
    std::optional<uint32_t> best;
    for (uint32_t level : root.loaded) {
        if (level >= wanted && (!best || level < *best)) {
            best = level;
        }
    }
    if (best) {
        return best;
    }
    return *std::ranges::max_element(root.loaded);
}

auto World::prototypeCount() const -> size_t {
    return (size_t) std::ranges::count_if(contents, [](const WorldContent& content) { return content.kind == ContentKind::Prototype; });
}

auto World::visit(const std::function<bool(const VisitedEntity&)>& visitor) const -> void {
    std::function<void(const WorldContent&, EntityId, const VisitState&)> visitChildren;

    auto visitEntity = [&](const WorldContent& content, EntityId id, const VisitState& parent) -> void {
        const auto& entity = content.entities[id];
        VisitState state;
        state.path = primpath::child(parent.path, entity.name);
        state.worldTransform = parent.worldTransform;
        if (entity.xformable) {
            if (entity.resetsXformStack) {
                state.worldTransform = entity.localTransform;
            } else {
                state.worldTransform = parent.worldTransform * entity.localTransform;
            }
        }
        state.visible = parent.visible && (!entity.invisible || entity.lodItem);
        state.purpose = entity.purpose.value_or(parent.purpose);
        state.materialAsset = parent.materialAsset;
        state.strongBinding = parent.strongBinding;
        if (!parent.strongBinding && entity.binding) {
            state.materialAsset = entity.binding->materialAsset;
            state.strongBinding = entity.binding->strongerThanDescendants;
        }
        state.insideInstance = parent.insideInstance;
        state.inUndrawnLevel = parent.inUndrawnLevel;
        state.inInstancerPrototype = parent.inInstancerPrototype || (!content.pointInstancers.empty() && content.isInstancerPrototype(id));

        VisitedEntity visited;
        visited.content = &content;
        visited.id = id;
        visited.entity = &entity;
        visited.path = state.path;
        visited.worldTransform = state.worldTransform;
        visited.visible = state.visible;
        visited.purpose = state.purpose;
        visited.materialAsset = state.materialAsset;
        visited.strongBinding = state.strongBinding;
        visited.insideInstance = state.insideInstance;
        visited.inUndrawnLevel = state.inUndrawnLevel;
        visited.inInstancerPrototype = state.inInstancerPrototype;
        if (!visitor(visited)) {
            return;
        }
        if (entity.prototype >= 0) {
            VisitState inside = state;
            inside.insideInstance = true;
            visitChildren(contents[(size_t) entity.prototype], 0, inside);
            return;
        }
        visitChildren(content, id, state);
    };

    visitChildren = [&](const WorldContent& content, EntityId id, const VisitState& state) -> void {
        const auto& entity = content.entities[id];
        std::optional<uint32_t> drawn;
        const LodRoot* lodRoot = nullptr;
        if (auto it = content.lodRoots.find(id); it != content.lodRoots.end() && it->second.streams) {
            // A render-LOD root's levels are children like any other: which one draws is the renderer's, per placement.
            lodRoot = &it->second;
            drawn = drawnLevel(state.path, *lodRoot);
        }
        for (EntityId child : entity.children) {
            if (lodRoot == nullptr) {
                visitEntity(content, child, state);
                continue;
            }
            auto level = std::ranges::find(lodRoot->levels, child);
            VisitState childState = state;
            if (level != lodRoot->levels.end() && (!drawn || (uint32_t) (level - lodRoot->levels.begin()) != *drawn)) {
                childState.inUndrawnLevel = true;
            }
            // A level is its content's entity 0, the level prim as composed (loaded or not), with everything under it; one still composing
            // is its stand-in alone.
            if (auto linked = content.entities[child].linkedContent; linked >= 0) {
                visitEntity(contents[(size_t) linked], 0, childState);
                continue;
            }
            visitEntity(content, child, childState);
        }
    };

    VisitState root;
    root.path = "/";
    visitChildren(stage, 0, root);
}

auto lodPreferenceName(LodPreference preference) -> const char* {
    switch (preference) {
        case LodPreference::Distance:
            return "distance";
        case LodPreference::ScreenSize:
            return "screen";
        case LodPreference::Authored:
            return "authored";
    }
    return "distance";
}

auto lodPreferenceFromName(std::string_view name) -> std::optional<LodPreference> {
    for (auto preference : {LodPreference::Distance, LodPreference::ScreenSize, LodPreference::Authored}) {
        if (name == lodPreferenceName(preference)) {
            return preference;
        }
    }
    return std::nullopt;
}

auto chooseLodHeuristic(const std::vector<LodHeuristic>& heuristics, LodDomain domain, LodPreference preference) -> int32_t {
    auto first = [&](std::optional<LodHeuristicKind> kind) -> int32_t {
        for (size_t i = 0; i < heuristics.size(); i++) {
            if (heuristics[i].usable() && heuristics[i].domainKind == domain && (!kind || heuristics[i].kind == *kind)) {
                return (int32_t) i;
            }
        }
        return -1;
    };
    if (preference == LodPreference::Authored) {
        return first(std::nullopt);
    }
    auto preferred = preference == LodPreference::Distance ? LodHeuristicKind::Distance : LodHeuristicKind::ScreenSize;
    auto other = preference == LodPreference::Distance ? LodHeuristicKind::ScreenSize : LodHeuristicKind::Distance;
    if (auto index = first(preferred); index >= 0) {
        return index;
    }
    return first(other);
}

auto streamingState(LodHeuristicKind kind, const std::vector<float>& load, const std::vector<float>& unload, float measure, uint32_t levelCount, std::optional<uint32_t> previous) -> uint32_t {
    if (levelCount == 0) {
        return 0;
    }
    auto last = levelCount - 1;
    auto boundaries = (uint32_t) std::min<size_t>(load.size(), last);
    bool distance = kind == LodHeuristicKind::Distance;
    // Whether the measure is at or past an edge toward the coarser side: a distance at or beyond it, a screen size at or below it.
    auto past = [&](float edge) {
        return distance ? measure >= edge : measure <= edge;
    };
    uint32_t byLoad = 0;
    while (byLoad < boundaries && past(load[byLoad])) {
        byLoad++;
    }
    uint32_t byUnload = 0;
    while (byUnload < boundaries && past(byUnload < unload.size() ? unload[byUnload] : load[byUnload])) {
        byUnload++;
    }
    if (!previous || byLoad <= *previous) {
        return byLoad;
    }
    return std::min(byLoad, std::max(std::min(*previous, last), byUnload));
}

auto checkLodAuthoring(const std::vector<LodHeuristic>& heuristics, uint32_t levelCount, const std::vector<LodHeuristic>* parent) -> std::vector<LodAuthoringFinding> {
    std::vector<LodAuthoringFinding> findings;
    if (levelCount == 0) {
        return findings;
    }
    auto boundaries = levelCount - 1;
    for (const auto& heuristic : heuristics) {
        if (heuristic.domainKind != LodDomain::Other && heuristic.thresholds.size() != boundaries) {
            findings.push_back({
                .rule = "a threshold per level boundary",
                .heuristic = heuristic.path,
                .boundary = 0,
                .detail = std::format("{} thresholds for {} levels", heuristic.thresholds.size(), levelCount),
            });
        }
    }
    auto usableDistance = [](const std::vector<LodHeuristic>& list, LodDomain domain) -> const LodHeuristic* {
        for (const auto& heuristic : list) {
            if (heuristic.usable() && heuristic.domainKind == domain && heuristic.kind == LodHeuristicKind::Distance) {
                return &heuristic;
            }
        }
        return nullptr;
    };
    const LodHeuristic* streaming = usableDistance(heuristics, LodDomain::Streaming);
    if (streaming != nullptr) {
        for (const auto& imaging : heuristics) {
            if (!imaging.usable() || imaging.domainKind != LodDomain::Imaging || imaging.kind != LodHeuristicKind::Distance) {
                continue;
            }
            auto count = std::min(imaging.thresholds.size(), streaming->thresholds.size());
            for (size_t k = 0; k < count; k++) {
                if (imaging.thresholds[k] > streaming->thresholds[k]) {
                    findings.push_back({
                        .rule = "imaging threshold beyond the load radius",
                        .heuristic = imaging.path,
                        .boundary = (uint32_t) k,
                        .detail = std::format("drawn at {:g}, loaded at {:g} ({})", imaging.thresholds[k], streaming->thresholds[k], streaming->path),
                    });
                }
            }
        }
        for (size_t k = 0; k < streaming->unloadThresholds.size() && k < streaming->thresholds.size(); k++) {
            if (streaming->unloadThresholds[k] < streaming->thresholds[k]) {
                findings.push_back({
                    .rule = "load radius beyond the unload radius",
                    .heuristic = streaming->path,
                    .boundary = (uint32_t) k,
                    .detail = std::format("loaded at {:g}, unloaded at {:g}", streaming->thresholds[k], streaming->unloadThresholds[k]),
                });
            }
        }
    }
    if (parent != nullptr) {
        const LodHeuristic* parentImaging = usableDistance(*parent, LodDomain::Imaging);
        const LodHeuristic* parentStreaming = usableDistance(*parent, LodDomain::Streaming);
        const LodHeuristic* imaging = usableDistance(heuristics, LodDomain::Imaging);
        if (imaging != nullptr && parentImaging != nullptr && !imaging->thresholds.empty() && !parentImaging->thresholds.empty()) {
            float farthest = *std::ranges::max_element(imaging->thresholds);
            if (farthest > parentImaging->thresholds[0]) {
                findings.push_back({
                    .rule = "child imaging threshold outside the parent's first",
                    .heuristic = parentImaging->path,
                    .boundary = 0,
                    .detail = std::format("child drawn to {:g}, parent switches at {:g}", farthest, parentImaging->thresholds[0]),
                });
            }
        }
        if (streaming != nullptr && parentStreaming != nullptr && !streaming->thresholds.empty() && !parentStreaming->thresholds.empty()) {
            const auto& unload = streaming->unloadThresholds.empty() ? streaming->thresholds : streaming->unloadThresholds;
            float farthest = *std::ranges::max_element(unload);
            if (farthest > parentStreaming->thresholds[0]) {
                findings.push_back({
                    .rule = "child unload radius outside the parent's first load radius",
                    .heuristic = parentStreaming->path,
                    .boundary = 0,
                    .detail = std::format("child resident to {:g}, parent loads its children at {:g}", farthest, parentStreaming->thresholds[0]),
                });
            }
        }
    }
    return findings;
}

auto pickLodLevel(LodHeuristicKind kind, const std::vector<float>& thresholds, const std::vector<float>& blendThresholds, float measure, uint32_t levelCount, uint32_t previous, float hysteresis) -> LodPick {
    if (levelCount == 0) {
        return {};
    }
    auto last = levelCount - 1;
    auto boundaries = (uint32_t) std::min<size_t>(thresholds.size(), last);
    bool distance = kind == LodHeuristicKind::Distance;
    // Whether the measure is at or past an edge toward the coarser side: a distance at or beyond it, a screen size at or below it.
    auto past = [&](float edge) {
        return distance ? measure >= edge : measure <= edge;
    };
    // A boundary's band: its blend threshold, clamped to the next threshold as UsdLod does; none past the authored ones.
    auto band = [&](uint32_t k) -> std::optional<float> {
        if (k >= blendThresholds.size()) {
            return std::nullopt;
        }
        float edge = blendThresholds[k];
        if (k + 1 < thresholds.size()) {
            edge = distance ? std::min(edge, thresholds[k + 1]) : std::max(edge, thresholds[k + 1]);
        }
        return edge;
    };
    float coarserScale = distance ? 1.0f / std::sqrt(1.0f - hysteresis) : 1.0f - hysteresis;
    // Finer: every boundary the measure is fully past. Coarser: the same, a boundary without a band only once past it with the margin.
    uint32_t finer = 0;
    while (finer < boundaries && past(band(finer).value_or(thresholds[finer]))) {
        finer++;
    }
    uint32_t coarser = 0;
    while (coarser < boundaries && past(band(coarser).value_or(thresholds[coarser] * coarserScale))) {
        coarser++;
    }
    uint32_t level = previous;
    if (finer < previous) {
        level = finer;
    } else if (coarser > previous) {
        level = coarser;
    }
    level = std::min(level, last);
    LodPick pick{.level = level};
    // Inside the band past this level's threshold: blending toward the next, as UsdLod's fractional index.
    if (level < boundaries) {
        if (auto edge = band(level); edge && past(thresholds[level]) && !past(*edge)) {
            float span = *edge - thresholds[level];
            if (span != 0.0f) {
                pick.blend = std::clamp((measure - thresholds[level]) / span, 0.0f, 1.0f);
            }
        }
    }
    return pick;
}
