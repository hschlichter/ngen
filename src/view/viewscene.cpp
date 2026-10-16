#include "viewscene.h"

#include "assetclient.h"
#include "packedtexture.h"
#include "profile.h"
#include "scenepack.h"
#include "trace.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace {

auto msSince(std::chrono::steady_clock::time_point start) -> double {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// sRGB-encodes a linear colour channel for an R8G8B8A8_SRGB texel, so sampling decodes it back to the authored colour.
auto encodeSrgb(float c) -> uint8_t {
    c = std::clamp(c, 0.0f, 1.0f);
    float s = (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
    return (uint8_t) std::lround(s * 255.0f);
}

auto childPathOf(const std::string& parent, const std::string& name) -> std::string {
    if (parent == "/") {
        return "/" + name;
    }
    return parent + "/" + name;
}

auto parentPathOf(const std::string& path) -> std::string {
    auto slash = path.rfind('/');
    if (slash == 0 || slash == std::string::npos) {
        return "/";
    }
    return path.substr(0, slash);
}

// A path relative to a content's root ("/" for the root itself), made full at the content's use.
auto joinPathOf(const std::string& base, const std::string& relative) -> std::string {
    if (relative == "/") {
        return base;
    }
    if (base == "/") {
        return relative;
    }
    return base + relative;
}

auto toVertex(const PackVertex& vertex) -> Vertex {
    return {.position = vertex.position, .normal = vertex.normal, .color = vertex.color, .texCoord = vertex.texCoord};
}

// A packed texture as the renderer takes it; null when the bytes aren't one.
auto textureDesc(std::span<const std::byte> bytes) -> std::shared_ptr<const TextureDesc> {
    auto texture = readPackedTexture(bytes);
    if (!texture) {
        return nullptr;
    }
    auto desc = std::make_shared<TextureDesc>();
    desc->width = texture->width;
    desc->height = texture->height;
    desc->mipLevels = texture->mipLevels;
    desc->levels.assign(texture->levels.begin(), texture->levels.end());
    return desc;
}

} // namespace

auto TransformIndex::find(const std::string& path) const -> std::optional<EntityTransform> {
    if (auto moved = overrides.find(path); moved != overrides.end()) {
        return moved->second;
    }
    // The deepest use at or above the path that holds it: a prim is in the content used nearest above it.
    std::string at = path;
    while (true) {
        if (auto use = uses.find(at); use != uses.end()) {
            std::string relative = "/";
            if (at == "/") {
                relative = path;
            } else if (path != at) {
                relative = path.substr(at.size());
            }
            if (auto prim = use->second.prims->find(relative); prim != use->second.prims->end()) {
                auto out = prim->second;
                if (!out.absolute) {
                    out.world = use->second.world * out.world;
                }
                return out;
            }
        }
        if (at == "/") {
            return std::nullopt;
        }
        at = parentPathOf(at);
    }
}

auto decomposeTransform(const glm::mat4& local) -> Transform {
    Transform out;
    out.position = glm::vec3(local[3]);
    out.scale = glm::vec3(glm::length(glm::vec3(local[0])), glm::length(glm::vec3(local[1])), glm::length(glm::vec3(local[2])));
    glm::mat3 rotation(glm::vec3(local[0]) / out.scale.x, glm::vec3(local[1]) / out.scale.y, glm::vec3(local[2]) / out.scale.z);
    out.rotation = glm::quat_cast(rotation);
    return out;
}

ViewScene::ViewScene(AssetClient& client) : assets(client) {
    published = current;
    assets.setAnsweredCallback([this] {
        {
            std::lock_guard lock(mutex);
            answered = true;
        }
        wake.notify_one();
    });
    thread = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

ViewScene::~ViewScene() {
    assets.setAnsweredCallback({});
    thread.request_stop();
    wake.notify_one();
}

auto ViewScene::post(Request request) -> void {
    requestsPosted++;
    {
        std::lock_guard lock(mutex);
        requests.push_back(std::move(request));
    }
    wake.notify_one();
}

auto ViewScene::open(const std::string& sceneId) -> void {
    post(OpenRequest{.id = sceneId});
}

auto ViewScene::setLodLevel(const std::string& rootPath, uint32_t level) -> void {
    post(LodRequest{.root = rootPath, .level = level});
}

auto ViewScene::resetLodLevel(const std::string& rootPath) -> void {
    post(LodRequest{.root = rootPath, .level = std::nullopt});
}

auto ViewScene::setCamera(const glm::vec3& position, float fovY, float aspect) -> void {
    {
        std::lock_guard lock(mutex);
        postedCamera = CameraInput{.position = position, .fovY = fovY, .aspect = aspect};
    }
    wake.notify_one();
}

auto ViewScene::cameraAspect() const -> float {
    std::lock_guard lock(mutex);
    return postedCamera ? postedCamera->aspect : 1.0f;
}

auto ViewScene::setLodHeuristic(bool on) -> void {
    post(HeuristicRequest{.on = on, .unloadDelay = std::nullopt});
}

auto ViewScene::setUnloadDelay(double seconds) -> void {
    post(HeuristicRequest{.on = std::nullopt, .unloadDelay = seconds});
}

auto ViewScene::setPurposeShown(Purpose purpose, bool shown) -> void {
    if (purpose == Purpose::Default || purpose == Purpose::Render) {
        return;
    }
    if (shown) {
        shownPurposes.insert(purpose);
    } else {
        shownPurposes.erase(purpose);
    }
    post(PurposeRequest{.purpose = purpose, .shown = shown});
}

auto ViewScene::purposeShown(Purpose purpose) const -> bool {
    return shownPurposes.contains(purpose);
}

auto ViewScene::refresh(const std::vector<std::string>& ids, bool force) -> void {
    if (ids.empty()) {
        return;
    }
    post(RefreshRequest{.ids = ids, .force = force});
}

auto ViewScene::setLocalTransform(const std::string& path, const glm::mat4& local) -> bool {
    auto found = current->transforms->find(path);
    if (!found || !found->xformable) {
        return false;
    }
    post(TransformRequest{.path = path, .local = local});
    return true;
}

auto ViewScene::update() -> bool {
    std::lock_guard lock(mutex);
    if (published == current) {
        return false;
    }
    current = published;
    return true;
}

auto ViewScene::idle() const -> bool {
    if (requestsApplied.load() != requestsPosted.load()) {
        return false;
    }
    std::lock_guard lock(mutex);
    // The snapshot taken is the latest, and it was published with nothing left to do: no request, answer or camera the scene thread hasn't
    // taken (a camera moved may want other levels), and none it took and is still working on.
    return published == current && current->idle && requests.empty() && !answered && !postedCamera.has_value() && !working;
}

auto ViewScene::takeChanges() -> SceneChanges {
    std::lock_guard lock(mutex);
    auto changes = std::move(pending);
    pending = {};
    pending.slotCount = changes.slotCount;
    return changes;
}

auto ViewScene::addChanges(SceneChanges changes) -> void {
    std::lock_guard lock(mutex);
    pending.append(std::move(changes));
}

// The scene thread: requests in order, then what the asset server answered, then one compose and extract for the batch. Nothing waits
// but the loop itself, for the next request or answer.
auto ViewScene::run(const std::stop_token& stop) -> void {
    profile::registerThread("Scene");
    while (!stop.stop_requested()) {
        std::deque<Request> batch;
        bool answers = false;
        {
            std::unique_lock lock(mutex);
            auto woken = [this] {
                return !requests.empty() || answered || postedCamera.has_value();
            };
            // A held-back extract, or something kept past its unload delay: wake for it at its time, or sooner for a request or an answer.
            std::optional<std::chrono::steady_clock::time_point> due;
            if (extractNeeded) {
                due = extractDue;
            }
            if (releaseDue && (!due || *releaseDue < *due)) {
                due = releaseDue;
            }
            if (due) {
                wake.wait_until(lock, stop, *due, woken);
            } else {
                wake.wait(lock, stop, woken);
            }
            if (stop.stop_requested()) {
                return;
            }
            batch = std::move(requests);
            requests.clear();
            answers = answered;
            answered = false;
            working = true;
            if (postedCamera) {
                camera = postedCamera;
                postedCamera.reset();
                evaluateNeeded = true;
            }
        }
        PROFILE_ZONE("SceneBatch");
        stats.batches++;
        for (auto& request : batch) {
            apply(request);
        }
        if (answers) {
            for (const auto& assetId : assets.takeAnswered()) {
                receive(assetId);
            }
        }
        if (releaseDue && std::chrono::steady_clock::now() >= *releaseDue) {
            // Something kept past its unload delay: an extract lets it go.
            releaseDue.reset();
            extractNeeded = true;
        }
        if (composeNeeded) {
            compose();
        }
        if (requestNeeded) {
            requestNeeded = false;
            requestDrawnAssets();
        }
        if (extractNeeded) {
            // While assets arrive, an extract waits at least as long after the last as the last took (250 ms at least), so the scene
            // thread keeps composing and requesting between them; once nothing is in flight it runs at once.
            bool arriving = !inFlight.empty() || composeNeeded;
            auto pause = std::chrono::milliseconds((int64_t) std::max(250.0, stats.lastExtractMs));
            auto now = std::chrono::steady_clock::now();
            if (!arriving || now >= lastExtractEnd + pause) {
                extract();
                lastExtractEnd = std::chrono::steady_clock::now();
                // Roots reached for the first time are evaluated now.
                evaluateNeeded = true;
            } else {
                extractDue = lastExtractEnd + pause;
                stats.extractsHeld++;
            }
        }
        if (evaluateNeeded) {
            evaluateNeeded = false;
            if (evaluateLevels()) {
                relink();
            }
        }
        unload();
        reportAssets();
        publish();
        requestsApplied += batch.size();
    }
}

auto ViewScene::apply(Request& request) -> void {
    if (auto* open = std::get_if<OpenRequest>(&request)) {
        applyOpen(open->id);
    } else if (auto* lod = std::get_if<LodRequest>(&request)) {
        // A level set by hand holds until reset; the heuristic's evaluation gives the composer what changed.
        if (lod->level) {
            handLevels[lod->root] = *lod->level;
        } else {
            handLevels.erase(lod->root);
        }
        evaluateNeeded = true;
    } else if (auto* heuristic = std::get_if<HeuristicRequest>(&request)) {
        if (heuristic->on) {
            heuristicOn = *heuristic->on;
        }
        if (heuristic->unloadDelay) {
            unloadDelaySeconds = std::max(0.0, *heuristic->unloadDelay);
            extractNeeded = true; // what is kept is weighed against the new delay
        }
        evaluateNeeded = true;
    } else if (auto* purpose = std::get_if<PurposeRequest>(&request)) {
        if (purpose->shown) {
            drawnPurposes.insert(purpose->purpose);
        } else {
            drawnPurposes.erase(purpose->purpose);
        }
        extractNeeded = true;
        requestNeeded = true;
    } else if (auto* refresh = std::get_if<RefreshRequest>(&request)) {
        applyRefresh(refresh->ids, refresh->force);
    } else if (auto* transform = std::get_if<TransformRequest>(&request)) {
        applyTransform(transform->path, transform->local);
    }
}

auto ViewScene::applyOpen(const std::string& sceneId) -> void {
    // The counters are per scene.
    stats = {};
    // Every draw of the scene before goes: its slots die and are reused, lowest first, so a scene opened draws in visit order.
    SceneChanges changes;
    for (auto& [key, slot] : slotOfDraw) {
        drawn.meshInstances[slot] = RenderMeshInstance{.primFirst = false, .alive = false};
        changes.instances.emplace_back(slot, drawn.meshInstances[slot]);
        freeSlots.insert(slot);
    }
    changes.slotCount = (uint32_t) drawn.meshInstances.size();
    addChanges(std::move(changes));
    slotOfDraw.clear();
    id = sceneId;
    composer = std::make_unique<WorldComposer>(sceneId);
    requestedPacks.clear();
    meshPacks.clear();
    materialPacks.clear();
    texturePacks.clear();
    packVersions.clear();
    failedAssets.clear();
    refreshing.clear();
    composeReported = false;
    openedAt = std::chrono::steady_clock::now();
    TRACE_EVENT("Scene", "SceneRequested", sceneId.empty() ? "(empty scene)" : sceneId).text(sceneId.empty() ? std::string("an empty scene") : std::format("scene {} requested", sceneId));
    composeNeeded = !sceneId.empty();
    extractNeeded = true;
}

auto ViewScene::requestAssets(const std::vector<std::pair<std::string, AssetKind>>& wanted, bool force) -> void {
    std::vector<std::string> ids;
    for (const auto& [assetId, kind] : wanted) {
        if (!force && inFlight.contains(assetId)) {
            continue;
        }
        inFlight[assetId] = kind;
        ids.push_back(assetId);
    }
    if (ids.empty()) {
        return;
    }
    assetsReported = false;
    assets.request(ids, force);
}

// A refresh: the ids are fetched again (repacked first when forced); what arrives replaces what the scene holds.
auto ViewScene::applyRefresh(const std::vector<std::string>& ids, bool force) -> void {
    auto kindOf = [this](const std::string& assetId) -> std::optional<AssetKind> {
        if (requestedPacks.contains(assetId)) {
            return AssetKind::Layer;
        }
        if (meshPacks.contains(assetId)) {
            return AssetKind::Mesh;
        }
        if (materialPacks.contains(assetId)) {
            return AssetKind::Material;
        }
        if (texturePacks.contains(assetId)) {
            return AssetKind::Texture;
        }
        if (failedAssets.contains(assetId)) {
            // Failed before: what it is shows when it arrives.
            return AssetKind::Other;
        }
        return std::nullopt;
    };
    std::vector<std::pair<std::string, AssetKind>> wanted;
    size_t sceneAssets = 0;
    for (const auto& assetId : ids) {
        auto kind = kindOf(assetId);
        if (kind) {
            sceneAssets++;
            refreshing.insert(assetId);
            wanted.emplace_back(assetId, *kind);
        } else if (force) {
            // Not the scene's (a shader): repacked on the server; whoever uses it fetches it.
            wanted.emplace_back(assetId, AssetKind::Other);
        }
    }
    requestAssets(wanted, force);
    TRACE_EVENT("Scene", "AssetsRefreshRequested", id)
        .text(std::format("{} assets {} ({} of the scene's)", wanted.size(), force ? "to repack" : "changed at their source, fetched again", sceneAssets))
        .field("forced", force)
        .field("assets", (int64_t) wanted.size())
        .field("scene_assets", (int64_t) sceneAssets);
}

// What the asset server answered for an asset this thread asked for: stored by its kind, or a failure. A refreshed one replaces what the
// scene held; a mesh or texture that arrives unchanged (the same version) keeps its library entry, so nothing is uploaded again.
auto ViewScene::receive(const std::string& assetId) -> void {
    auto it = inFlight.find(assetId);
    if (it == inFlight.end()) {
        return;
    }
    auto kind = it->second;
    inFlight.erase(it);
    stats.assetsReceived++;
    bool refreshed = refreshing.erase(assetId) > 0;
    assetsSinceReport++;
    auto asset = kind == AssetKind::Other && !refreshed ? std::optional<PackedAsset>{} : assets.take(assetId);
    if (kind == AssetKind::Other && !refreshed) {
        return;
    }
    if (!asset) {
        std::string reason;
        for (const auto& line : assets.errors(assetId)) {
            reason += reason.empty() ? line : "; " + line;
        }
        if (refreshed) {
            TRACE_WARNING("Scene", "SceneAssetFailed", assetId).text(std::format("{}: fetching it again failed, the scene keeps what it had: {}", assetId, reason.empty() ? std::string("no reason given") : reason));
            return;
        }
        failedAssets.insert(assetId);
        TRACE_WARNING("Scene", "SceneAssetFailed", assetId).text(std::format("{} failed: {}", assetId, reason.empty() ? std::string("no reason given") : reason));
        if (kind == AssetKind::Layer && composer) {
            composer->markFailed(assetId);
            composeNeeded = true;
        }
        extractNeeded = true;
        return;
    }
    failedAssets.erase(assetId);
    bool unchanged = refreshed && packVersions[assetId] == asset->version;
    packVersions[assetId] = asset->version;
    auto packKind = scenePackKind(asset->bytes);
    if (packKind && *packKind == ScenePackKind::Layer) {
        if (auto error = composer->addPack(assetId, asset->bytes)) {
            TRACE_WARNING("Scene", "SceneAssetFailed", assetId).text(*error);
            composer->markFailed(assetId);
        }
        composeNeeded = true;
        composeReported = false;
        if (refreshed && !unchanged) {
            // The file's meshes and materials, which the file's job packed with it, are fetched again too.
            auto file = assetId + "#";
            std::vector<std::pair<std::string, AssetKind>> again;
            for (const auto& [meshId, pack] : meshPacks) {
                if (meshId.starts_with(file)) {
                    again.emplace_back(meshId, AssetKind::Mesh);
                    refreshing.insert(meshId);
                }
            }
            for (const auto& [materialId, pack] : materialPacks) {
                if (materialId.starts_with(file)) {
                    again.emplace_back(materialId, AssetKind::Material);
                    refreshing.insert(materialId);
                }
            }
            requestAssets(again, false);
        }
    } else if (packKind && *packKind == ScenePackKind::Mesh) {
        if (auto mesh = readMeshPack(asset->bytes)) {
            meshPacks[assetId] = std::move(*mesh);
            if (!unchanged) {
                // New content: a new library entry, so the renderer uploads it.
                meshLibrary.forget(assetId);
            }
        } else {
            failedAssets.insert(assetId);
        }
    } else if (packKind && *packKind == ScenePackKind::Material) {
        if (auto material = readMaterialPack(asset->bytes)) {
            materialPacks[assetId] = std::move(*material);
            requestNeeded = true; // its textures
        } else {
            failedAssets.insert(assetId);
        }
    } else if (auto desc = textureDesc(asset->bytes)) {
        texturePacks[assetId] = std::move(desc);
        if (!unchanged) {
            textureLibrary.forget(assetId);
        }
    } else {
        failedAssets.insert(assetId);
        TRACE_WARNING("Scene", "SceneAssetFailed", assetId).text(std::format("{} isn't a layer, mesh, material or texture pack", assetId));
    }
    extractNeeded = true;
}

// Composes with the layer packs held, gaps where one hasn't arrived, and asks for those it names and lacks.
auto ViewScene::compose() -> void {
    composeNeeded = false;
    extractNeeded = true;
    requestNeeded = true;
    if (!composer) {
        return;
    }
    stats.composes++;
    auto start = std::chrono::steady_clock::now();
    std::vector<std::pair<std::string, AssetKind>> fresh;
    auto needs = composer->compose();
    stats.lastComposeCallMs = msSince(start);
    countWorld();
    const auto& composed = composer->stats();
    stats.composeStageMs = composed.stageMs;
    stats.composeResolveMs = composed.resolveMs;
    stats.composeFinishMs = composed.finishMs;
    stats.prototypesComposed = composed.prototypesComposed;
    stats.levelsComposed = composed.levelsComposed;
    stats.prototypesHeld = composed.prototypesHeld;
    stats.levelsHeld = composed.levelsHeld;
    stats.contentsReached = composed.contentsReached;
    for (const auto& needed : needs) {
        if (requestedPacks.insert(needed).second) {
            fresh.emplace_back(needed, AssetKind::Layer);
        }
    }
    requestAssets(fresh, false);
    bool layersInFlight = std::ranges::any_of(inFlight, [](const auto& entry) { return entry.second == AssetKind::Layer; });
    if (layersInFlight || composeReported) {
        return;
    }
    // Every layer pack it names has been answered: the composition is the scene's.
    composeReported = true;
    for (const auto& message : composer->messages()) {
        TRACE_WARNING("Scene", "CompositionMessage", id).text(message);
    }
    const auto& world = composer->world();
    if (world.layerStack.empty()) {
        TRACE_ERROR("Scene", "SceneFailed", id).text(std::format("scene {}: its root layer can't be had", id));
    }
    size_t entities = 0;
    world.visit([&entities](const VisitedEntity&) {
        entities++;
        return true;
    });
    auto ms = msSince(start);
    stats.lastComposeMs = ms;
    TRACE_EVENT("Scene", "PacksComposed", id)
        .text(std::format("{} composed: {} layer packs, {} prims, {} prototypes, {:.0f} ms after it was requested", id, requestedPacks.size(), entities, world.prototypes.size(), msSince(openedAt)))
        .field("packs", (int64_t) requestedPacks.size())
        .field("prims", (int64_t) entities)
        .field("prototypes", (int64_t) world.prototypes.size())
        .field("ms", ms);
}

// The meshes and materials of everything drawn, and the textures of the materials held; each asked for once.
auto ViewScene::releaseMeshPack(const MeshPack& pack) -> void {
    stats.releasedMeshBytes += pack.vertices.size() * sizeof(PackVertex) + pack.indices.size() * sizeof(uint32_t);
    stats.releasedMeshes++;
}

auto ViewScene::requestDrawnAssets() -> void {
    if (!composer) {
        return;
    }
    readiness.clear();
    // What is drawn, from the kept walks: each drawn mesh, its material and its subsets' own.
    std::set<std::string> meshIds;
    std::set<std::string> materialIds;
    replay(
        composer->world(),
        [&](const WorldContent& content, const ExtractItem& item, const std::string&, const glm::mat4&, uint32_t) {
            const auto& entity = *item.entity;
            if (entity.meshAsset.empty() || !drawsPurpose(item.purpose)) {
                return;
            }
            meshIds.insert(entity.meshAsset);
            if (!item.material.empty()) {
                materialIds.insert(item.material);
            }
            // A subset's own binding: the subset entities are the mesh's children.
            for (EntityId child : entity.children) {
                const auto& subset = content.entities[child];
                if (subset.binding && !subset.binding->materialAsset.empty()) {
                    materialIds.insert(subset.binding->materialAsset);
                }
            }
        },
        nullptr,
        true);
    // A pack no loaded level names and no library entry holds any more (its level unloaded after its entry went) is released here.
    meshesLoaded = meshIds;
    for (auto pack = meshPacks.begin(); pack != meshPacks.end();) {
        if (meshesLoaded.contains(pack->first) || meshLibrary.find(pack->first)) {
            ++pack;
            continue;
        }
        releaseMeshPack(pack->second);
        pack = meshPacks.erase(pack);
    }
    std::vector<std::pair<std::string, AssetKind>> wanted;
    auto want = [&](const std::string& assetId, AssetKind kind, bool held) {
        if (!held && !failedAssets.contains(assetId) && !inFlight.contains(assetId)) {
            wanted.emplace_back(assetId, kind);
        }
    };
    for (const auto& meshId : meshIds) {
        want(meshId, AssetKind::Mesh, meshPacks.contains(meshId));
    }
    for (const auto& materialId : materialIds) {
        auto resolved = resolveMaterial(materialId);
        want(resolved.packId, AssetKind::Material, resolved.pack != nullptr);
        for (const auto& texture : {resolved.baseColorTexture, resolved.opacityTexture}) {
            if (!texture.empty()) {
                want(texture, AssetKind::Texture, texturePacks.contains(texture));
            }
        }
    }
    requestAssets(wanted, false);
}

// Once every asset the drawn scene asked for has been answered, after it composed: SceneAssetsReceived.
auto ViewScene::reportAssets() -> void {
    bool assetsInFlight = std::ranges::any_of(inFlight, [](const auto& entry) { return entry.second != AssetKind::Layer; });
    if (assetsInFlight || assetsReported || !composeReported) {
        return;
    }
    assetsReported = true;
    TRACE_EVENT("Scene", "SceneAssetsReceived", id)
        .text(std::format("{} meshes, {} materials and {} textures drawn; {} answers since the last report, {:.0f} ms after the scene was requested", meshPacks.size(), materialPacks.size(), texturePacks.size(), assetsSinceReport, msSince(openedAt)))
        .field("meshes", (int64_t) meshPacks.size())
        .field("materials", (int64_t) materialPacks.size())
        .field("textures", (int64_t) texturePacks.size())
        .field("received", (int64_t) assetsSinceReport);
    assetsSinceReport = 0;
}

// The prim's new world transform, and the change of it applied to what is drawn under it, so a drag doesn't rebuild the draws: each draw
// instance and light under the prim gets its new world transform and bounds. The prim's own transform is published as an override until
// the next extract, which takes the view's local opinion (localOverrides) by walking what holds it afresh.
auto ViewScene::applyTransform(const std::string& path, const glm::mat4& local) -> void {
    auto found = transforms->find(path);
    if (!found || !found->xformable) {
        return;
    }
    localOverrides[path] = local;
    glm::mat4 parentWorld = found->world * glm::inverse(found->local);
    glm::mat4 newWorld = found->resetsXformStack ? local : parentWorld * local;
    glm::mat4 change = newWorld * glm::inverse(found->world);
    // The index is shared with published snapshots: the moved prim goes into a new one.
    auto moved = std::make_shared<TransformIndex>(*transforms);
    moved->overrides[path] = {.local = local, .world = newWorld, .xformable = true, .resetsXformStack = found->resetsXformStack, .absolute = true};
    transforms = std::move(moved);
    // Instance and light transforms are in metres: the change applies before the unit scale.
    glm::mat4 changeInMetres = unitScale * change * glm::inverse(unitScale);
    auto prefix = path + "/";
    auto isUnder = [&](const std::string& candidate) {
        return candidate == path || candidate.starts_with(prefix);
    };
    SceneChanges changes;
    const auto& paths = draws->primPaths;
    for (const auto& [prim, slots] : drawn.primInstances) {
        if (!isUnder(paths[prim - 1])) {
            continue;
        }
        for (auto i : slots) {
            auto& instance = drawn.meshInstances[i];
            instance.worldTransform = changeInMetres * instance.worldTransform;
            instance.worldBounds = meshBoundsOf[instance.mesh.index].transformed(instance.worldTransform);
            changes.instances.emplace_back(i, instance);
        }
    }
    for (auto& light : drawn.lights) {
        if (isUnder(paths[light.primHandle.index - 1])) {
            light.worldTransform = changeInMetres * light.worldTransform;
            changes.lightsChanged = true;
            changes.lights = drawn.lights;
        }
    }
    changes.slotCount = (uint32_t) drawn.meshInstances.size();
    addChanges(std::move(changes));
    // Published snapshots keep the draws they had: the moved ones go into a copy.
    auto movedDraws = std::make_shared<SceneDraws>(*draws);
    movedDraws->drawn = drawn;
    draws = std::move(movedDraws);
    inputsRevision++;
}

namespace {

// UsdLod's projectedSphere screen size (UsdLodScreenSizeHeuristicQuery), without its frustum test: the extent's bounding sphere (its
// corners placed by the root's transform) over the view window at depth 1, as a fraction of the window's area. Off-screen roots are
// measured like on-screen ones, so what is near stays loaded whichever way the camera looks.
auto screenSize(const SceneLodRoot& root, const glm::vec3& eye, float fovY, float aspect) -> float {
    auto worldMin = glm::vec3(root.worldTransform * glm::vec4(root.extent[0], 1.0f));
    auto worldMax = glm::vec3(root.worldTransform * glm::vec4(root.extent[1], 1.0f));
    auto radius = glm::length(worldMax - worldMin) * 0.5f;
    auto center = glm::vec3(root.worldTransform * glm::vec4((root.extent[0] + root.extent[1]) * 0.5f, 1.0f));
    auto distance = glm::length(eye - center);
    if (distance <= 1e-6f) {
        return 1e30f;
    }
    float r = radius / distance;
    float windowHeight = 2.0f * std::tan(fovY * 0.5f);
    float windowWidth = windowHeight * aspect;
    return glm::pi<float>() * r * r / (windowWidth * windowHeight);
}

// The level the thresholds pick for a screen size (UsdLod: the number of thresholds at or above it), with a band below each threshold
// before switching coarser: finer as soon as the size passes a threshold, coarser only once it is 20% below it.
auto levelFor(float size, const std::vector<float>& thresholds, uint32_t previous) -> uint32_t {
    auto count = [&](float scale) {
        uint32_t level = 0;
        while (level < thresholds.size() && size <= thresholds[level] * scale) {
            level++;
        }
        return level;
    };
    auto finer = count(1.0f);
    if (finer < previous) {
        return finer;
    }
    auto coarser = count(0.8f);
    if (coarser > previous) {
        return coarser;
    }
    return previous;
}

} // namespace

auto ViewScene::evaluateLevels() -> bool {
    if (!composer) {
        return false;
    }
    auto start = std::chrono::steady_clock::now();
    auto next = std::make_shared<SceneLodChoices>();
    uint32_t evaluated = 0;
    uint32_t finer = 0;
    uint32_t coarser = 0;
    for (const auto& root : draws->lodRoots) {
        auto levelCount = (uint32_t) root.levels.size();
        if (levelCount == 0) {
            continue;
        }
        SceneLodChoice choice;
        if (auto hand = handLevels.find(root.path); hand != handLevels.end()) {
            choice.level = std::min(hand->second, levelCount - 1);
            choice.reason = "hand-set";
        } else if (!heuristicOn) {
            choice.level = root.defaultIndex;
            choice.reason = "heuristic off";
        } else if (!root.heuristicChosen || root.thresholds.empty()) {
            choice.level = root.defaultIndex;
            choice.reason = "no usable heuristic";
        } else if (!camera) {
            choice.level = root.defaultIndex;
            choice.reason = "no camera";
        } else {
            choice.screenSize = screenSize(root, camera->position, camera->fovY, camera->aspect);
            auto previous = heuristicLevels.contains(root.path) ? heuristicLevels[root.path] : root.defaultIndex;
            choice.level = std::min(levelFor(choice.screenSize, root.thresholds, previous), levelCount - 1);
            heuristicLevels[root.path] = choice.level;
            choice.reason = "heuristic";
        }
        evaluated++;
        // The composer holds a level only where it isn't the root's default.
        bool held = choice.level != root.defaultIndex || handLevels.contains(root.path);
        auto pushed = composerLevels.find(root.path);
        uint32_t before = pushed != composerLevels.end() ? pushed->second : root.defaultIndex;
        if (held && (pushed == composerLevels.end() || pushed->second != choice.level)) {
            composer->setLodLevel(root.path, choice.level);
            composerLevels[root.path] = choice.level;
        } else if (!held && pushed != composerLevels.end()) {
            composer->resetLodLevel(root.path);
            composerLevels.erase(pushed);
        }
        if (choice.level < before) {
            finer++;
        } else if (choice.level > before) {
            coarser++;
        }
        (*next)[root.path] = std::move(choice);
    }
    stats.lodRootsEvaluated = evaluated;
    stats.lodFiner = finer;
    stats.lodCoarser = coarser;
    stats.lodEvaluations++;
    stats.lodEvaluateMs = msSince(start);
    choices = std::move(next);
    if (finer + coarser == 0) {
        return false;
    }
    TRACE_EVENT("Scene", "LevelsWanted", id)
        .text(std::format("LOD levels wanted: {} roots finer, {} coarser of {} evaluated", finer, coarser, stats.lodRootsEvaluated))
        .field("finer", (int64_t) finer)
        .field("coarser", (int64_t) coarser)
        .field("roots", (int64_t) stats.lodRootsEvaluated);
    return true;
}

// What no resolution has reached for the unload delay leaves the composer (at most once a second, or once per delay when it is shorter); what an extract released since the
// last time (mesh packs) is reported with it, one event per batch that let something go.
auto ViewScene::unload() -> void {
    auto now = std::chrono::steady_clock::now();
    uint32_t evicted = 0;
    auto delay = std::chrono::milliseconds((int64_t) (unloadDelaySeconds * 1000.0));
    if (composer && now - lastEvict >= std::min<std::chrono::steady_clock::duration>(std::chrono::seconds(1), delay)) {
        lastEvict = now;
        evicted = composer->evict(delay);
        stats.contentsEvicted += evicted;
    }
    auto meshes = stats.releasedMeshes - reportedReleasedMeshes;
    auto bytes = stats.releasedMeshBytes - reportedReleasedBytes;
    if (evicted == 0 && meshes == 0) {
        return;
    }
    reportedReleasedMeshes = stats.releasedMeshes;
    reportedReleasedBytes = stats.releasedMeshBytes;
    TRACE_EVENT("Scene", "LevelsUnloaded", id)
        .text(std::format("unloaded what wasn't used for {:g} s: {} composed contents, {} mesh packs ({:.1f} MiB)", unloadDelaySeconds, evicted, meshes, (double) bytes / (1024.0 * 1024.0)))
        .field("contents", (int64_t) evicted)
        .field("meshes", (int64_t) meshes)
        .field("bytes", (int64_t) bytes);
}

// Levels changed and nothing else: the composer links them again, composing level contents it doesn't keep yet, and asks for their packs.
auto ViewScene::relink() -> void {
    std::vector<std::pair<std::string, AssetKind>> fresh;
    for (const auto& needed : composer->relink()) {
        if (requestedPacks.insert(needed).second) {
            fresh.emplace_back(needed, AssetKind::Layer);
        }
    }
    requestAssets(fresh, false);
    stats.relinks++;
    countWorld();
    extractNeeded = true;
    requestNeeded = true;
}

// The prims World::visit reaches in a content: its entities (a level content's root among them, the stage's and a prototype's not), an
// instance's prototype in place of the instance's children, and a LOD level's linked content in place of its stand-in.
auto ViewScene::countWorld() -> void {
    worldPrims = composer ? countPrims(composer->world(), composer->world().stage, false) : 0;
}

auto ViewScene::countPrims(const World& world, const WorldContent& content, bool rootIsPrim) -> size_t {
    if (auto it = primCounts.find(&content); it != primCounts.end() && it->second.first == content.version && &content != &world.stage) {
        return it->second.second;
    }
    size_t count = 0;
    std::vector<EntityId> pending;
    if (rootIsPrim) {
        pending.push_back(0);
    } else {
        pending.assign(content.entities[0].children.begin(), content.entities[0].children.end());
    }
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        const auto& entity = content.entities[id];
        if (entity.linkedContent >= 0) {
            count += countPrims(world, world.linkedContents[(size_t) entity.linkedContent], true);
            continue;
        }
        count++;
        if (entity.prototype >= 0) {
            count += countPrims(world, world.prototypes[(size_t) entity.prototype], false);
            continue;
        }
        pending.insert(pending.end(), entity.children.begin(), entity.children.end());
    }
    primCounts[&content] = {content.version, count};
    return count;
}

auto ViewScene::publish() -> void {
    auto publishStart = std::chrono::steady_clock::now();
    auto snapshot = std::make_shared<SceneSnapshot>();
    snapshot->id = id;
    snapshot->revision = inputsRevision;
    snapshot->draws = draws;
    snapshot->transforms = transforms;
    snapshot->lodChoices = choices;
    snapshot->lodHeuristic = heuristicOn;
    snapshot->unloadDelay = unloadDelaySeconds;
    snapshot->thread = stats;
    for (const auto& [assetId, kind] : inFlight) {
        if (kind == AssetKind::Layer) {
            snapshot->thread.inFlightLayers++;
        } else if (kind == AssetKind::Mesh) {
            snapshot->thread.inFlightMeshes++;
        } else if (kind == AssetKind::Material) {
            snapshot->thread.inFlightMaterials++;
        } else if (kind == AssetKind::Texture) {
            snapshot->thread.inFlightTextures++;
        } else {
            snapshot->thread.inFlightOther++;
        }
    }
    snapshot->thread.layerPacks = (uint32_t) requestedPacks.size();
    snapshot->thread.meshPacks = (uint32_t) meshPacks.size();
    snapshot->thread.materialPacks = (uint32_t) materialPacks.size();
    snapshot->thread.texturePacks = (uint32_t) texturePacks.size();
    snapshot->thread.failedAssets = (uint32_t) failedAssets.size();
    snapshot->thread.meshEntries = meshLibrary.count();
    snapshot->thread.materialEntries = materialLibrary.count();
    snapshot->thread.textureEntries = textureLibrary.count();
    snapshot->thread.composeNeeded = composeNeeded;
    snapshot->thread.extractNeeded = extractNeeded;
    snapshot->thread.sinceOpenMs = id.empty() ? 0.0 : msSince(openedAt);
    snapshot->unitScale = unitScale;
    auto idKey = requestedPacks.size() + (meshPacks.size() << 16) + (materialPacks.size() << 32) + (texturePacks.size() << 40) + (failedAssets.size() << 48);
    if (idKey != assetIdKey) {
        assetIdKey = idKey;
        std::set<std::string> ids(requestedPacks.begin(), requestedPacks.end());
        for (const auto& [assetId, pack] : meshPacks) {
            ids.insert(assetId);
        }
        for (const auto& [assetId, pack] : materialPacks) {
            ids.insert(assetId);
        }
        for (const auto& [assetId, pack] : texturePacks) {
            ids.insert(assetId);
        }
        ids.insert(failedAssets.begin(), failedAssets.end());
        assetIdList.assign(ids.begin(), ids.end());
    }
    snapshot->assetIds = assetIdList;
    if (composer) {
        const auto& world = composer->world();
        snapshot->layerStack = world.layerStack;
        snapshot->upAxis = world.upAxis;
        snapshot->metersPerUnit = world.metersPerUnit;
        snapshot->prims = worldPrims;
        snapshot->prototypes = world.prototypes.size();
        snapshot->lodLevels = world.lodLevels;
    }
    snapshot->idle = inFlight.empty() && !composeNeeded && !extractNeeded;
    std::lock_guard lock(mutex);
    published = std::move(snapshot);
    working = false;
    stats.publishMs = msSince(publishStart);
}

auto ViewScene::drawsPurpose(Purpose purpose) const -> bool {
    return drawnPurposes.contains(purpose);
}

auto ViewScene::worldUp() const -> glm::vec3 {
    if (current->upAxis == "Z") {
        return {0.0f, 0.0f, 1.0f};
    }
    return {0.0f, 1.0f, 0.0f};
}

auto ViewScene::primPath(uint32_t prim) const -> std::string {
    if (prim == 0 || prim > current->draws->primPaths.size()) {
        return {};
    }
    return current->draws->primPaths[prim - 1];
}

auto ViewScene::instancePath(uint32_t instance) const -> std::string {
    const auto& instances = current->draws->drawn.meshInstances;
    if (instance >= instances.size() || !instances[instance].alive) {
        return {};
    }
    return primPath(instances[instance].prim);
}

auto ViewScene::drawsOf(const std::string& assetId) const -> std::vector<uint32_t> {
    if (auto it = current->draws->drawsByAsset.find(assetId); it != current->draws->drawsByAsset.end()) {
        return it->second;
    }
    return {};
}

auto ViewScene::drawCount(const std::string& assetId) const -> size_t {
    if (auto it = current->draws->drawsByAsset.find(assetId); it != current->draws->drawsByAsset.end()) {
        return it->second.size();
    }
    return 0;
}

auto ViewScene::assetsUnder(const std::string& path) const -> std::vector<std::string> {
    std::set<std::string> out;
    const auto& byPrim = current->draws->assetsByPrim;
    for (size_t i = 0; i < byPrim.size(); i++) {
        const auto& uses = byPrim[i];
        if (!uses) {
            continue;
        }
        const auto& candidate = current->draws->primPaths[i];
        bool under = path == "/" || candidate == path || (candidate.size() > path.size() && candidate.starts_with(path) && candidate[path.size()] == '/');
        if (under) {
            out.insert(uses->begin(), uses->end());
        }
    }
    return {out.begin(), out.end()};
}

auto ViewScene::primBounds(uint32_t prim) const -> AABB {
    AABB out = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
    if (auto it = current->draws->drawn.primInstances.find(prim); it != current->draws->drawn.primInstances.end()) {
        for (auto i : it->second) {
            const auto& bounds = current->draws->drawn.meshInstances[i].worldBounds;
            if (bounds.valid()) {
                out.min = glm::min(out.min, bounds.min);
                out.max = glm::max(out.max, bounds.max);
            }
        }
    }
    return out;
}

auto ViewScene::bounds(const std::string& path) const -> AABB {
    AABB out = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
    auto under = [&path](const std::string& candidate) {
        if (path == "/") {
            return true;
        }
        return candidate == path || (candidate.size() > path.size() && candidate.starts_with(path) && candidate[path.size()] == '/');
    };
    for (const auto& [prim, slots] : current->draws->drawn.primInstances) {
        if (!under(primPath(prim))) {
            continue;
        }
        for (auto i : slots) {
            const auto& bounds = current->draws->drawn.meshInstances[i].worldBounds;
            if (bounds.valid()) {
                out.min = glm::min(out.min, bounds.min);
                out.max = glm::max(out.max, bounds.max);
            }
        }
    }
    return out;
}

auto ViewScene::transform(const std::string& path) const -> std::optional<PrimTransform> {
    auto found = current->transforms->find(path);
    if (!found) {
        return std::nullopt;
    }
    return PrimTransform{.local = found->local, .world = current->unitScale * found->world, .xformable = found->xformable};
}

auto ViewScene::texture(const std::string& textureAsset) -> TextureHandle {
    if (auto handle = textureLibrary.find(textureAsset)) {
        return handle;
    }
    auto pack = texturePacks.find(textureAsset);
    if (pack == texturePacks.end()) {
        return {};
    }
    return textureLibrary.add(textureAsset, pack->second);
}

// A constant colour as a 1x1 texel, so it takes the same sampling path as a texture; one per colour, shared.
auto ViewScene::constantTexture(const glm::vec4& color) -> TextureHandle {
    std::array<uint8_t, 4> texel = {encodeSrgb(color.r), encodeSrgb(color.g), encodeSrgb(color.b), 255};
    auto key = std::format("constant {} {} {}", texel[0], texel[1], texel[2]);
    if (auto handle = textureLibrary.find(key)) {
        return handle;
    }
    auto desc = std::make_shared<TextureDesc>();
    desc->width = 1;
    desc->height = 1;
    desc->mipLevels = 1;
    const auto* bytes = reinterpret_cast<const std::byte*>(texel.data());
    desc->levels.assign(bytes, bytes + texel.size());
    return textureLibrary.add(key, std::move(desc));
}

auto ViewScene::mesh(const std::string& meshAsset) -> MeshHandle {
    if (auto handle = meshLibrary.find(meshAsset)) {
        return handle;
    }
    auto pack = meshPacks.find(meshAsset);
    if (pack == meshPacks.end() || pack->second.submeshes.empty()) {
        return {};
    }
    const auto& meshPack = pack->second;
    auto desc = std::make_shared<MeshDesc>();
    desc->vertices.reserve(meshPack.vertices.size());
    for (const auto& vertex : meshPack.vertices) {
        desc->vertices.push_back(toVertex(vertex));
    }
    desc->indices = meshPack.indices;
    desc->doubleSided = meshPack.doubleSided;
    for (const auto& submesh : meshPack.submeshes) {
        desc->submeshes.push_back({.indexOffset = submesh.indexOffset, .indexCount = submesh.indexCount});
    }
    auto bounds = meshBounds(*desc);
    auto handle = meshLibrary.add(meshAsset, std::move(desc));
    meshBoundsOf[handle.index] = bounds;
    return handle;
}

// A material: by its asset id; with no material bound, one per display colour, shared (a scene of many unbound prims would otherwise
// fill the renderer's texture slots with 1x1 textures of the same few colours). A bound material that authors neither a texture nor a
// colour takes the display colour of the first mesh that asks for it in an extract, as it always has. The entry is kept while its
// content is the same; content that changed (a repacked material or texture) makes a new entry.
auto ViewScene::resolveMaterial(const std::string& key) const -> ResolvedMaterial {
    ResolvedMaterial resolved;
    auto use = parseMaterialKey(key);
    resolved.packId = use.asset;
    auto pack = materialPacks.find(use.asset);
    if (pack == materialPacks.end()) {
        return resolved;
    }
    resolved.pack = &pack->second;
    auto textureOf = [&use](const std::string& fallback, const std::string& input) {
        if (!input.empty()) {
            if (auto value = use.input(input)) {
                return *value;
            }
        }
        return fallback;
    };
    resolved.baseColorTexture = textureOf(pack->second.baseColorTexture, pack->second.baseColorTextureInput);
    resolved.opacityTexture = textureOf(pack->second.opacityTexture, pack->second.opacityTextureInput);
    return resolved;
}

auto ViewScene::material(const std::string& materialAsset, const std::optional<std::array<float, 3>>& displayColor) -> MaterialHandle {
    std::string key = materialAsset;
    if (key.empty()) {
        auto color = displayColor.value_or(std::array<float, 3>{1.0f, 1.0f, 1.0f});
        key = std::format("displayColor {} {} {}", color[0], color[1], color[2]);
    }
    if (auto it = materialByAsset.find(key); it != materialByAsset.end()) {
        return it->second;
    }
    MaterialDesc desc;
    if (auto resolved = resolveMaterial(materialAsset); resolved.pack != nullptr) {
        const auto& material = *resolved.pack;
        desc.baseColorTexture = resolved.baseColorTexture.empty() ? TextureHandle{} : texture(resolved.baseColorTexture);
        if (!desc.baseColorTexture && material.hasBaseColor) {
            const auto& c = material.baseColor;
            desc.baseColorFactor = glm::vec4(c[0], c[1], c[2], 1.0f);
        }
        if (material.opacityThreshold > 0.0f) {
            desc.opacityThreshold = material.opacityThreshold;
            desc.opacity = material.opacity;
            desc.opacityChannel = material.opacityChannel;
            desc.opacityRaw = material.opacityRaw;
            if (!resolved.opacityTexture.empty() && resolved.opacityTexture == resolved.baseColorTexture && desc.baseColorTexture) {
                desc.opacityFromBaseColor = true;
            } else if (!resolved.opacityTexture.empty()) {
                desc.opacityTexture = texture(resolved.opacityTexture);
            }
        }
    }
    if (!desc.baseColorTexture && desc.baseColorFactor == glm::vec4(1.0f) && displayColor) {
        desc.baseColorFactor = glm::vec4((*displayColor)[0], (*displayColor)[1], (*displayColor)[2], 1.0f);
    }
    if (!desc.baseColorTexture) {
        desc.baseColorTexture = constantTexture(desc.baseColorFactor);
    }
    auto handle = materialLibrary.find(key);
    if (const auto* existing = materialLibrary.get(handle); existing == nullptr || !(*existing == desc)) {
        handle = materialLibrary.add(key, std::make_shared<MaterialDesc>(desc));
    }
    materialByAsset[key] = handle;
    return handle;
}

// A use with a gizmo's local opinion under it is walked afresh, not taken from the cache.
auto ViewScene::overridesUnder(const std::string& usePath) const -> bool {
    if (localOverrides.empty()) {
        return false;
    }
    if (usePath == "/") {
        return true;
    }
    auto prefix = usePath + "/";
    return std::ranges::any_of(localOverrides, [&](const auto& entry) { return entry.first.starts_with(prefix); });
}

// One content's walk from its root down: its prims that matter to extract, with transforms relative to the root, and where its
// instances and levels sit. overrideBase, when set, is the content's use path, to apply the gizmo's local opinions.
auto ViewScene::buildExtract(const WorldContent& content, bool rootIsPrim, Purpose purpose, const std::string& material, bool strong, const std::string& overrideBase, EntityId root)
    -> std::shared_ptr<const ContentExtract> {
    auto walkRoot = root != noEntity ? root : (rootIsPrim ? (EntityId) 0 : noEntity);
    auto out = std::make_shared<ContentExtract>();
    out->version = content.version;
    auto prims = std::make_shared<ContentTransforms>();
    struct State {
        std::string path;
        glm::mat4 world = glm::mat4(1.0f);
        bool absolute = false;
        Purpose purpose = Purpose::Default;
        std::string material;
        bool strong = false;
    };
    std::function<void(EntityId, const State&)> walk = [&](EntityId id, const State& parent) {
        const auto& entity = content.entities[id];
        State state;
        state.path = id == walkRoot ? std::string("/") : childPathOf(parent.path, entity.name);
        // A prim with payloads stands in for its linked content, used where it sits (read at use: resolution may link another).
        if (entity.linkedContent >= 0 && id != walkRoot) {
            out->items.push_back({.kind = ExtractItem::Kind::Level,
                                  .path = state.path,
                                  .world = parent.world,
                                  .absolute = parent.absolute,
                                  .id = id,
                                  .purpose = parent.purpose,
                                  .material = parent.material,
                                  .strongBinding = parent.strong});
            return;
        }
        glm::mat4 local(entity.localTransform);
        if (!overrideBase.empty() && entity.xformable) {
            if (auto authored = localOverrides.find(joinPathOf(overrideBase, state.path)); authored != localOverrides.end()) {
                local = authored->second;
            }
        }
        state.absolute = parent.absolute;
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
        ExtractItem prim{.kind = ExtractItem::Kind::Prim,
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
                         .lodRoot = content.lodRoots.contains(id),
                         .light = content.lights.contains(id)};
        if (!entity.meshAsset.empty()) {
            auto [it, added] = meshIds.try_emplace(entity.meshAsset, (uint32_t) meshIds.size());
            prim.meshId = it->second;
            auto [material, materialAdded] = materialIds.try_emplace(state.material, (uint32_t) materialIds.size());
            prim.materialId = material->second;
            prim.subsets = std::ranges::any_of(entity.children, [&](EntityId child) { return content.entities[child].binding.has_value(); });
        }
        if (prim.meshId != UINT32_MAX || prim.camera || prim.payload || prim.lodRoot || prim.light) {
            out->items.push_back(std::move(prim));
        }
        if (content.pointInstancers.contains(id)) {
            out->items.push_back({.kind = ExtractItem::Kind::PointInstancer,
                                  .path = state.path,
                                  .world = state.world,
                                  .absolute = state.absolute,
                                  .id = id,
                                  .purpose = state.purpose,
                                  .material = state.material,
                                  .strongBinding = state.strong});
        }
        if (entity.prototype >= 0) {
            out->items.push_back({.kind = ExtractItem::Kind::Instance,
                                  .path = state.path,
                                  .world = state.world,
                                  .absolute = state.absolute,
                                  .purpose = state.purpose,
                                  .material = state.material,
                                  .strongBinding = state.strong,
                                  .content = entity.prototype});
            return;
        }
        if (auto lod = content.lodRoots.find(id); lod != content.lodRoots.end()) {
            // A level's link to its content changes at resolution without the content changing: it is read where the content is used.
            for (uint32_t i = 0; i < lod->second.levels.size(); i++) {
                auto standIn = lod->second.levels[i];
                out->items.push_back({.kind = ExtractItem::Kind::Level,
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

// A content's walk, kept by its version and the state it inherits; afresh when the gizmo has moved something in it.
auto ViewScene::extractOf(char kind, int32_t index, const WorldContent& content, bool rootIsPrim, Purpose purpose, const std::string& material, bool strong, const std::string& usePath, EntityId root)
    -> std::shared_ptr<const ContentExtract> {
    if (overridesUnder(usePath)) {
        return buildExtract(content, rootIsPrim, purpose, material, strong, usePath, root);
    }
    auto key = std::format("{}{} {} {} {} {} {}", kind, index, root, content.version, (int) purpose, strong ? 1 : 0, material);
    if (auto it = extractCache.find(key); it != extractCache.end()) {
        extractCacheHits++;
        return it->second;
    }
    extractCacheBuilds++;
    auto built = buildExtract(content, rootIsPrim, purpose, material, strong, {}, root);
    extractCache[key] = built;
    return built;
}

// The World's kept walks replayed from the stage down, in walk order: each prim that matters at its full path and world transform (before
// the unit scale), with the drawn LOD level + 1 of the nearest root above it. With `transformsOut`, every use of a content is registered there.
auto ViewScene::contentReady(const World& w, char kind, int32_t index, Purpose purpose) -> bool {
    auto key = ((int64_t) kind << 40) | ((int64_t) purpose << 36) | (int64_t) index;
    if (auto it = readiness.find(key); it != readiness.end()) {
        return it->second;
    }
    readiness[key] = false; // a cycle can't make it ready
    const WorldContent& content = kind == 'P' ? w.prototypes[(size_t) index] : w.linkedContents[(size_t) index];
    bool ready = content.complete;
    if (ready) {
        auto walked = extractOf(kind, index, content, kind == 'L', purpose, {}, false, content.sourcePath);
        ready = itemsReady(w, kind, index, content, *walked);
    }
    readiness[key] = ready;
    return ready;
}

auto ViewScene::itemsReady(const World& w, char kind, int32_t index, const WorldContent& content, const ContentExtract& walked) -> bool {
    bool ready = true;
    for (const auto& item : walked.items) {
        if (item.kind == ExtractItem::Kind::PointInstancer) {
            // Its prototypes' walks, as the points draw them.
            for (EntityId prototype : content.pointInstancers.at(item.id).prototypes) {
                if (prototype != noEntity) {
                    auto prototypeWalk = extractOf(kind, index, content, true, item.purpose, item.material, item.strongBinding, content.sourcePath, prototype);
                    ready = itemsReady(w, kind, index, content, *prototypeWalk);
                }
                if (!ready) {
                    break;
                }
            }
        } else if (item.kind == ExtractItem::Kind::Prim) {
            const auto& meshAsset = item.entity->meshAsset;
            if (!meshAsset.empty() && drawsPurpose(item.purpose) && !meshPacks.contains(meshAsset) && !failedAssets.contains(meshAsset)) {
                ready = false;
            }
        } else if (item.kind == ExtractItem::Kind::Instance) {
            ready = contentReady(w, 'P', item.content, item.purpose);
        } else if (item.root == noEntity) {
            auto linked = content.entities[item.id].linkedContent;
            ready = linked >= 0 && contentReady(w, 'L', linked, item.purpose);
        } else if (item.level + 1 == content.lodRoots.at(item.root).levels.size()) {
            // A LOD root in it counts by its coarsest level, the floor it draws while a finer one loads.
            auto linked = content.entities[item.id].linkedContent;
            ready = linked >= 0 && contentReady(w, 'L', linked, item.purpose);
        }
        if (!ready) {
            break;
        }
    }
    return ready;
}

auto ViewScene::countWaiting(const World& w, char kind, int32_t index, Purpose purpose, std::set<int64_t>& seenContents, std::set<std::string>& seenMeshes, SceneLodRoot::Waiting& waiting) -> void {
    auto key = ((int64_t) kind << 40) | ((int64_t) purpose << 36) | (int64_t) index;
    if (!seenContents.insert(key).second) {
        return;
    }
    const WorldContent& content = kind == 'P' ? w.prototypes[(size_t) index] : w.linkedContents[(size_t) index];
    if (!content.complete) {
        waiting.contentsIncomplete++;
    }
    auto walked = extractOf(kind, index, content, kind == 'L', purpose, {}, false, content.sourcePath);
    std::vector<const ContentExtract*> walks = {walked.get()};
    std::vector<std::shared_ptr<const ContentExtract>> prototypeWalks;
    for (size_t at = 0; at < walks.size(); at++) {
        for (const auto& item : walks[at]->items) {
            if (item.kind == ExtractItem::Kind::PointInstancer) {
                for (EntityId prototype : content.pointInstancers.at(item.id).prototypes) {
                    if (prototype != noEntity) {
                        prototypeWalks.push_back(extractOf(kind, index, content, true, item.purpose, item.material, item.strongBinding, content.sourcePath, prototype));
                        walks.push_back(prototypeWalks.back().get());
                    }
                }
            } else if (item.kind == ExtractItem::Kind::Prim) {
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
            } else if (item.kind == ExtractItem::Kind::Instance) {
                countWaiting(w, 'P', item.content, item.purpose, seenContents, seenMeshes, waiting);
            } else if (item.root == noEntity || item.level + 1 == content.lodRoots.at(item.root).levels.size()) {
                auto linked = content.entities[item.id].linkedContent;
                if (linked >= 0) {
                    countWaiting(w, 'L', linked, item.purpose, seenContents, seenMeshes, waiting);
                }
            }
        }
    }
}

auto lodRootInFrustum(const SceneLodRoot& root, const glm::vec3& eye, const glm::vec3& forward, const glm::vec3& right, float fovY, float aspect) -> bool {
    auto worldMin = glm::vec3(root.worldTransform * glm::vec4(root.extent[0], 1.0f));
    auto worldMax = glm::vec3(root.worldTransform * glm::vec4(root.extent[1], 1.0f));
    auto radius = glm::length(worldMax - worldMin) * 0.5f;
    auto center = glm::vec3(root.worldTransform * glm::vec4((root.extent[0] + root.extent[1]) * 0.5f, 1.0f));
    auto toCenter = center - eye;
    auto up = glm::cross(right, forward);
    float depth = glm::dot(toCenter, forward);
    float x = glm::dot(toCenter, right);
    float y = glm::dot(toCenter, up);
    float tanY = std::tan(fovY * 0.5f);
    float tanX = tanY * aspect;
    // A side plane's distance to the centre, against the radius: |x| - depth * tan is along the view, scaled by the plane's normal.
    bool insideX = std::abs(x) - depth * tanX <= radius * std::sqrt(1.0f + tanX * tanX);
    bool insideY = std::abs(y) - depth * tanY <= radius * std::sqrt(1.0f + tanY * tanY);
    return depth >= -radius && insideX && insideY;
}

auto ViewScene::drawnLevelOf(const World& w, const std::string& rootPath, const LodRoot& root, const WorldContent& content, Purpose purpose) -> std::optional<uint32_t> {
    if (root.loaded.empty()) {
        return std::nullopt;
    }
    auto wanted = w.wantedLevel(rootPath, root);
    // Loaded levels are finest first (set order): the first at or coarser than the wanted one that is complete.
    std::optional<uint32_t> drawn;
    for (uint32_t level : root.loaded) {
        if (level < wanted) {
            continue;
        }
        auto linked = level < root.levels.size() ? content.entities[root.levels[level]].linkedContent : -1;
        if (linked >= 0 && contentReady(w, 'L', linked, purpose)) {
            drawn = level;
            break;
        }
    }
    if (!drawn) {
        // Nothing complete yet: the coarsest loaded level, as it arrives (the scene opening).
        drawn = *std::ranges::max_element(root.loaded);
    }
    return drawn;
}

auto ViewScene::replay(const World& w, const PrimVisitor& onPrim, TransformIndex* transformsOut, bool allLoaded) -> void {
    // A walk's items where it is used; a content's use, which walks it (kept) and uses its items.
    std::function<void(char, int32_t, const WorldContent&, const ContentExtract&, const std::string&, const glm::mat4&, uint32_t)> useItems;
    std::function<void(char, int32_t, const WorldContent&, bool, const std::string&, const glm::mat4&, Purpose, const std::string&, bool, uint32_t)> useContent =
        [&](char kind, int32_t index, const WorldContent& content, bool rootIsPrim, const std::string& usePath, const glm::mat4& useWorld, Purpose purpose, const std::string& material, bool strong, uint32_t lodTag) {
            auto walked = extractOf(kind, index, content, rootIsPrim, purpose, material, strong, usePath);
            if (transformsOut != nullptr) {
                transformsOut->uses[usePath] = {.prims = walked->transforms, .world = useWorld};
            }
            useItems(kind, index, content, *walked, usePath, useWorld, lodTag);
        };
    useItems = [&](char kind, int32_t index, const WorldContent& content, const ContentExtract& walked, const std::string& usePath, const glm::mat4& useWorld, uint32_t lodTag) {
        for (const auto& item : walked.items) {
            auto world = item.absolute ? item.world : useWorld * item.world;
            auto path = joinPathOf(usePath, item.path);
            if (item.kind == ExtractItem::Kind::PointInstancer) {
                // Each visible point uses its prototype's walk, at a path of its own (the prototype's, then "#" and the point's id),
                // so its draws are its own. Points have no entry in the transform index.
                const auto& data = content.pointInstancers.at(item.id);
                std::vector<std::shared_ptr<const ContentExtract>> prototypeWalks;
                std::vector<std::string> prototypePaths;
                for (EntityId prototype : data.prototypes) {
                    if (prototype == noEntity) {
                        prototypeWalks.push_back(nullptr);
                        prototypePaths.emplace_back();
                        continue;
                    }
                    // The prototype's path below the instancer, in their content.
                    auto inContent = content.path(prototype);
                    auto instancerInContent = content.path(item.id);
                    prototypePaths.push_back(path + inContent.substr(std::min(inContent.size(), instancerInContent.size())));
                    prototypeWalks.push_back(extractOf(kind, index, content, true, item.purpose, item.material, item.strongBinding, prototypePaths.back(), prototype));
                }
                for (size_t point = 0; point < data.protoIndices.size(); point++) {
                    auto which = data.protoIndices[point];
                    if (data.visible[point] == 0 || which >= prototypeWalks.size() || !prototypeWalks[which]) {
                        continue;
                    }
                    auto id = data.ids.empty() ? (int64_t) point : data.ids[point];
                    auto pointPath = std::format("{}#{}", prototypePaths[which], id);
                    useItems(kind, index, content, *prototypeWalks[which], pointPath, world * glm::mat4(data.transforms[point]), lodTag);
                }
                continue;
            }
            if (item.kind == ExtractItem::Kind::Instance) {
                useContent('P', item.content, w.prototypes[(size_t) item.content], false, path, world, item.purpose, item.material, item.strongBinding, lodTag);
                continue;
            }
            if (item.kind == ExtractItem::Kind::Level) {
                auto linked = content.entities[item.id].linkedContent;
                if (linked < 0) {
                    continue;
                }
                // A prim with payloads (no root) is always used; a level only when the root draws it: the finest loaded one not finer
                // than the one it wants, per use of the root.
                auto tag = lodTag;
                if (item.root != noEntity) {
                    const auto& root = content.lodRoots.at(item.root);
                    if (allLoaded) {
                        if (std::ranges::find(root.loaded, item.level) == root.loaded.end()) {
                            continue;
                        }
                    } else {
                        auto drawnLevel = drawnLevelOf(w, parentPathOf(path), root, content, item.purpose);
                        if (!drawnLevel || *drawnLevel != item.level) {
                            continue;
                        }
                        tag = *drawnLevel + 1;
                    }
                }
                useContent('L', linked, w.linkedContents[(size_t) linked], true, path, world, item.purpose, item.material, item.strongBinding, tag);
                continue;
            }
            onPrim(content, item, path, world, lodTag);
        }
    };
    useContent('S', 0, w.stage, false, "/", glm::mat4(1.0f), Purpose::Default, {}, false, 0);
}

// The renderer's inputs from the World: a draw instance per submesh of every drawn mesh, and the distant lights. World transforms are
// computed as the view always has: in float, parent times local, then scaled to metres by the scene's metersPerUnit.
auto ViewScene::extract() -> void {
    extractNeeded = false;
    readiness.clear();
    auto start = std::chrono::steady_clock::now();
    SceneChanges changes;
    auto previousLights = std::move(drawn.lights);
    drawn.lights.clear();
    drawn.primInstances.clear();
    primPaths.clear();
    materialByAsset.clear();
    authoredCameras.clear();
    drawnLodRoots.clear();
    reachedPayloads.clear();
    drawsByAsset.clear();
    assetsByPrim.clear();
    // The last extract's sizes: what this one will most likely build again.
    primPaths.reserve(draws->primPaths.size());
    assetsByPrim.reserve(draws->primPaths.size());
    static const World emptyWorld;
    const auto& w = composer ? composer->world() : emptyWorld;
    unitScale = glm::mat4(1.0f);
    if (w.metersPerUnit != 1.0) {
        unitScale = glm::scale(glm::mat4(1.0f), glm::vec3((float) w.metersPerUnit));
    }

    // The draws, in visit order, keyed by prim path and submesh.
    struct Draw {
        std::string key;
        RenderMeshInstance instance;
    };
    std::vector<Draw> draws;
    draws.reserve(this->draws->drawn.meshInstances.size());
    std::set<uint32_t> meshesUsed;
    std::set<uint32_t> materialsUsed;
    std::set<uint32_t> texturesUsed;

    auto transformIndex = std::make_shared<TransformIndex>();
    bool hasDistantLight = false;

    // A mesh and the material bound where it is drawn, resolved once per extract: the materials of its submeshes, whether any cuts, and the
    // assets its draws use (the mesh, the file defining it, each material and its textures), with the lists that name its prims.
    struct Resolved {
        MeshHandle mesh;
        const MeshDesc* meshData = nullptr;
        std::vector<MaterialHandle> materials;
        std::vector<bool> cutout;
        bool primHasCutout = false;
        std::shared_ptr<const std::vector<std::string>> uses;
        std::vector<std::vector<uint32_t>*> useLists;
    };
    std::unordered_map<uint64_t, Resolved> resolvedByKey; // by mesh id, material id and the binding's strength
    // A mesh's handle by its id, looked up once per extract; checked marks the ids looked up (a null handle: its pack isn't held).
    std::vector<MeshHandle> meshMemo(meshIds.size() + 1024);
    std::vector<bool> meshChecked(meshIds.size() + 1024, false);
    // The submesh materials and uses of a mesh where bound to `boundMaterial`, with the subsets' own bindings when `subsetsOf` is set.
    auto resolve = [&](const std::string& meshAsset, const std::string& boundMaterial, bool strongBinding, const Entity* subsetsOf, const WorldContent& content, MeshHandle meshHandle) -> Resolved {
        Resolved out;
        out.mesh = meshHandle;
        out.meshData = meshLibrary.get(meshHandle);
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
                out.materials.push_back(material(boundMaterial, submesh.subsetPath.empty() ? displayColor : std::nullopt));
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
                out.materials.push_back(material(subsetMaterial, std::nullopt));
                useMaterial(subsetMaterial);
            }
        }
        for (auto handle : out.materials) {
            const auto* materialData = materialLibrary.get(handle);
            bool cuts = materialData != nullptr && materialData->cutout();
            out.cutout.push_back(cuts);
            out.primHasCutout = out.primHasCutout || cuts;
            materialsUsed.insert(handle.index);
            if (materialData != nullptr) {
                for (auto texture : {materialData->baseColorTexture, materialData->opacityTexture}) {
                    if (texture) {
                        texturesUsed.insert(texture.index);
                    }
                }
            }
        }
        std::ranges::sort(*uses);
        auto [last, end] = std::ranges::unique(*uses);
        uses->erase(last, end);
        for (const auto& used : *uses) {
            out.useLists.push_back(&drawsByAsset[used]);
        }
        out.uses = std::move(uses);
        meshesUsed.insert(meshHandle.index);
        return out;
    };

    // What a prim gives the renderer and the queries, at its full path and world transform (in metres) where its content is used.
    auto primAt = [&](const WorldContent& content, const ExtractItem& item, const std::string& path, const glm::mat4& worldTransform, uint32_t lodTag) -> void {
        const auto& entity = *item.entity;
        if (item.camera) {
            authoredCameras.push_back({.path = path, .worldTransform = worldTransform});
        }
        if (!item.payload) {
        } else if (auto payload = content.payloads.find(item.id); payload != content.payloads.end()) {
            ScenePayload placement = {.path = path, .assetIds = payload->second.assetIds, .loaded = payload->second.loaded};
            if (const auto& hint = payload->second.extentsHint) {
                placement.worldBounds = AABB{.min = (*hint)[0], .max = (*hint)[1]}.transformed(worldTransform);
            }
            reachedPayloads.push_back(std::move(placement));
        }
        if (!item.lodRoot) {
        } else if (auto lod = content.lodRoots.find(item.id); lod != content.lodRoots.end()) {
            const auto& root = lod->second;
            SceneLodRoot row;
            row.path = path;
            for (EntityId level : root.levels) {
                row.levels.push_back(content.entities[level].name);
                auto payload = content.payloads.find(level);
                row.levelPayloads.push_back(payload != content.payloads.end() ? payload->second.assetIds : std::vector<std::string>{});
            }
            row.heuristicType = root.heuristicType;
            row.heuristicChosen = root.heuristicChosen;
            row.thresholds = root.thresholds;
            row.extent = root.extent;
            row.projectedExtent = root.projection == LodProjection::Extent;
            row.defaultIndex = root.defaultIndex;
            row.wanted = w.wantedLevel(path, root);
            row.drawn = drawnLevelOf(w, path, root, content, item.purpose);
            row.loaded = root.loaded;
            row.setByHand = handLevels.contains(path);
            row.worldTransform = worldTransform;
            if (row.drawn && *row.drawn != row.wanted && std::ranges::contains(root.loaded, row.wanted) && row.wanted < root.levels.size()) {
                auto linked = content.entities[root.levels[row.wanted]].linkedContent;
                if (linked >= 0) {
                    SceneLodRoot::Waiting waiting;
                    std::set<int64_t> seenContents;
                    std::set<std::string> seenMeshes;
                    countWaiting(w, 'L', linked, item.purpose, seenContents, seenMeshes, waiting);
                    row.waiting = waiting;
                }
            }
            drawnLodRoots.push_back(std::move(row));
        }

        if (!item.light) {
        } else if (auto light = content.lights.find(item.id); light != content.lights.end()) {
            if (light->second.shape == LightShape::Distant) {
                hasDistantLight = true;
                primPaths.push_back(path);
                drawn.lights.push_back({
                    .type = LightType::Directional,
                    .color = light->second.color,
                    .intensity = light->second.intensity,
                    .exposure = light->second.exposure,
                    .angle = light->second.angle,
                    .shadowEnable = light->second.shadowEnable,
                    .shadowColor = light->second.shadowColor,
                    .worldTransform = worldTransform,
                    .primHandle = PrimHandle{(uint32_t) primPaths.size()},
                });
            }
        }

        if (item.meshId == UINT32_MAX || !drawsPurpose(item.purpose)) {
            return;
        }
        if (item.meshId >= meshMemo.size()) {
            meshMemo.resize(item.meshId + 1024);
            meshChecked.resize(item.meshId + 1024, false);
        }
        if (!meshChecked[item.meshId]) {
            meshChecked[item.meshId] = true;
            meshMemo[item.meshId] = mesh(entity.meshAsset);
        }
        auto meshHandle = meshMemo[item.meshId];
        if (!meshHandle) {
            return;
        }
        // Prims of the same mesh bound to the same material share what they resolve to; a mesh with subset bindings resolves per prim.
        const Resolved* resolved = nullptr;
        Resolved own;
        if (item.subsets) {
            own = resolve(entity.meshAsset, item.material, item.strongBinding, &entity, content, meshHandle);
            resolved = &own;
        } else {
            auto key = ((uint64_t) item.meshId << 32) | ((uint64_t) item.materialId << 1) | (item.strongBinding ? 1u : 0u);
            auto it = resolvedByKey.find(key);
            if (it == resolvedByKey.end()) {
                it = resolvedByKey.emplace(key, resolve(entity.meshAsset, item.material, item.strongBinding, nullptr, content, meshHandle)).first;
            }
            resolved = &it->second;
        }
        const auto* meshData = resolved->meshData;
        auto worldBounds = meshBoundsOf[meshHandle.index].transformed(worldTransform);

        primPaths.push_back(path);
        auto prim = (uint32_t) primPaths.size();
        for (size_t s = 0; s < meshData->submeshes.size(); s++) {
            const auto& sub = meshData->submeshes[s];
            auto key = path;
            key += '#';
            key += std::to_string(s);
            draws.push_back({
                .key = std::move(key),
                .instance = {
                    .mesh = meshHandle,
                    .material = resolved->materials[s],
                    .prim = prim,
                    .worldTransform = worldTransform,
                    .worldBounds = worldBounds,
                    .indexOffset = sub.indexOffset,
                    .indexCount = sub.indexCount,
                    .primFirst = (s == 0),
                    .doubleSided = meshData->doubleSided,
                    .cutout = resolved->cutout[s],
                    .primHasCutout = resolved->primHasCutout,
                    .lodLevel = lodTag,
                },
            });
        }
        for (auto* list : resolved->useLists) {
            list->push_back(prim);
        }
        assetsByPrim.resize(prim);
        assetsByPrim[prim - 1] = resolved->uses;
    };

    replay(w, [&](const WorldContent& content, const ExtractItem& item, const std::string& path, const glm::mat4& world, uint32_t lodTag) { primAt(content, item, path, unitScale * world, lodTag); }, transformIndex.get());
    // Kept walks of contents composed again since are dropped.
    std::erase_if(extractCache, [&](const auto& entry) {
        const auto& key = entry.first;
        auto index = std::atoi(key.c_str() + 1);
        const WorldContent* content = nullptr;
        if (key[0] == 'S') {
            content = &w.stage;
        } else if (key[0] == 'P' && index < (int) w.prototypes.size()) {
            content = &w.prototypes[(size_t) index];
        } else if (key[0] == 'L' && index < (int) w.linkedContents.size()) {
            content = &w.linkedContents[(size_t) index];
        }
        return content == nullptr || content->version != entry.second->version;
    });
    transforms = std::move(transformIndex);
    stats.extractReused = extractCacheHits;
    stats.extractWalked = extractCacheBuilds;
    stats.extractKept = extractCache.size();
    extractCacheHits = 0;
    extractCacheBuilds = 0;

    stats.extractVisitMs = msSince(start);
    auto slotsStart = std::chrono::steady_clock::now();
    // The engine's light when the scene has no distant light: upper-front-right in the scene's own frame, as it always was.
    if (!hasDistantLight) {
        bool zUp = w.upAxis == "Z";
        auto dir = zUp ? glm::normalize(glm::vec3(1.0f, 1.0f, 2.0f)) : glm::normalize(glm::vec3(1.0f, 2.0f, 1.0f));
        auto sceneUp = zUp ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        auto xAxis = glm::normalize(glm::cross(sceneUp, dir));
        auto yAxis = glm::cross(dir, xAxis);
        // Through double, as the light's transform op once held it.
        glm::dmat4 basis(glm::dvec4(xAxis, 0.0), glm::dvec4(yAxis, 0.0), glm::dvec4(dir, 0.0), glm::dvec4(0.0, 0.0, 0.0, 1.0));
        primPaths.push_back("/__DefaultEngineLight");
        drawn.lights.push_back({
            .type = LightType::Directional,
            .color = glm::vec3(1.0f, 0.95f, 0.88f),
            .intensity = 1.0f,
            .exposure = 0.0f,
            .angle = 0.53f,
            .shadowEnable = true,
            .shadowColor = glm::vec3(0.0f),
            .worldTransform = unitScale * glm::mat4(basis),
            .primHandle = PrimHandle{(uint32_t) primPaths.size()},
        });
    }

    // Library entries first, so the draws below name only what the renderer has been told about.
    collectChanges(meshesUsed, materialsUsed, texturesUsed, changes);

    // Slots: a draw kept from the last extract keeps its slot, a new one takes the lowest free slot or a new one, a gone one's slot dies.
    std::set<std::string> present;
    for (const auto& draw : draws) {
        present.insert(draw.key);
    }
    for (auto it = slotOfDraw.begin(); it != slotOfDraw.end();) {
        if (present.contains(it->first)) {
            ++it;
            continue;
        }
        auto slot = it->second;
        drawn.meshInstances[slot] = RenderMeshInstance{.primFirst = false, .alive = false};
        changes.instances.emplace_back(slot, drawn.meshInstances[slot]);
        freeSlots.insert(slot);
        it = slotOfDraw.erase(it);
    }
    for (auto& draw : draws) {
        uint32_t slot = 0;
        if (auto it = slotOfDraw.find(draw.key); it != slotOfDraw.end()) {
            slot = it->second;
            if (drawn.meshInstances[slot] == draw.instance) {
                drawn.primInstances[draw.instance.prim].push_back(slot);
                continue;
            }
        } else if (!freeSlots.empty()) {
            slot = *freeSlots.begin();
            freeSlots.erase(freeSlots.begin());
            slotOfDraw[draw.key] = slot;
        } else {
            slot = (uint32_t) drawn.meshInstances.size();
            drawn.meshInstances.emplace_back();
            slotOfDraw[draw.key] = slot;
        }
        drawn.meshInstances[slot] = draw.instance;
        changes.instances.emplace_back(slot, draw.instance);
        drawn.primInstances[draw.instance.prim].push_back(slot);
    }
    changes.slotCount = (uint32_t) drawn.meshInstances.size();
    stats.extractSlotsMs = msSince(slotsStart);
    // What this extract built, shared by every snapshot until the next one.
    auto built = std::make_shared<SceneDraws>();
    built->drawn = drawn;
    built->primPaths = std::move(primPaths);
    built->drawsByAsset = std::move(drawsByAsset);
    built->assetsByPrim = std::move(assetsByPrim);
    built->cameras = std::move(authoredCameras);
    // Roots whose drawn level changed since the last extract: one event for all of them.
    uint32_t switchedFiner = 0;
    uint32_t switchedCoarser = 0;
    std::string firstSwitched;
    std::unordered_map<std::string, uint32_t> drawnLevelsNow;
    for (const auto& root : drawnLodRoots) {
        if (!root.drawn) {
            continue;
        }
        drawnLevelsNow[root.path] = *root.drawn;
        auto before = previousDrawnLevels.find(root.path);
        if (before == previousDrawnLevels.end() || before->second == *root.drawn) {
            continue;
        }
        if (*root.drawn < before->second) {
            switchedFiner++;
        } else {
            switchedCoarser++;
        }
        if (firstSwitched.empty()) {
            firstSwitched = std::format("{} {} -> {}", root.path, before->second, *root.drawn);
        }
    }
    previousDrawnLevels = std::move(drawnLevelsNow);
    if (switchedFiner + switchedCoarser > 0) {
        TRACE_EVENT("Scene", "LevelsSwitched", id)
            .text(std::format("{} roots draw another level: {} finer, {} coarser (first: {})", switchedFiner + switchedCoarser, switchedFiner, switchedCoarser, firstSwitched))
            .field("finer", (int64_t) switchedFiner)
            .field("coarser", (int64_t) switchedCoarser);
    }
    built->lodRoots = std::move(drawnLodRoots);
    built->payloads = std::move(reachedPayloads);
    this->draws = std::move(built);
    if (drawn.lights != previousLights) {
        changes.lightsChanged = true;
        changes.lights = drawn.lights;
    }
    addChanges(std::move(changes));

    inputsRevision++;
    auto ms = msSince(start);
    stats.extracts++;
    stats.lastExtractMs = ms;
    TRACE_EVENT("Scene", "WorldExtracted", id)
        .text(std::format("draws built: {} instances of {} meshes, {} materials, {} textures, {} lights in {:.0f} ms", draws.size(), meshesUsed.size(), materialsUsed.size(), texturesUsed.size(), drawn.lights.size(), ms))
        .field("instances", (int64_t) draws.size())
        .field("meshes", (int64_t) meshesUsed.size())
        .field("materials", (int64_t) materialsUsed.size())
        .field("textures", (int64_t) texturesUsed.size())
        .field("lights", (int64_t) drawn.lights.size())
        .field("ms", ms);
}

auto ViewScene::collectChanges(const std::set<uint32_t>& meshesNow, const std::set<uint32_t>& materialsNow, const std::set<uint32_t>& texturesNow, SceneChanges& changes) -> void {
    // What stopped being drawn is kept (in the libraries and on the GPU) for the unload delay, so turning back doesn't upload it again;
    // after it, it leaves the libraries and the renderer, and a mesh's pack is released (asked for again if wanted later).
    auto delay = std::chrono::milliseconds((int64_t) (unloadDelaySeconds * 1000.0));
    auto keep = [&](const std::set<uint32_t>& used, const std::set<uint32_t>& drawnNow, std::unordered_map<uint32_t, std::chrono::steady_clock::time_point>& unusedSince) {
        std::set<uint32_t> kept = drawnNow;
        for (auto index : used) {
            if (drawnNow.contains(index)) {
                unusedSince.erase(index);
                continue;
            }
            auto [since, added] = unusedSince.try_emplace(index, std::chrono::steady_clock::now());
            if (std::chrono::steady_clock::now() - since->second < delay) {
                kept.insert(index);
            } else {
                unusedSince.erase(since);
            }
        }
        for (auto index : drawnNow) {
            unusedSince.erase(index);
        }
        return kept;
    };
    auto meshesKept = keep(usedMeshes, meshesNow, meshUnusedSince);
    auto materialsKept = keep(usedMaterials, materialsNow, materialUnusedSince);
    auto texturesKept = keep(usedTextures, texturesNow, textureUnusedSince);
    // The earliest moment something kept is due to go, so the scene thread wakes for it.
    releaseDue.reset();
    for (const auto* waiting : {&meshUnusedSince, &materialUnusedSince, &textureUnusedSince}) {
        for (const auto& [index, since] : *waiting) {
            if (!releaseDue || since + delay < *releaseDue) {
                releaseDue = since + delay;
            }
        }
    }

    for (auto index : texturesKept) {
        if (!usedTextures.contains(index)) {
            changes.texturesAdded.emplace_back(TextureHandle{index}, textureLibrary.shared(TextureHandle{index}));
        }
    }
    for (auto index : meshesKept) {
        if (!usedMeshes.contains(index)) {
            changes.meshesAdded.emplace_back(MeshHandle{index}, meshLibrary.shared(MeshHandle{index}));
        }
    }
    for (auto index : materialsKept) {
        if (!usedMaterials.contains(index)) {
            changes.materialsChanged.emplace_back(MaterialHandle{index}, *materialLibrary.get(MaterialHandle{index}));
        }
    }
    // What isn't kept any more leaves the libraries; a handle is never reused, so the renderer can't confuse it with a later entry.
    for (auto index : usedMaterials) {
        if (!materialsKept.contains(index)) {
            changes.materialsRemoved.push_back(MaterialHandle{index});
        }
    }
    for (auto index : usedMeshes) {
        if (!meshesKept.contains(index)) {
            changes.meshesRemoved.push_back(MeshHandle{index});
        }
    }
    for (auto index : usedTextures) {
        if (!texturesKept.contains(index)) {
            changes.texturesRemoved.push_back(TextureHandle{index});
        }
    }
    // Entries made this extract but not used (a material replaced within it) and entries not kept are dropped; a dropped mesh's pack too.
    std::vector<MeshHandle> dropMeshes;
    meshLibrary.each([&](MeshHandle handle, const MeshDesc&) {
        if (!meshesKept.contains(handle.index)) {
            dropMeshes.push_back(handle);
        }
    });
    for (auto handle : dropMeshes) {
        auto asset = meshLibrary.key(handle);
        meshLibrary.remove(handle);
        meshBoundsOf.erase(handle.index);
        if (!meshLibrary.find(asset) && !meshesLoaded.contains(asset)) {
            if (auto pack = meshPacks.find(asset); pack != meshPacks.end()) {
                releaseMeshPack(pack->second);
                meshPacks.erase(pack);
            }
        }
    }
    std::vector<MaterialHandle> dropMaterials;
    materialLibrary.each([&](MaterialHandle handle, const MaterialDesc&) {
        if (!materialsKept.contains(handle.index)) {
            dropMaterials.push_back(handle);
        }
    });
    for (auto handle : dropMaterials) {
        materialLibrary.remove(handle);
    }
    std::vector<TextureHandle> dropTextures;
    textureLibrary.each([&](TextureHandle handle, const TextureDesc&) {
        if (!texturesKept.contains(handle.index)) {
            dropTextures.push_back(handle);
        }
    });
    for (auto handle : dropTextures) {
        textureLibrary.remove(handle);
    }
    usedMeshes = meshesKept;
    usedMaterials = materialsKept;
    usedTextures = texturesKept;
}
