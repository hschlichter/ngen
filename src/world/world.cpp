#include "world.h"

#include <algorithm>
#include <format>

namespace {

auto childPath(const std::string& parent, const std::string& name) -> std::string {
    if (parent == "/") {
        return "/" + name;
    }
    return parent + "/" + name;
}

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
        state.path = childPath(parent.path, entity.name);
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
        if (auto it = content.lodRoots.find(id); it != content.lodRoots.end()) {
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
