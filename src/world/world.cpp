#include "world.h"

#include <algorithm>

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
};

} // namespace

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
        state.visible = parent.visible && !entity.invisible;
        state.purpose = entity.purpose.value_or(parent.purpose);
        state.materialAsset = parent.materialAsset;
        state.strongBinding = parent.strongBinding;
        if (!parent.strongBinding && entity.binding) {
            state.materialAsset = entity.binding->materialAsset;
            state.strongBinding = entity.binding->strongerThanDescendants;
        }
        state.insideInstance = parent.insideInstance;
        state.inUndrawnLevel = parent.inUndrawnLevel;

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
        if (!visitor(visited)) {
            return;
        }
        if (entity.prototype >= 0) {
            VisitState inside = state;
            inside.insideInstance = true;
            visitChildren(prototypes[(size_t) entity.prototype], 0, inside);
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
            // A level is its content's entity 0, the level prim as composed (loaded or not), with everything under it.
            if (auto linked = content.entities[child].levelContent; linked >= 0) {
                visitEntity(levelContents[(size_t) linked], 0, childState);
                continue;
            }
            visitEntity(content, child, childState);
        }
    };

    VisitState root;
    root.path = "/";
    visitChildren(stage, 0, root);
}
