#include "placement.h"

#include "profile.h"
#include "trace.h"
#include "viewscene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <format>

namespace {

auto msSince(std::chrono::steady_clock::time_point start) -> double {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

auto childPathOf(const std::string& parent, const std::string& name) -> std::string {
    if (parent == "/") {
        return "/" + name;
    }
    return parent + "/" + name;
}

// A path relative to a content's root ("/" for the root itself), made full where the content is placed.
auto joinPathOf(const std::string& base, const std::string& relative) -> std::string {
    if (relative == "/") {
        return base;
    }
    if (base == "/") {
        return relative;
    }
    return base + relative;
}

// Whether `path` is `base` or under it.
auto isUnder(const std::string& path, const std::string& base) -> bool {
    if (base == "/") {
        return true;
    }
    return path == base || (path.size() > base.size() && path.starts_with(base) && path[base.size()] == '/');
}

auto kindChar(Placement::Kind kind) -> char {
    switch (kind) {
        case Placement::Kind::Stage:
            return 'S';
        case Placement::Kind::Prototype:
            return 'P';
        case Placement::Kind::Level:
            return 'L';
        case Placement::Kind::Instancer:
            return 'I';
    }
    return 'S';
}

// The kind of the content a placement places: an Instancer's points come from the content holding the PointInstancer.
auto placedContentKind(const Placement& node) -> Placement::Kind {
    return node.kind == Placement::Kind::Instancer ? node.contentKind : node.kind;
}

} // namespace

auto findTransform(const Placement& tree, const std::string& path) -> std::optional<EntityTransform> {
    // The deepest drawn placement at or above the path that holds it: a prim is in the content placed nearest above it. A PointInstancer's
    // points aren't looked into: the instancer prim itself is in the content holding it.
    auto deeper = [&](const std::shared_ptr<Placement>& child) -> std::optional<EntityTransform> {
        if (!child || !child->drawn || child->kind == Placement::Kind::Instancer || !isUnder(path, child->path)) {
            return std::nullopt;
        }
        return findTransform(*child, path);
    };
    for (const auto& child : tree.children) {
        if (auto found = deeper(child)) {
            return found;
        }
    }
    for (const auto& root : tree.roots) {
        for (const auto& level : root.levels) {
            if (auto found = deeper(level)) {
                return found;
            }
        }
    }
    if (tree.segments.empty() || !isUnder(path, tree.path)) {
        return std::nullopt;
    }
    const auto& segment = tree.segments.front();
    std::string relative = "/";
    if (tree.path == "/") {
        relative = path;
    } else if (path != tree.path) {
        relative = path.substr(tree.path.size());
    }
    auto prim = segment.walk->transforms->find(relative);
    if (prim == segment.walk->transforms->end()) {
        return std::nullopt;
    }
    auto out = prim->second;
    if (!out.absolute) {
        out.world = segment.world * out.world;
    }
    return out;
}

auto gatherRecords(const Placement& tree) -> PlacementRecords {
    PlacementRecords out;
    // The mesh prims a placement draws and those below it: a prim index per drawn mesh prim and per light.
    std::function<uint64_t(const Placement&)> visit = [&](const Placement& node) -> uint64_t {
        if (!node.drawn) {
            return 0;
        }
        out.cameras.insert(out.cameras.end(), node.cameras.begin(), node.cameras.end());
        out.payloads.insert(out.payloads.end(), node.payloads.begin(), node.payloads.end());
        out.variantPrims.insert(out.variantPrims.end(), node.variantPrims.begin(), node.variantPrims.end());
        size_t instancerRow = 0;
        if (node.instancerRow) {
            instancerRow = out.instancers.size();
            out.instancers.push_back(*node.instancerRow);
        }
        for (const auto& root : node.roots) {
            if (root.listed) {
                out.lodRoots.push_back(root.row);
            }
        }
        uint64_t primsDrawn = node.prims.size() - node.lights.size();
        for (const auto& child : node.children) {
            primsDrawn += visit(*child);
        }
        for (const auto& root : node.roots) {
            for (const auto& level : root.levels) {
                if (level) {
                    primsDrawn += visit(*level);
                }
            }
        }
        if (node.instancerRow) {
            out.instancers[instancerRow].primsDrawn = primsDrawn;
        }
        return primsDrawn;
    };
    visit(tree);
    return out;
}

auto SceneDraws::records() const -> const PlacementRecords& {
    std::call_once(recordsGathered, [this] {
        if (placements) {
            gathered = gatherRecords(*placements);
        }
    });
    return gathered;
}

auto SceneDraws::slotsByPrim() const -> const std::vector<std::vector<uint32_t>>& {
    std::call_once(primIndexBuilt, [this] {
        primSlots.resize(primPaths.size() + 1);
        for (size_t slot = 0; slot < drawn.meshInstances.size(); slot++) {
            const auto& instance = drawn.meshInstances[slot];
            if (instance.alive && instance.prim < primSlots.size()) {
                primSlots[instance.prim].push_back((uint32_t) slot);
            }
        }
    });
    return primSlots;
}

auto SceneDraws::drawsByAsset() const -> const std::unordered_map<std::string, std::vector<uint32_t>>& {
    std::call_once(assetIndexBuilt, [this] {
        for (size_t i = 0; i < assetsByPrim.size(); i++) {
            if (!assetsByPrim[i]) {
                continue;
            }
            for (const auto& asset : *assetsByPrim[i]) {
                assetIndex[asset].push_back((uint32_t) i + 1);
            }
        }
    });
    return assetIndex;
}

// A use with a gizmo's local opinion under it is walked afresh, not taken from the kept walks.
auto ViewScene::overridesUnder(const std::string& usePath) const -> bool {
    if (localOverrides.empty()) {
        return false;
    }
    return std::ranges::any_of(localOverrides, [&](const auto& entry) { return isUnder(entry.first, usePath); });
}

auto ViewScene::buildWalk(const WorldContent& content, bool rootIsPrim, Purpose purpose, const std::string& material, bool strong, const std::string& overrideBase, EntityId root)
    -> std::shared_ptr<const ContentWalk> {
    stats.walksBuilt++;
    auto walkRoot = root != noEntity ? root : (rootIsPrim ? (EntityId) 0 : noEntity);
    auto out = std::make_shared<ContentWalk>();
    out->version = content.version;
    auto prims = std::make_shared<ContentTransforms>();
    struct State {
        std::string path;
        glm::mat4 world = glm::mat4(1.0f);
        bool absolute = false;
        Purpose purpose = Purpose::Default;
        std::string material;
        bool strong = false;
        int32_t lodRootItem = -1; // under a level of a render-LOD root: the root's item and the level
        uint32_t lodLevel = 0;
    };
    std::function<void(EntityId, const State&)> walk = [&](EntityId id, const State& parent) {
        const auto& entity = content.entities[id];
        State state;
        state.path = id == walkRoot ? std::string("/") : childPathOf(parent.path, entity.name);
        glm::mat4 local(entity.localTransform);
        if (!overrideBase.empty() && entity.xformable) {
            if (auto authored = localOverrides.find(joinPathOf(overrideBase, state.path)); authored != localOverrides.end()) {
                local = authored->second;
            }
        }
        state.absolute = parent.absolute;
        state.lodRootItem = parent.lodRootItem;
        state.lodLevel = parent.lodLevel;
        if (!entity.xformable) {
            state.world = parent.world;
        } else if (entity.resetsXformStack) {
            state.world = local;
            state.absolute = true;
        } else {
            state.world = parent.world * local;
        }
        // A LOD item's own visibility is for readers that don't select LOD; the drawn level is shown whatever it authors.
        if (!entity.active || !entity.defined || entity.abstract || (entity.invisible && !entity.lodItem)) {
            return;
        }
        state.purpose = entity.purpose.value_or(parent.purpose);
        state.material = parent.material;
        state.strong = parent.strong;
        if (!parent.strong && entity.binding) {
            state.material = entity.binding->materialAsset;
            state.strong = entity.binding->strongerThanDescendants;
        }
        (*prims)[state.path] = {.local = local, .world = state.world, .xformable = entity.xformable, .resetsXformStack = entity.resetsXformStack, .absolute = state.absolute};
        if (id != walkRoot && !content.pointInstancers.empty() && content.isInstancerPrototype(id)) {
            // A PointInstancer's prototype beneath it is drawn per point, not where it is.
            return;
        }
        WalkItem prim{.kind = WalkItem::Kind::Prim,
                      .path = state.path,
                      .world = state.world,
                      .absolute = state.absolute,
                      .entity = &entity,
                      .id = id,
                      .purpose = state.purpose,
                      .material = state.material,
                      .strongBinding = state.strong,
                      .camera = entity.typeName == "Camera",
                      .payload = content.payloads.contains(id),
                      .lodRoot = content.lodRoots.contains(id) && content.lodRoots.at(id).streams,
                      .light = content.lights.contains(id),
                      .variants = !entity.variantSelections.empty(),
                      .renderLodRoot = content.lodRoots.contains(id) && !content.lodRoots.at(id).streams,
                      .lodRootItem = state.lodRootItem,
                      .lodLevel = state.lodLevel};
        bool drawsMesh = !entity.meshAsset.empty();
        if (drawsMesh) {
            prim.subsets = std::ranges::any_of(entity.children, [&](EntityId child) { return content.entities[child].binding.has_value(); });
        }
        int32_t primItem = -1;
        if (drawsMesh || prim.camera || prim.payload || prim.lodRoot || prim.light || prim.variants || prim.renderLodRoot) {
            auto index = (uint32_t) out->items.size();
            primItem = (int32_t) index;
            if (drawsMesh) {
                out->meshItems.push_back(index);
                if (!std::ranges::contains(out->meshAssets, entity.meshAsset)) {
                    out->meshAssets.push_back(entity.meshAsset);
                }
            }
            if (prim.camera || prim.payload || prim.lodRoot || prim.variants) {
                out->rowItems.push_back(index);
            }
            out->items.push_back(std::move(prim));
        }
        if (content.pointInstancers.contains(id)) {
            out->items.push_back({.kind = WalkItem::Kind::PointInstancer,
                                  .path = state.path,
                                  .world = state.world,
                                  .absolute = state.absolute,
                                  .id = id,
                                  .purpose = state.purpose,
                                  .material = state.material,
                                  .strongBinding = state.strong,
                                  .lodRootItem = state.lodRootItem,
                                  .lodLevel = state.lodLevel});
        }
        if (entity.prototype >= 0) {
            out->items.push_back({.kind = WalkItem::Kind::Instance,
                                  .path = state.path,
                                  .world = state.world,
                                  .absolute = state.absolute,
                                  .purpose = state.purpose,
                                  .material = state.material,
                                  .strongBinding = state.strong,
                                  .content = entity.prototype,
                                  .lodRootItem = state.lodRootItem,
                                  .lodLevel = state.lodLevel});
            return;
        }
        if (auto lod = content.lodRoots.find(id); lod != content.lodRoots.end() && !lod->second.streams) {
            // A render-LOD root's levels are walked like its other children, what is under a level tagged with the root's item and the
            // level: the renderer draws the level the root's group picks.
            for (EntityId child : entity.children) {
                auto level = std::ranges::find(lod->second.levels, child);
                if (level == lod->second.levels.end()) {
                    walk(child, state);
                    continue;
                }
                State levelState = state;
                levelState.lodRootItem = primItem;
                levelState.lodLevel = (uint32_t) (level - lod->second.levels.begin());
                walk(child, levelState);
            }
            return;
        }
        if (auto lod = content.lodRoots.find(id); lod != content.lodRoots.end()) {
            // A level's link to its content changes at resolution without the content changing: it is read where the content is placed.
            for (uint32_t i = 0; i < lod->second.levels.size(); i++) {
                auto standIn = lod->second.levels[i];
                out->items.push_back({.kind = WalkItem::Kind::Level,
                                      .path = childPathOf(state.path, content.entities[standIn].name),
                                      .world = state.world,
                                      .absolute = state.absolute,
                                      .id = standIn,
                                      .purpose = state.purpose,
                                      .material = state.material,
                                      .strongBinding = state.strong,
                                      .root = id,
                                      .level = i});
            }
            return;
        }
        for (EntityId child : entity.children) {
            walk(child, state);
        }
    };
    State top{.path = "/", .world = glm::mat4(1.0f), .absolute = false, .purpose = purpose, .material = material, .strong = strong};
    if (walkRoot != noEntity) {
        walk(walkRoot, top);
    } else {
        for (EntityId child : content.entities[0].children) {
            walk(child, top);
        }
    }
    out->transforms = std::move(prims);
    return out;
}

auto ViewScene::walkOf(Placement::Kind kind, int32_t index, const WorldContent& content, Purpose purpose, const std::string& material, bool strong, const std::string& usePath, EntityId root)
    -> std::shared_ptr<const ContentWalk> {
    bool rootIsPrim = kind == Placement::Kind::Level;
    if (overridesUnder(usePath)) {
        return buildWalk(content, rootIsPrim, purpose, material, strong, usePath, root);
    }
    auto key = std::format("{}{} {} {} {} {} {}", kindChar(kind), index, root, content.version, (int) purpose, strong ? 1 : 0, material);
    if (auto it = walks.find(key); it != walks.end()) {
        return it->second;
    }
    auto built = buildWalk(content, rootIsPrim, purpose, material, strong, {}, root);
    walks[key] = built;
    return built;
}

auto ViewScene::contentOf(const World& w, Placement::Kind kind, int32_t index) const -> const WorldContent& {
    if (kind == Placement::Kind::Stage || index < 0) {
        return w.stage;
    }
    return w.contents[(size_t) index];
}

auto ViewScene::contentReady(const World& w, Placement::Kind kind, int32_t index, Purpose purpose) -> bool {
    auto key = ((int64_t) kind << 40) | ((int64_t) purpose << 36) | (int64_t) index;
    if (auto it = readiness.find(key); it != readiness.end()) {
        return it->second;
    }
    readiness[key] = false; // a cycle can't make it ready
    const auto& content = contentOf(w, kind, index);
    // A linked content is complete: what it waits for is its meshes and the contents it uses.
    auto walked = walkOf(kind, index, content, purpose, {}, false, content.sourcePath);
    bool ready = itemsReady(w, kind, index, content, *walked);
    readiness[key] = ready;
    return ready;
}

auto ViewScene::itemsReady(const World& w, Placement::Kind kind, int32_t index, const WorldContent& content, const ContentWalk& walked) -> bool {
    bool ready = true;
    for (const auto& item : walked.items) {
        if (item.kind == WalkItem::Kind::PointInstancer) {
            // Its prototypes' walks, as the points draw them.
            for (EntityId prototype : content.pointInstancers.at(item.id).prototypes) {
                if (prototype != noEntity) {
                    auto prototypeWalk = walkOf(kind, index, content, item.purpose, item.material, item.strongBinding, content.sourcePath, prototype);
                    ready = itemsReady(w, kind, index, content, *prototypeWalk);
                }
                if (!ready) {
                    break;
                }
            }
        } else if (item.kind == WalkItem::Kind::Prim) {
            const auto& meshAsset = item.entity->meshAsset;
            if (!meshAsset.empty() && drawsPurpose(item.purpose) && !meshPacks.contains(meshAsset) && !failedAssets.contains(meshAsset)) {
                ready = false;
            }
        } else if (item.kind == WalkItem::Kind::Instance) {
            ready = contentReady(w, Placement::Kind::Prototype, item.content, item.purpose);
        } else if (item.level + 1 == content.lodRoots.at(item.root).levels.size()) {
            // A LOD root in it counts by its coarsest level, the floor it draws while a finer one loads.
            auto linked = content.entities[item.id].linkedContent;
            ready = linked >= 0 && contentReady(w, Placement::Kind::Level, linked, item.purpose);
        }
        if (!ready) {
            break;
        }
    }
    return ready;
}

auto ViewScene::countWaiting(const World& w, Placement::Kind kind, int32_t index, Purpose purpose, std::set<int64_t>& seenContents, std::set<std::string>& seenMeshes, SceneLodRoot::Waiting& waiting)
    -> void {
    auto key = ((int64_t) kind << 40) | ((int64_t) purpose << 36) | (int64_t) index;
    if (!seenContents.insert(key).second) {
        return;
    }
    const auto& content = contentOf(w, kind, index);
    auto walked = walkOf(kind, index, content, purpose, {}, false, content.sourcePath);
    std::vector<const ContentWalk*> pending = {walked.get()};
    std::vector<std::shared_ptr<const ContentWalk>> prototypeWalks;
    for (size_t at = 0; at < pending.size(); at++) {
        for (const auto& item : pending[at]->items) {
            if (item.kind == WalkItem::Kind::PointInstancer) {
                for (EntityId prototype : content.pointInstancers.at(item.id).prototypes) {
                    if (prototype != noEntity) {
                        prototypeWalks.push_back(walkOf(kind, index, content, item.purpose, item.material, item.strongBinding, content.sourcePath, prototype));
                        pending.push_back(prototypeWalks.back().get());
                    }
                }
            } else if (item.kind == WalkItem::Kind::Prim) {
                const auto& meshAsset = item.entity->meshAsset;
                if (meshAsset.empty() || !drawsPurpose(item.purpose) || meshPacks.contains(meshAsset) || failedAssets.contains(meshAsset)) {
                    continue;
                }
                if (seenMeshes.insert(meshAsset).second) {
                    waiting.meshesMissing++;
                    if (inFlight.contains(meshAsset)) {
                        waiting.meshesInFlight++;
                    }
                }
            } else if (item.kind == WalkItem::Kind::Instance) {
                countWaiting(w, Placement::Kind::Prototype, item.content, item.purpose, seenContents, seenMeshes, waiting);
            } else if (item.level + 1 == content.lodRoots.at(item.root).levels.size()) {
                // A floor not linked yet is still composing.
                auto linked = content.entities[item.id].linkedContent;
                if (linked >= 0) {
                    countWaiting(w, Placement::Kind::Level, linked, item.purpose, seenContents, seenMeshes, waiting);
                } else {
                    waiting.contentsIncomplete++;
                }
            }
        }
    }
}

auto ViewScene::drawnLevelOf(const World& w, const std::string& rootPath, const LodRoot& root, const WorldContent& content, Purpose purpose) -> std::optional<uint32_t> {
    if (root.loaded.empty()) {
        return std::nullopt;
    }
    auto wanted = w.wantedLevel(rootPath, root);
    // Loaded levels are finest first (set order): the first at or coarser than the wanted one that is ready.
    for (uint32_t level : root.loaded) {
        if (level < wanted) {
            continue;
        }
        auto linked = level < root.levels.size() ? content.entities[root.levels[level]].linkedContent : -1;
        if (linked >= 0 && contentReady(w, Placement::Kind::Level, linked, purpose)) {
            return level;
        }
    }
    // Nothing ready yet: the coarsest loaded level, as it arrives (the scene opening).
    return *std::ranges::max_element(root.loaded);
}

auto ViewScene::touchSlot(uint32_t slot) -> void {
    if (slot < slotCountBefore) {
        slotsBefore.try_emplace(slot, drawn.meshInstances[slot]);
    }
}

// A slot's draw, with the uses of its mesh and material counted.
auto ViewScene::setSlot(uint32_t slot, const RenderMeshInstance& instance) -> void {
    touchSlot(slot);
    const auto& before = drawn.meshInstances[slot];
    auto release = [](std::unordered_map<uint32_t, uint32_t>& uses, uint32_t index) {
        auto it = uses.find(index);
        if (it != uses.end() && --it->second == 0) {
            uses.erase(it);
        }
    };
    if (before.alive) {
        release(meshUses, before.mesh.index);
        release(materialUses, before.material.index);
    }
    drawn.meshInstances.edit(slot) = instance;
    if (instance.alive) {
        meshUses[instance.mesh.index]++;
        materialUses[instance.material.index]++;
    }
}

auto ViewScene::allocSlot() -> uint32_t {
    if (!freeSlots.empty()) {
        auto slot = *freeSlots.begin();
        freeSlots.erase(freeSlots.begin());
        return slot;
    }
    drawn.meshInstances.push_back(RenderMeshInstance{.primFirst = false, .alive = false});
    return (uint32_t) drawn.meshInstances.size() - 1;
}

auto ViewScene::freeSlot(uint32_t slot) -> void {
    setSlot(slot, RenderMeshInstance{.primFirst = false, .alive = false});
    freeSlots.insert(slot);
}

auto ViewScene::allocPrim(const std::string& path) -> uint32_t {
    uint32_t prim = 0;
    if (!freePrims.empty()) {
        prim = *freePrims.begin();
        freePrims.erase(freePrims.begin());
        primPaths.edit(prim - 1) = path;
    } else {
        primPaths.push_back(path);
        assetsByPrim.push_back(nullptr);
        prim = (uint32_t) primPaths.size();
    }
    return prim;
}

auto ViewScene::freePrim(uint32_t prim) -> void {
    primPaths.edit(prim - 1).clear();
    if (assetsByPrim[prim - 1]) {
        assetsByPrim.edit(prim - 1).reset();
    }
    if (lightsByPrim.erase(prim) > 0) {
        lightsChanged = true;
    }
    freePrims.insert(prim);
}

// What a placement counts for while it is in the tree: the placements and drawn ones, its content's placements (and the version placed),
// and its walks' uses, through which the mesh assets it names keep their packs.
auto ViewScene::useNode(const Placement& node, int delta) -> void {
    placementCount += delta;
    if (node.drawn) {
        drawnPlacementCount += delta;
    }
    if (node.kind == Placement::Kind::Prototype || node.kind == Placement::Kind::Level) {
        auto key = ((int64_t) node.kind << 32) | (int64_t) (uint32_t) node.content;
        auto& count = contentPlacements[key];
        count += delta;
        if (count == 0) {
            contentPlacements.erase(key);
            placedVersions.erase(key);
        } else {
            placedVersions[key] = node.version;
        }
    }
    std::vector<const ContentWalk*> seen;
    for (const auto& segment : node.segments) {
        const auto* walk = segment.walk.get();
        if (std::ranges::contains(seen, walk)) {
            continue;
        }
        seen.push_back(walk);
        auto& uses = walkUses[walk];
        bool first = uses == 0;
        uses += delta;
        bool last = uses == 0;
        if (last) {
            walkUses.erase(walk);
        }
        if (!first && !last) {
            continue;
        }
        for (const auto& meshAsset : walk->meshAssets) {
            if (first) {
                meshNames[meshAsset]++;
            } else if (auto it = meshNames.find(meshAsset); it != meshNames.end() && --it->second == 0) {
                meshNames.erase(it);
            }
        }
    }
}

auto ViewScene::releasePlacement(const Placement& node) -> void {
    for (auto slot : node.slots) {
        freeSlot(slot);
    }
    for (auto prim : node.prims) {
        freePrim(prim);
    }
    for (auto group : node.lodGroups) {
        freeLodGroup(group);
    }
    useNode(node, -1);
    for (const auto& child : node.children) {
        releasePlacement(*child);
    }
    for (const auto& root : node.roots) {
        for (const auto& level : root.levels) {
            if (level) {
                releasePlacement(*level);
            }
        }
    }
}

auto ViewScene::allocLodGroup() -> uint32_t {
    if (!freeLodGroups.empty()) {
        auto group = *freeLodGroups.begin();
        freeLodGroups.erase(freeLodGroups.begin());
        return group;
    }
    drawn.lodGroups.push_back(RenderLodGroup{});
    lodGroupInfo.emplace_back();
    lodGroupPaths.push_back({});
    return (uint32_t) drawn.lodGroups.size() - 1;
}

auto ViewScene::setLodGroup(uint32_t group, const RenderLodGroup& value) -> void {
    if (group < lodGroupCountBefore) {
        lodGroupsBefore.try_emplace(group, drawn.lodGroups[group]);
    }
    drawn.lodGroups.edit(group) = value;
}

auto ViewScene::freeLodGroup(uint32_t group) -> void {
    setLodGroup(group, RenderLodGroup{});
    if (auto it = lodGroupByPath.find(lodGroupInfo[group].path); it != lodGroupByPath.end() && it->second == group) {
        lodGroupByPath.erase(it);
    }
    lodGroupInfo[group] = {};
    if (!lodGroupPaths[group].empty()) {
        lodGroupPaths.edit(group).clear();
    }
    freeLodGroups.insert(group);
}

auto ViewScene::forcedLevelOf(const LodGroupInfo& info) const -> int32_t {
    if (info.levelCount == 0) {
        return -1;
    }
    if (auto hand = handLevels.find(info.path); hand != handLevels.end()) {
        return (int32_t) std::min(hand->second, info.levelCount - 1);
    }
    if (!heuristicOn || !info.usable) {
        return (int32_t) info.defaultLevel;
    }
    return -1;
}

auto ViewScene::refreshForcedLevels(const std::string& path) -> void {
    auto refresh = [this](uint32_t group) {
        const auto& current = drawn.lodGroups[group];
        if (!current.alive) {
            return;
        }
        auto forced = forcedLevelOf(lodGroupInfo[group]);
        if (forced != current.forcedLevel) {
            auto changed = current;
            changed.forcedLevel = forced;
            setLodGroup(group, changed);
        }
    };
    if (!path.empty()) {
        if (auto it = lodGroupByPath.find(path); it != lodGroupByPath.end()) {
            refresh(it->second);
        }
        return;
    }
    for (uint32_t group = 0; group < drawn.lodGroups.size(); group++) {
        refresh(group);
    }
}

auto ViewScene::touched(const std::string& path) const -> bool {
    if (std::ranges::any_of(overridesMoved, [&](const std::string& moved) { return isUnder(moved, path); })) {
        return true;
    }
    if (rootsToCheck.empty()) {
        return false;
    }
    if (path == "/" || rootsToCheck.contains(path)) {
        return true;
    }
    auto prefix = path + "/";
    auto it = rootsToCheck.lower_bound(prefix);
    return it != rootsToCheck.end() && it->starts_with(prefix);
}

auto ViewScene::wantMaterial(const std::string& key, std::vector<std::string>& waiting) -> void {
    if (key.empty()) {
        return;
    }
    auto want = [&](const std::string& assetId, AssetKind kind) {
        if (failedAssets.contains(assetId) || std::ranges::contains(waiting, assetId)) {
            return;
        }
        waiting.push_back(assetId);
        passWanted.emplace_back(assetId, kind);
    };
    auto resolved = resolveMaterial(key);
    if (resolved.pack == nullptr) {
        want(resolved.packId, AssetKind::Material);
        return;
    }
    for (const auto& texture : {resolved.baseColorTexture, resolved.opacityTexture}) {
        if (!texture.empty() && !texturePacks.contains(texture)) {
            want(texture, AssetKind::Texture);
        }
    }
}

auto ViewScene::resolveDraw(const std::string& meshAsset, const std::string& boundMaterial, bool strongBinding, const Entity* subsetsOf, const WorldContent& content)
    -> std::shared_ptr<const ResolvedDraw> {
    // Prims of the same mesh bound to the same material share what they resolve to; a mesh with subset bindings resolves per prim.
    std::string key;
    if (subsetsOf == nullptr) {
        key = std::format("{}\n{}\n{}", meshAsset, boundMaterial, strongBinding ? 1 : 0);
        if (auto it = resolvedDraws.find(key); it != resolvedDraws.end()) {
            return it->second;
        }
    }
    auto out = std::make_shared<ResolvedDraw>();
    const auto& meshPack = meshPacks.at(meshAsset);
    std::optional<std::array<float, 3>> displayColor;
    if (meshPack.hasDisplayColor) {
        displayColor = meshPack.displayColor;
    }
    auto uses = std::make_shared<std::vector<std::string>>(std::vector<std::string>{meshAsset, meshAsset.substr(0, meshAsset.find('#'))});
    auto useMaterial = [&](const std::string& materialAsset) {
        if (materialAsset.empty()) {
            return;
        }
        auto resolved = resolveMaterial(materialAsset);
        uses->push_back(resolved.packId);
        for (const auto& texture : {resolved.baseColorTexture, resolved.opacityTexture}) {
            if (!texture.empty()) {
                uses->push_back(texture);
            }
        }
    };
    for (const auto& submesh : meshPack.submeshes) {
        if (submesh.subsetPath.empty() || subsetsOf == nullptr) {
            out->materials.push_back(material(boundMaterial, submesh.subsetPath.empty() ? displayColor : std::nullopt));
            useMaterial(boundMaterial);
        } else {
            // The subset's own binding, unless an ancestor's is stronger; a subset has no display colour of its own.
            auto name = submesh.subsetPath.substr(submesh.subsetPath.rfind('/') + 1);
            std::string subsetMaterial = boundMaterial;
            for (EntityId child : subsetsOf->children) {
                const auto& subset = content.entities[child];
                if (subset.name == name && subset.binding && !strongBinding) {
                    subsetMaterial = subset.binding->materialAsset;
                }
            }
            out->materials.push_back(material(subsetMaterial, std::nullopt));
            useMaterial(subsetMaterial);
        }
    }
    for (auto handle : out->materials) {
        const auto* materialData = materialLibrary.get(handle);
        bool cuts = materialData != nullptr && materialData->cutout();
        out->cutout.push_back(cuts);
        out->primHasCutout = out->primHasCutout || cuts;
    }
    std::ranges::sort(*uses);
    auto [last, end] = std::ranges::unique(*uses);
    uses->erase(last, end);
    out->uses = std::move(uses);
    if (!key.empty()) {
        resolvedDraws[key] = out;
    }
    return out;
}

namespace {

// What a placement and everything below it waits for: its waiting assets, and its pending roots with their levels' counts.
auto pendingOf(const Placement& node) -> uint32_t {
    auto pending = (uint32_t) node.waiting.size();
    for (const auto& child : node.children) {
        pending += child->pending;
    }
    for (const auto& root : node.roots) {
        if (root.pending) {
            pending++;
        }
        for (const auto& level : root.levels) {
            if (level) {
                pending += level->pending;
            }
        }
    }
    return pending;
}

} // namespace

auto ViewScene::syncPlacement(const std::shared_ptr<Placement>& node, const PlacementSpec& spec, bool mine) -> std::shared_ptr<Placement> {
    // The scene thread's alone when no published tree holds it: then it changes in place, else as a copy.
    mine = mine && node && node.use_count() == 1;
    const auto& w = composer->world();
    auto version = contentOf(w, spec.kind == Placement::Kind::Instancer ? spec.contentKind : spec.kind, spec.content).version;
    bool rebuild = !node;
    if (!rebuild) {
        const auto& n = *node;
        rebuild = n.kind != spec.kind || n.content != spec.content || n.contentKind != spec.contentKind || n.instancer != spec.instancer || n.version != version || n.path != spec.path ||
                  n.world != spec.world || n.purpose != spec.purpose || n.material != spec.material || n.strong != spec.strong || n.lodTag != spec.lodTag || n.drawn != spec.drawn ||
                  n.lodGroup != spec.lodGroup || n.lodGroupLevel != spec.lodGroupLevel;
    }
    if (!rebuild) {
        rebuild = std::ranges::any_of(overridesMoved, [&](const std::string& moved) { return isUnder(moved, spec.path); });
    }
    if (!rebuild) {
        rebuild = std::ranges::any_of(node->waiting, [this](const std::string& asset) { return arrived.contains(asset); });
    }
    if (rebuild) {
        return buildPlacement(node, spec, version, mine);
    }
    std::shared_ptr<Placement> out;
    auto edit = [&]() -> Placement& {
        passEdited = true;
        if (!out) {
            out = mine ? node : std::make_shared<Placement>(*node);
        }
        return *out;
    };
    // A child that waits for nothing, with no root changed and no gizmo move under it, stays as it is.
    for (size_t i = 0; i < node->children.size(); i++) {
        const auto& child = node->children[i];
        if (!fullPass && child->pending == 0 && !touched(child->path)) {
            continue;
        }
        PlacementSpec childSpec{.kind = child->kind,
                                .content = child->content,
                                .contentKind = child->contentKind,
                                .instancer = child->instancer,
                                .path = child->path,
                                .world = child->world,
                                .purpose = child->purpose,
                                .material = child->material,
                                .strong = child->strong,
                                .lodTag = child->lodTag,
                                .drawn = child->drawn,
                                .lodGroup = child->lodGroup,
                                .lodGroupLevel = child->lodGroupLevel};
        auto next = syncPlacement(child, childSpec, mine);
        if (next != child) {
            edit().children[i] = next;
        }
    }
    syncRoots(*node, edit, mine, false);
    const auto& now = out ? *out : *node;
    auto pending = pendingOf(now);
    if (pending != now.pending) {
        edit().pending = pending;
    }
    return out ? out : node;
}

auto ViewScene::buildPlacement(const std::shared_ptr<Placement>& old, const PlacementSpec& spec, uint64_t version, bool mine) -> std::shared_ptr<Placement> {
    passBuilt++;
    const auto& w = composer->world();
    // Its draws are placed again in the slots and prim indices it had, in order, so a draw that didn't change changes nothing for the
    // renderer; what it has left over is freed, what it lacks is taken from the free lists.
    std::vector<uint32_t> oldSlots;
    std::vector<uint32_t> oldPrims;
    std::vector<uint32_t> oldGroups;
    std::vector<std::shared_ptr<Placement>> previousChildren;
    std::vector<PlacedRoot> previousRoots;
    bool wasDrawn = old && old->drawn;
    if (old) {
        oldSlots = old->slots;
        oldPrims = old->prims;
        oldGroups = old->lodGroups;
        useNode(*old, -1);
    }
    std::shared_ptr<Placement> node;
    if (old && mine) {
        node = old;
        previousChildren = std::move(node->children);
        previousRoots = std::move(node->roots);
    } else {
        node = std::make_shared<Placement>();
        if (old) {
            previousChildren = old->children;
            previousRoots = old->roots;
        }
    }
    node->kind = spec.kind;
    node->content = spec.content;
    node->contentKind = spec.contentKind;
    node->instancer = spec.instancer;
    node->version = version;
    node->path = spec.path;
    node->world = spec.world;
    node->purpose = spec.purpose;
    node->material = spec.material;
    node->strong = spec.strong;
    node->lodTag = spec.lodTag;
    node->drawn = spec.drawn;
    node->lodGroup = spec.lodGroup;
    node->lodGroupLevel = spec.lodGroupLevel;
    node->lodGroups.clear();
    node->segments.clear();
    node->slots.clear();
    node->prims.clear();
    node->lights.clear();
    node->waiting.clear();
    node->children.clear();
    node->roots.clear();
    node->cameras.clear();
    node->payloads.clear();
    node->variantPrims.clear();
    node->instancerRow.reset();
    size_t slotCursor = 0;
    size_t primCursor = 0;
    size_t groupCursor = 0;
    auto takeGroup = [&]() -> uint32_t {
        if (groupCursor < oldGroups.size()) {
            return oldGroups[groupCursor++];
        }
        return allocLodGroup();
    };
    auto takeSlot = [&]() -> uint32_t {
        if (slotCursor < oldSlots.size()) {
            return oldSlots[slotCursor++];
        }
        return allocSlot();
    };
    auto takePrim = [&](const std::string& path) -> uint32_t {
        if (primCursor < oldPrims.size()) {
            auto prim = oldPrims[primCursor++];
            if (primPaths[prim - 1] != path) {
                primPaths.edit(prim - 1) = path;
            }
            if (lightsByPrim.erase(prim) > 0) {
                lightsChanged = true;
            }
            return prim;
        }
        return allocPrim(path);
    };

    auto contentKind = placedContentKind(*node);
    const auto& content = contentOf(w, contentKind, spec.content);
    if (spec.kind == Placement::Kind::Instancer) {
        // A segment per shown point: its prototype's walk, at a path of its own (the prototype's, then "#" and the point's id).
        const auto& data = content.pointInstancers.at(spec.instancer);
        std::vector<std::shared_ptr<const ContentWalk>> prototypeWalks;
        std::vector<std::string> prototypePaths;
        auto instancerInContent = content.path(spec.instancer);
        for (EntityId prototype : data.prototypes) {
            if (prototype == noEntity) {
                prototypeWalks.push_back(nullptr);
                prototypePaths.emplace_back();
                continue;
            }
            auto inContent = content.path(prototype);
            prototypePaths.push_back(spec.path + inContent.substr(std::min(inContent.size(), instancerInContent.size())));
            prototypeWalks.push_back(walkOf(contentKind, spec.content, content, spec.purpose, spec.material, spec.strong, prototypePaths.back(), prototype));
        }
        for (size_t point = 0; point < data.protoIndices.size(); point++) {
            auto which = data.protoIndices[point];
            if (data.visible[point] == 0 || which >= prototypeWalks.size() || !prototypeWalks[which]) {
                continue;
            }
            auto id = data.ids.empty() ? (int64_t) point : data.ids[point];
            node->segments.push_back({.walk = prototypeWalks[which], .path = std::format("{}#{}", prototypePaths[which], id), .world = spec.world * glm::mat4(data.transforms[point])});
        }
        if (spec.drawn) {
            node->instancerRow = SceneInstancer{.path = spec.path, .points = (uint32_t) data.protoIndices.size(), .visiblePoints = (uint32_t) node->segments.size(), .prototypes = prototypePaths};
        }
    } else {
        node->segments.push_back({.walk = walkOf(spec.kind, spec.content, content, spec.purpose, spec.material, spec.strong, spec.path), .path = spec.path, .world = spec.world});
    }

    std::unordered_map<std::string, size_t> previousByPath;
    for (size_t i = 0; i < previousChildren.size(); i++) {
        previousByPath[previousChildren[i]->path] = i;
    }
    std::vector<bool> kept(previousChildren.size(), false);
    auto adopt = [&](const PlacementSpec& childSpec) {
        std::shared_ptr<Placement> previous;
        if (auto it = previousByPath.find(childSpec.path); it != previousByPath.end() && !kept[it->second]) {
            kept[it->second] = true;
            previous = std::move(previousChildren[it->second]);
        }
        node->children.push_back(syncPlacement(previous, childSpec, mine));
    };
    auto want = [&](const std::string& assetId, AssetKind kind) {
        if (failedAssets.contains(assetId) || std::ranges::contains(node->waiting, assetId)) {
            return;
        }
        node->waiting.push_back(assetId);
        passWanted.emplace_back(assetId, kind);
    };

    for (uint32_t s = 0; s < node->segments.size(); s++) {
        const auto& segment = node->segments[s];
        const auto& items = segment.walk->items;
        // The group slot + 1 of each render-LOD root item in this segment, as its item is reached (before its levels' items).
        std::unordered_map<int32_t, uint32_t> groupOfItem;
        if (spec.drawn) {
            for (auto index : segment.walk->rowItems) {
                const auto& item = items[index];
                auto world = unitScale * (item.absolute ? item.world : segment.world * item.world);
                auto path = joinPathOf(segment.path, item.path);
                if (item.camera) {
                    node->cameras.push_back({.path = path, .worldTransform = world});
                }
                if (item.variants) {
                    node->variantPrims.push_back({.path = path, .selections = item.entity->variantSelections});
                }
                if (!item.payload) {
                } else if (auto payload = content.payloads.find(item.id); payload != content.payloads.end()) {
                    ScenePayload placed = {.path = path, .assetIds = payload->second.assetIds, .loaded = payload->second.loaded};
                    if (const auto& hint = payload->second.extentsHint) {
                        placed.worldBounds = AABB{.min = (*hint)[0], .max = (*hint)[1]}.transformed(world);
                    }
                    node->payloads.push_back(std::move(placed));
                }
            }
        }
        for (uint32_t i = 0; i < items.size(); i++) {
            const auto& item = items[i];
            auto world = item.absolute ? item.world : segment.world * item.world;
            auto path = joinPathOf(segment.path, item.path);
            // The render-LOD group and level its draws are tagged with: its root's in this segment, else the one this placement is under.
            uint32_t tagGroup = spec.lodGroup;
            uint32_t tagLevel = spec.lodGroupLevel;
            if (item.lodRootItem >= 0) {
                auto group = groupOfItem.find(item.lodRootItem);
                tagGroup = group != groupOfItem.end() ? group->second : 0;
                tagLevel = item.lodLevel;
                if (tagLevel >= RenderLodGroup::maxLevels) {
                    // A level past the group's: not drawn (the composer says so).
                    continue;
                }
            }
            if (item.kind == WalkItem::Kind::Instance) {
                adopt({.kind = Placement::Kind::Prototype,
                       .content = item.content,
                       .contentKind = Placement::Kind::Prototype,
                       .path = path,
                       .world = world,
                       .purpose = item.purpose,
                       .material = item.material,
                       .strong = item.strongBinding,
                       .lodTag = spec.lodTag,
                       .drawn = spec.drawn,
                       .lodGroup = tagGroup,
                       .lodGroupLevel = tagLevel});
                continue;
            }
            if (item.kind == WalkItem::Kind::PointInstancer) {
                adopt({.kind = Placement::Kind::Instancer,
                       .content = spec.content,
                       .contentKind = contentKind,
                       .instancer = item.id,
                       .path = path,
                       .world = world,
                       .purpose = item.purpose,
                       .material = item.material,
                       .strong = item.strongBinding,
                       .lodTag = spec.lodTag,
                       .drawn = spec.drawn,
                       .lodGroup = tagGroup,
                       .lodGroupLevel = tagLevel});
                continue;
            }
            if (item.renderLodRoot && spec.drawn) {
                // A group where the root is placed: its heuristic's extent's bounding sphere (as the view's screenSize measures it), its
                // thresholds, default and forced level.
                const auto& lod = content.lodRoots.at(item.id);
                LodGroupInfo info{
                    .path = path,
                    .defaultLevel = 0,
                    .levelCount = (uint32_t) std::min<size_t>(lod.levels.size(), RenderLodGroup::maxLevels),
                    .usable = lod.heuristicChosen && !lod.thresholds.empty(),
                };
                if (info.levelCount > 0) {
                    info.defaultLevel = std::min(lod.defaultIndex, info.levelCount - 1);
                    auto placed = unitScale * world;
                    auto worldMin = glm::vec3(placed * glm::vec4(lod.extent[0], 1.0f));
                    auto worldMax = glm::vec3(placed * glm::vec4(lod.extent[1], 1.0f));
                    RenderLodGroup group{
                        .center = glm::vec3(placed * glm::vec4((lod.extent[0] + lod.extent[1]) * 0.5f, 1.0f)),
                        .radius = glm::length(worldMax - worldMin) * 0.5f,
                        .levelCount = info.levelCount,
                        .defaultLevel = info.defaultLevel,
                        .forcedLevel = forcedLevelOf(info),
                        .alive = true,
                    };
                    for (size_t t = 0; t < group.thresholds.size() && t < lod.thresholds.size() && t + 1 < info.levelCount; t++) {
                        group.thresholds[t] = lod.thresholds[t];
                    }
                    auto slot = takeGroup();
                    node->lodGroups.push_back(slot);
                    if (auto stale = lodGroupByPath.find(lodGroupInfo[slot].path); stale != lodGroupByPath.end() && stale->second == slot && lodGroupInfo[slot].path != path) {
                        lodGroupByPath.erase(stale);
                    }
                    lodGroupInfo[slot] = info;
                    lodGroupByPath[path] = slot;
                    if (lodGroupPaths[slot] != path) {
                        lodGroupPaths.edit(slot) = path;
                    }
                    if (!(drawn.lodGroups[slot] == group)) {
                        setLodGroup(slot, group);
                    }
                    groupOfItem[(int32_t) i] = slot + 1;
                }
            }
            if (item.kind == WalkItem::Kind::Level) {
                continue;
            }
            if (item.lodRoot) {
                const auto& lod = content.lodRoots.at(item.id);
                PlacedRoot root{.segment = s, .item = i, .path = path};
                // A LOD root in a point's prototype ("<prototype>#<id>/…") draws its default level and isn't the heuristic's to choose.
                root.listed = path.find('#') == std::string::npos;
                root.levels.resize(lod.levels.size());
                auto& row = root.row;
                row.path = path;
                for (EntityId level : lod.levels) {
                    row.levels.push_back(content.entities[level].name);
                    auto payload = content.payloads.find(level);
                    row.levelPayloads.push_back(payload != content.payloads.end() ? payload->second.assetIds : std::vector<std::string>{});
                }
                row.heuristicType = lod.heuristicType;
                row.heuristicChosen = lod.heuristicChosen;
                row.thresholds = lod.thresholds;
                row.extent = lod.extent;
                row.projectedExtent = lod.projection == LodProjection::Extent;
                row.defaultIndex = lod.defaultIndex;
                row.worldTransform = unitScale * world;
                auto previous = std::ranges::find(previousRoots, path, &PlacedRoot::path);
                if (previous != previousRoots.end()) {
                    root.drawn = previous->drawn;
                    root.pending = previous->pending;
                    for (size_t level = 0; level < root.levels.size() && level < previous->levels.size(); level++) {
                        root.levels[level] = std::move(previous->levels[level]);
                    }
                }
                if (spec.drawn && root.listed && (!wasDrawn || previous == previousRoots.end())) {
                    passNewRoots = true;
                }
                node->roots.push_back(std::move(root));
            }
            if (item.light && spec.drawn) {
                const auto& light = content.lights.at(item.id);
                if (light.shape == LightShape::Distant) {
                    auto prim = takePrim(path);
                    node->prims.push_back(prim);
                    if (assetsByPrim[prim - 1]) {
                        assetsByPrim.edit(prim - 1).reset();
                    }
                    RenderLight placed{
                        .type = LightType::Directional,
                        .color = light.color,
                        .intensity = light.intensity,
                        .exposure = light.exposure,
                        .angle = light.angle,
                        .shadowEnable = light.shadowEnable,
                        .shadowColor = light.shadowColor,
                        .worldTransform = unitScale * world,
                        .primHandle = PrimHandle{prim},
                    };
                    node->lights.push_back(placed);
                    lightsByPrim[prim] = placed;
                    lightsChanged = true;
                }
            }
            const auto& entity = *item.entity;
            if (entity.meshAsset.empty() || !drawsPurpose(item.purpose)) {
                continue;
            }
            // What it draws with: the mesh, its binding's material and the subsets' own, each asked for while it isn't held.
            wantMaterial(item.material, node->waiting);
            if (item.subsets && !item.strongBinding) {
                for (EntityId child : entity.children) {
                    const auto& subset = content.entities[child];
                    if (subset.binding) {
                        wantMaterial(subset.binding->materialAsset, node->waiting);
                    }
                }
            }
            if (!meshPacks.contains(entity.meshAsset)) {
                want(entity.meshAsset, AssetKind::Mesh);
                continue;
            }
            if (!spec.drawn) {
                continue;
            }
            auto meshHandle = mesh(entity.meshAsset);
            if (!meshHandle) {
                continue;
            }
            auto resolved = resolveDraw(entity.meshAsset, item.material, item.strongBinding, item.subsets ? &entity : nullptr, content);
            const auto* meshData = meshLibrary.get(meshHandle);
            auto worldTransform = unitScale * world;
            auto worldBounds = meshBoundsOf[meshHandle.index].transformed(worldTransform);
            auto prim = takePrim(path);
            node->prims.push_back(prim);
            if (assetsByPrim[prim - 1] != resolved->uses) {
                assetsByPrim.edit(prim - 1) = resolved->uses;
            }
            for (size_t sub = 0; sub < meshData->submeshes.size(); sub++) {
                const auto& submesh = meshData->submeshes[sub];
                auto slot = takeSlot();
                RenderMeshInstance instance{
                    .mesh = meshHandle,
                    .material = resolved->materials[sub],
                    .prim = prim,
                    .worldTransform = worldTransform,
                    .worldBounds = worldBounds,
                    .indexOffset = submesh.indexOffset,
                    .indexCount = submesh.indexCount,
                    .primFirst = (sub == 0),
                    .doubleSided = meshData->doubleSided,
                    .cutout = resolved->cutout[sub],
                    .primHasCutout = resolved->primHasCutout,
                    .lodLevel = tagGroup != 0 ? tagLevel + 1 : spec.lodTag,
                    .lodGroup = tagGroup,
                };
                if (!(drawn.meshInstances[slot] == instance)) {
                    setSlot(slot, instance);
                }
                node->slots.push_back(slot);
            }
        }
    }
    for (; slotCursor < oldSlots.size(); slotCursor++) {
        freeSlot(oldSlots[slotCursor]);
    }
    for (; primCursor < oldPrims.size(); primCursor++) {
        freePrim(oldPrims[primCursor]);
    }
    for (; groupCursor < oldGroups.size(); groupCursor++) {
        freeLodGroup(oldGroups[groupCursor]);
    }
    for (size_t i = 0; i < previousChildren.size(); i++) {
        if (!kept[i] && previousChildren[i]) {
            releasePlacement(*previousChildren[i]);
        }
    }
    for (const auto& previous : previousRoots) {
        if (std::ranges::find(node->roots, previous.path, &PlacedRoot::path) != node->roots.end()) {
            continue;
        }
        for (const auto& level : previous.levels) {
            if (level) {
                releasePlacement(*level);
            }
        }
    }
    useNode(*node, 1);
    auto edit = [&]() -> Placement& {
        return *node;
    };
    syncRoots(*node, edit, mine, true);
    node->pending = pendingOf(*node);
    return node;
}

auto ViewScene::syncRoots(const Placement& node, const std::function<Placement&()>& edit, bool mine, bool all) -> void {
    const auto& w = composer->world();
    const auto& content = contentOf(w, placedContentKind(node), node.content);
    for (size_t r = 0; r < node.roots.size(); r++) {
        const auto& root = node.roots[r];
        // A settled root (drawing the level it wants) with no change to its wanted level and nothing below it waiting stays as it is.
        bool levelsPending = std::ranges::any_of(root.levels, [](const std::shared_ptr<Placement>& level) { return level && level->pending > 0; });
        if (!all && !fullPass && !root.pending && !levelsPending && !touched(root.path)) {
            continue;
        }
        const auto& segment = node.segments[root.segment];
        const auto& rootItem = segment.walk->items[root.item];
        const auto& lod = content.lodRoots.at(rootItem.id);
        auto rootWorld = rootItem.absolute ? rootItem.world : segment.world * rootItem.world;
        auto wanted = w.wantedLevel(root.path, lod);
        auto drawnLevel = drawnLevelOf(w, root.path, lod, content, rootItem.purpose);
        if (drawnLevel != root.drawn) {
            if (node.drawn && root.drawn && drawnLevel) {
                if (*drawnLevel < *root.drawn) {
                    passSwitchedFiner++;
                } else {
                    passSwitchedCoarser++;
                }
                if (passFirstSwitched.empty()) {
                    passFirstSwitched = std::format("{} {} -> {}", root.path, *root.drawn, *drawnLevel);
                }
            }
            edit().roots[r].drawn = drawnLevel;
        }
        // Pending while it doesn't draw the level it wants, or a level it loads isn't linked yet (still composing): a later pass places it.
        bool pending = !drawnLevel || *drawnLevel != wanted;
        for (auto level : lod.loaded) {
            if (level < lod.levels.size() && content.entities[lod.levels[level]].linkedContent < 0) {
                pending = true;
            }
        }
        if (pending != root.pending) {
            edit().roots[r].pending = pending;
        }
        if (root.listed) {
            // The row's state: what it wants, draws and holds, set by hand or not, and while it wants a loaded level it doesn't draw yet, what
            // that level waits for.
            std::optional<SceneLodRoot::Waiting> waiting;
            if (drawnLevel && *drawnLevel != wanted && std::ranges::contains(lod.loaded, wanted) && wanted < lod.levels.size()) {
                auto linked = content.entities[lod.levels[wanted]].linkedContent;
                SceneLodRoot::Waiting counted;
                if (linked >= 0) {
                    std::set<int64_t> seenContents;
                    std::set<std::string> seenMeshes;
                    countWaiting(w, Placement::Kind::Level, linked, rootItem.purpose, seenContents, seenMeshes, counted);
                } else {
                    // The wanted level is still composing: it is linked once complete.
                    counted.contentsIncomplete = 1;
                }
                waiting = counted;
            }
            const auto& row = node.roots[r].row;
            bool sameWaiting = row.waiting.has_value() == waiting.has_value();
            if (sameWaiting && waiting) {
                sameWaiting = row.waiting->contentsIncomplete == waiting->contentsIncomplete && row.waiting->meshesMissing == waiting->meshesMissing &&
                              row.waiting->meshesInFlight == waiting->meshesInFlight;
            }
            bool setByHand = handLevels.contains(root.path);
            if (row.wanted != wanted || row.drawn != drawnLevel || row.loaded != lod.loaded || row.setByHand != setByHand || !sameWaiting) {
                auto& changed = edit().roots[r].row;
                changed.wanted = wanted;
                changed.drawn = drawnLevel;
                changed.loaded = lod.loaded;
                changed.setByHand = setByHand;
                changed.waiting = waiting;
            }
        }
        for (uint32_t i = 0; i < lod.levels.size(); i++) {
            auto standIn = lod.levels[i];
            auto linked = content.entities[standIn].linkedContent;
            bool loaded = linked >= 0 && std::ranges::contains(lod.loaded, i);
            // Read through the node, which edit() may replace with a copy holding the same pointers.
            const auto& current = node.roots[r].levels[i];
            if (!loaded) {
                if (current) {
                    releasePlacement(*current);
                    edit().roots[r].levels[i] = nullptr;
                }
                continue;
            }
            PlacementSpec spec{.kind = Placement::Kind::Level,
                               .content = linked,
                               .contentKind = Placement::Kind::Level,
                               .path = childPathOf(root.path, content.entities[standIn].name),
                               .world = rootWorld,
                               .purpose = rootItem.purpose,
                               .material = rootItem.material,
                               .strong = rootItem.strongBinding,
                               .lodTag = i + 1,
                               .drawn = node.drawn && drawnLevel == i};
            auto next = syncPlacement(current, spec, mine);
            if (next != current) {
                edit().roots[r].levels[i] = next;
            }
        }
    }
}

// One placement pass: the stage placed as the World is now, looking only where something changed; then the slots it touched as changes for
// the renderer, the library entries the slots use, the lights, and the assets the placements want and lack asked for.
auto ViewScene::place() -> void {
    PROFILE_ZONE("Place");
    placeNeeded = false;
    auto start = std::chrono::steady_clock::now();
    readiness.clear();
    materialByAsset.clear();
    resolvedDraws.clear();
    passWanted.clear();
    passBuilt = 0;
    passEdited = false;
    passNewRoots = false;
    passSwitchedFiner = 0;
    passSwitchedCoarser = 0;
    passFirstSwitched.clear();
    const auto& w = composer->world();
    auto scale = glm::mat4(1.0f);
    if (w.metersPerUnit != 1.0) {
        scale = glm::scale(glm::mat4(1.0f), glm::vec3((float) w.metersPerUnit));
    }
    if (scale != unitScale) {
        unitScale = scale;
        resetPlacements = true;
    }
    if (resetPlacements) {
        resetPlacements = false;
        if (tree) {
            releasePlacement(*tree);
        }
        tree.reset();
    }
    // A placed content composed again (repacked, or let go and composed again) changes where it is placed: every placement is looked at.
    fullPass = std::ranges::any_of(placedVersions, [&](const auto& entry) {
        auto kind = (Placement::Kind) (entry.first >> 32);
        auto index = (int32_t) (uint32_t) entry.first;
        if (index < 0 || (size_t) index >= w.contents.size()) {
            return true;
        }
        return contentOf(w, kind, index).version != entry.second;
    });
    PlacementSpec stage{.kind = Placement::Kind::Stage, .content = -1, .contentKind = Placement::Kind::Stage, .path = "/"};
    std::shared_ptr<Placement> next;
    {
        PROFILE_ZONE("PlaceSync");
        next = syncPlacement(tree, stage, true);
    }
    bool changed = next != tree || passBuilt > 0 || passEdited || !slotsBefore.empty() || drawn.meshInstances.size() != slotCountBefore || lightsChanged ||
                   !lodGroupsBefore.empty() || drawn.lodGroups.size() != lodGroupCountBefore;
    tree = std::move(next);
    overridesMoved.clear();
    rootsToCheck.clear();
    arrived.clear();
    requestAssets(passWanted, false);
    passWanted.clear();
    stats.places++;
    evaluateNeeded = evaluateNeeded || passNewRoots;
    if (!changed && !libraryNeeded) {
        return;
    }
    libraryNeeded = false;

    PROFILE_ZONE("PlaceChanges");
    SceneChanges changes;
    if (changed) {
        // Kept walks of contents composed again since are dropped.
        std::erase_if(walks, [&](const auto& entry) {
            const auto& key = entry.first;
            auto index = std::atoi(key.c_str() + 1);
            const WorldContent* content = nullptr;
            if (key[0] == 'S') {
                content = &w.stage;
            } else if (index < (int) w.contents.size()) {
                content = &w.contents[(size_t) index];
            }
            return content == nullptr || content->version != entry.second->version;
        });
        // The engine's light follows the scene's up axis, known once the stage composed: the lights are gathered on every change.
        lightsChanged = false;
        auto previousLights = drawn.lights;
        collectLights();
        if (drawn.lights != previousLights) {
            changes.lightsChanged = true;
            changes.lights = drawn.lights;
        }
    }
    // Library entries first, so the draws below name only what the renderer has been told about.
    std::set<uint32_t> meshesNow;
    std::set<uint32_t> materialsNow;
    std::set<uint32_t> texturesNow;
    for (const auto& [index, count] : meshUses) {
        meshesNow.insert(index);
    }
    for (const auto& [index, count] : materialUses) {
        materialsNow.insert(index);
        if (const auto* material = materialLibrary.get(MaterialHandle{index})) {
            for (auto texture : {material->baseColorTexture, material->opacityTexture}) {
                if (texture) {
                    texturesNow.insert(texture.index);
                }
            }
        }
    }
    collectChanges(meshesNow, materialsNow, texturesNow, changes);
    // A pack no placement names and no library entry holds any more (its level unloaded after its entry went) is released.
    for (auto pack = meshPacks.begin(); pack != meshPacks.end();) {
        if (meshNames.contains(pack->first) || meshLibrary.find(pack->first)) {
            ++pack;
            continue;
        }
        releaseMeshPack(pack->second);
        pack = meshPacks.erase(pack);
    }

    uint32_t added = 0;
    uint32_t removed = 0;
    uint32_t updated = 0;
    for (const auto& [slot, before] : slotsBefore) {
        const auto& now = drawn.meshInstances[slot];
        if (now == before) {
            continue;
        }
        changes.instances.emplace_back(slot, now);
        if (before.alive && now.alive) {
            updated++;
        } else if (now.alive) {
            added++;
        } else {
            removed++;
        }
    }
    for (auto slot = slotCountBefore; slot < drawn.meshInstances.size(); slot++) {
        changes.instances.emplace_back(slot, drawn.meshInstances[slot]);
        if (drawn.meshInstances[slot].alive) {
            added++;
        }
    }
    slotsBefore.clear();
    slotCountBefore = (uint32_t) drawn.meshInstances.size();
    changes.slotCount = slotCountBefore;
    // The groups touched, against what they held, and the new ones.
    auto groupChanges = changes.lodGroups.size();
    for (const auto& [group, before] : lodGroupsBefore) {
        if (!(drawn.lodGroups[group] == before)) {
            changes.lodGroups.emplace_back(group, drawn.lodGroups[group]);
        }
    }
    for (auto group = lodGroupCountBefore; group < drawn.lodGroups.size(); group++) {
        changes.lodGroups.emplace_back(group, drawn.lodGroups[group]);
    }
    lodGroupsBefore.clear();
    lodGroupCountBefore = (uint32_t) drawn.lodGroups.size();
    changes.lodGroupCount = lodGroupCountBefore;
    auto groupsChanged = (uint32_t) (changes.lodGroups.size() - groupChanges);
    addChanges(std::move(changes));
    if (!changed) {
        return;
    }

    // What the snapshots read: the lists are shared in chunks, so this copies only their chunk pointers.
    auto built = std::make_shared<SceneDraws>();
    built->drawn = drawn;
    built->placements = tree;
    built->primPaths = primPaths;
    built->assetsByPrim = assetsByPrim;
    built->lodGroupPaths = lodGroupPaths;
    draws = std::move(built);
    inputsRevision++;
    if (passSwitchedFiner + passSwitchedCoarser > 0) {
        TRACE_EVENT("Scene", "LevelsSwitched", id)
            .text(std::format("{} roots draw another level: {} finer, {} coarser (first: {})", passSwitchedFiner + passSwitchedCoarser, passSwitchedFiner, passSwitchedCoarser, passFirstSwitched))
            .field("finer", (int64_t) passSwitchedFiner)
            .field("coarser", (int64_t) passSwitchedCoarser);
    }
    auto ms = msSince(start);
    stats.lastPlaceMs = ms;
    stats.placementsBuilt = passBuilt;
    stats.slotsAdded = added;
    stats.slotsRemoved = removed;
    stats.slotsUpdated = updated;
    stats.placements = placementCount;
    stats.placementsDrawn = drawnPlacementCount;
    stats.walksKept = walks.size();
    stats.lodGroups = (uint32_t) (drawn.lodGroups.size() - freeLodGroups.size());
    stats.lodGroupsChanged = groupsChanged;
    TRACE_EVENT("Scene", "PlacementsChanged", id)
        .text(std::format("{} placements built of {} ({} drawn){}: {} slots added, {} removed, {} updated, {} LOD groups changed of {}, in {:.1f} ms",
                          passBuilt,
                          placementCount,
                          drawnPlacementCount,
                          fullPass ? ", every placement looked at" : "",
                          added,
                          removed,
                          updated,
                          groupsChanged,
                          stats.lodGroups,
                          ms))
        .field("lodGroups", (int64_t) stats.lodGroups)
        .field("lodGroupsChanged", (int64_t) groupsChanged)
        .field("built", (int64_t) passBuilt)
        .field("placements", (int64_t) placementCount)
        .field("drawn", (int64_t) drawnPlacementCount)
        .field("full", fullPass)
        .field("added", (int64_t) added)
        .field("removed", (int64_t) removed)
        .field("updated", (int64_t) updated)
        .field("ms", ms);
}

// The distant lights in prim order, or the engine's light when the scene has none: upper-front-right in the scene's own frame, as it
// always was.
auto ViewScene::collectLights() -> void {
    const auto& w = composer->world();
    drawn.lights.clear();
    for (const auto& [prim, light] : lightsByPrim) {
        drawn.lights.push_back(light);
    }
    if (!drawn.lights.empty()) {
        if (defaultLightPrim != 0) {
            freePrim(defaultLightPrim);
            defaultLightPrim = 0;
        }
        return;
    }
    if (defaultLightPrim == 0) {
        defaultLightPrim = allocPrim("/__DefaultEngineLight");
    }
    bool zUp = w.upAxis == "Z";
    auto dir = zUp ? glm::normalize(glm::vec3(1.0f, 1.0f, 2.0f)) : glm::normalize(glm::vec3(1.0f, 2.0f, 1.0f));
    auto sceneUp = zUp ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    auto xAxis = glm::normalize(glm::cross(sceneUp, dir));
    auto yAxis = glm::cross(dir, xAxis);
    // Through double, as the light's transform op once held it.
    glm::dmat4 basis(glm::dvec4(xAxis, 0.0), glm::dvec4(yAxis, 0.0), glm::dvec4(dir, 0.0), glm::dvec4(0.0, 0.0, 0.0, 1.0));
    drawn.lights.push_back({
        .type = LightType::Directional,
        .color = glm::vec3(1.0f, 0.95f, 0.88f),
        .intensity = 1.0f,
        .exposure = 0.0f,
        .angle = 0.53f,
        .shadowEnable = true,
        .shadowColor = glm::vec3(0.0f),
        .worldTransform = unitScale * glm::mat4(basis),
        .primHandle = PrimHandle{defaultLightPrim},
    });
}
