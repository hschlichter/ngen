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
#include <utility>

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
    if (!current->draws->placements) {
        return false;
    }
    auto found = findTransform(*current->draws->placements, path);
    if (!found || !found->xformable) {
        return false;
    }
    post(TransformRequest{.path = path, .local = local});
    return true;
}

auto ViewScene::update() -> bool {
    {
        std::lock_guard lock(mutex);
        if (published == current) {
            return false;
        }
        retired.push_back(std::move(current));
        current = published;
    }
    wake.notify_one();
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
    pending.lodGroupCount = changes.lodGroupCount;
    return changes;
}

auto ViewScene::addChanges(SceneChanges changes) -> void {
    std::lock_guard lock(mutex);
    pending.append(std::move(changes));
}

// The scene thread: requests in order, then what the asset server answered, then one compose and placement pass for the batch. Nothing waits
// but the loop itself, for the next request or answer.
auto ViewScene::run(const std::stop_token& stop) -> void {
    profile::registerThread("Scene");
    while (!stop.stop_requested()) {
        std::deque<Request> batch;
        bool answers = false;
        std::vector<std::shared_ptr<const SceneSnapshot>> toFree;
        {
            std::unique_lock lock(mutex);
            auto woken = [this] {
                return !requests.empty() || answered || postedCamera.has_value() || !retired.empty();
            };
            // A level kept for the unload delay: wake when its delay runs out, or sooner for a request or an answer.
            if (levelsDue) {
                wake.wait_until(lock, stop, *levelsDue, woken);
            } else {
                wake.wait(lock, stop, woken);
            }
            if (stop.stop_requested()) {
                return;
            }
            toFree = std::move(retired);
            retired.clear();
            // Woken only to free what the main thread let go of: no batch, so nothing is published (which would hand back another).
            bool due = levelsDue && std::chrono::steady_clock::now() >= *levelsDue;
            if (requests.empty() && !answered && !postedCamera.has_value() && !due) {
                lock.unlock();
                PROFILE_ZONE("SceneRetire");
                toFree.clear();
                continue;
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
        {
            PROFILE_ZONE("SceneRetire");
            toFree.clear();
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
        // Levels past their unload delay go: the composer links again without them (a compose does too) and their placements are released.
        if (levelsDue && std::chrono::steady_clock::now() >= *levelsDue && releaseLevels() && !composeNeeded) {
            relink();
        }
        if (composeNeeded) {
            compose();
        }
        if (placeNeeded) {
            place();
        }
        // Roots placed for the first time are evaluated; levels it changes are linked and placed, which may place new roots.
        while (evaluateNeeded) {
            evaluateNeeded = false;
            if (evaluateLevels()) {
                relink();
                place();
            }
        }
        updateLevelsDue();
        reportReleases();
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
        rootsToCheck.insert(lod->root);
        // A render-LOD root's group takes the level as its forced one.
        refreshForcedLevels(lod->root);
        placeNeeded = true;
        evaluateNeeded = true;
    } else if (auto* heuristic = std::get_if<HeuristicRequest>(&request)) {
        if (heuristic->on) {
            heuristicOn = *heuristic->on;
            refreshForcedLevels({});
            placeNeeded = true;
        }
        if (heuristic->unloadDelay) {
            // Applies to the levels kept now too: their delays run from when they stopped being wanted.
            unloadDelaySeconds = std::max(0.0, *heuristic->unloadDelay);
            keptLevelsChanged = true;
        }
        evaluateNeeded = true;
    } else if (auto* purpose = std::get_if<PurposeRequest>(&request)) {
        if (purpose->shown) {
            drawnPurposes.insert(purpose->purpose);
        } else {
            drawnPurposes.erase(purpose->purpose);
        }
        resetPlacements = true;
        placeNeeded = true;
    } else if (auto* refresh = std::get_if<RefreshRequest>(&request)) {
        applyRefresh(refresh->ids, refresh->force);
    } else if (auto* transform = std::get_if<TransformRequest>(&request)) {
        applyTransform(transform->path, transform->local);
    }
}

auto ViewScene::applyOpen(const std::string& sceneId) -> void {
    // The counters are per scene.
    stats = {};
    // Every placement of the scene before goes: its slots die and are reused, lowest first, so a scene opened draws in tree order.
    resetPlacements = true;
    walks.clear();
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
    levelsKept.clear();
    keptLevelsChanged = true;
    levelsDue.reset();
    reportedReleasedLevels = 0;
    reportedReleasedContents = 0;
    reportedReleasedMeshes = 0;
    reportedReleasedBytes = 0;
    openedAt = std::chrono::steady_clock::now();
    TRACE_EVENT("Scene", "SceneRequested", sceneId.empty() ? "(empty scene)" : sceneId).text(sceneId.empty() ? std::string("an empty scene") : std::format("scene {} requested", sceneId));
    composeNeeded = !sceneId.empty();
    placeNeeded = true;
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
        arrived.insert(assetId);
        placeNeeded = true;
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
                // New content: a new library entry, so the renderer uploads it; a repack is placed afresh.
                meshLibrary.forget(assetId);
                resetPlacements = resetPlacements || refreshed;
            }
        } else {
            failedAssets.insert(assetId);
        }
    } else if (packKind && *packKind == ScenePackKind::Material) {
        if (auto material = readMaterialPack(asset->bytes)) {
            materialPacks[assetId] = std::move(*material);
            resetPlacements = resetPlacements || (refreshed && !unchanged);
        } else {
            failedAssets.insert(assetId);
        }
    } else if (auto desc = textureDesc(asset->bytes)) {
        texturePacks[assetId] = std::move(desc);
        if (!unchanged) {
            textureLibrary.forget(assetId);
            resetPlacements = resetPlacements || refreshed;
        }
    } else {
        failedAssets.insert(assetId);
        TRACE_WARNING("Scene", "SceneAssetFailed", assetId).text(std::format("{} isn't a layer, mesh, material or texture pack", assetId));
    }
    // A placement waiting for it is built again.
    arrived.insert(assetId);
    placeNeeded = true;
}

// Composes with the layer packs held, gaps where one hasn't arrived, and asks for those it names and lacks.
auto ViewScene::compose() -> void {
    composeNeeded = false;
    placeNeeded = true;
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
    stats.releasedContents += composed.released;
    traceCompleted();
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
        .text(std::format("{} composed: {} layer packs, {} prims, {} prototypes, {:.0f} ms after it was requested", id, requestedPacks.size(), entities, world.prototypeCount(), msSince(openedAt)))
        .field("packs", (int64_t) requestedPacks.size())
        .field("prims", (int64_t) entities)
        .field("prototypes", (int64_t) world.prototypeCount())
        .field("ms", ms);
}

// The meshes and materials of everything drawn, and the textures of the materials held; each asked for once.
auto ViewScene::releaseMeshPack(const MeshPack& pack) -> void {
    stats.releasedMeshBytes += pack.vertices.size() * sizeof(PackVertex) + pack.indices.size() * sizeof(uint32_t);
    stats.releasedMeshes++;
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

// A gizmo's move: the view's local opinion of the prim (localOverrides), kept until the scene closes. The placements holding the prim are
// built again in this batch's pass, walking it afresh with the opinion, and what is drawn under it moves with them.
auto ViewScene::applyTransform(const std::string& path, const glm::mat4& local) -> void {
    auto found = tree ? findTransform(*tree, path) : std::nullopt;
    if (!found || !found->xformable) {
        return;
    }
    localOverrides[path] = local;
    overridesMoved.push_back(path);
    placeNeeded = true;
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
    for (const auto& [path, placed] : placedRoots) {
        const auto& root = placed.row;
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
        // The level it wanted before stays loaded for the unload delay (the coarsest always is); the one it wants now isn't kept, it is wanted.
        if (choice.level != before) {
            if (before + 1 < levelCount) {
                keepLevel(root.path, before, true);
            }
            keepLevel(root.path, choice.level, false);
        }
        if (held && (pushed == composerLevels.end() || pushed->second != choice.level)) {
            composer->setLodLevel(root.path, choice.level);
            composerLevels[root.path] = choice.level;
            rootsToCheck.insert(root.path);
        } else if (!held && pushed != composerLevels.end()) {
            composer->resetLodLevel(root.path);
            composerLevels.erase(pushed);
            rootsToCheck.insert(root.path);
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

// A level kept loaded for the unload delay from now, or no longer kept (wanted again, or let go); the composer is told at once.
auto ViewScene::keepLevel(const std::string& rootPath, uint32_t level, bool keep) -> void {
    auto& levels = levelsKept[rootPath];
    bool changed = false;
    if (keep) {
        changed = levels.insert_or_assign(level, std::chrono::steady_clock::now()).second;
    } else {
        changed = levels.erase(level) > 0;
    }
    if (!changed) {
        if (levels.empty()) {
            levelsKept.erase(rootPath);
        }
        return;
    }
    std::set<uint32_t> kept;
    for (const auto& [keptLevel, since] : levels) {
        kept.insert(keptLevel);
    }
    if (levels.empty()) {
        levelsKept.erase(rootPath);
    }
    if (composer) {
        composer->keepLodLevels(rootPath, kept);
    }
    keptLevelsChanged = true;
}

// The levels whose delay ran out go, but a level its root still draws (the level it wants isn't ready yet) is kept for another delay.
auto ViewScene::releaseLevels() -> bool {
    auto now = std::chrono::steady_clock::now();
    auto delay = std::chrono::milliseconds((int64_t) (unloadDelaySeconds * 1000.0));
    std::vector<std::pair<std::string, uint32_t>> due;
    for (auto& [path, levels] : levelsKept) {
        for (auto& [level, since] : levels) {
            if (now < since + delay) {
                continue;
            }
            auto placed = placedRoots.find(path);
            if (placed != placedRoots.end() && placed->second.row.drawn == level) {
                since = now;
                keptLevelsChanged = true;
                continue;
            }
            due.emplace_back(path, level);
        }
    }
    for (const auto& [path, level] : due) {
        keepLevel(path, level, false);
        rootsToCheck.insert(path);
        stats.releasedLevels++;
    }
    return !due.empty();
}

auto ViewScene::updateLevelsDue() -> void {
    auto delay = std::chrono::milliseconds((int64_t) (unloadDelaySeconds * 1000.0));
    levelsDue.reset();
    uint32_t count = 0;
    for (const auto& [path, levels] : levelsKept) {
        for (const auto& [level, since] : levels) {
            count++;
            if (!levelsDue || since + delay < *levelsDue) {
                levelsDue = since + delay;
            }
        }
    }
    stats.keptLevels = count;
}

auto ViewScene::reportReleases() -> void {
    auto levels = stats.releasedLevels - reportedReleasedLevels;
    auto contents = stats.releasedContents - reportedReleasedContents;
    auto meshes = stats.releasedMeshes - reportedReleasedMeshes;
    auto bytes = stats.releasedMeshBytes - reportedReleasedBytes;
    if (levels == 0 && contents == 0 && meshes == 0) {
        return;
    }
    reportedReleasedLevels = stats.releasedLevels;
    reportedReleasedContents = stats.releasedContents;
    reportedReleasedMeshes = stats.releasedMeshes;
    reportedReleasedBytes = stats.releasedMeshBytes;
    TRACE_EVENT("Scene", "LevelsUnloaded", id)
        .text(std::format("unloaded {} levels not wanted for {:g} s: {} composed contents, {} mesh packs ({:.1f} MiB)", levels, unloadDelaySeconds, contents, meshes, (double) bytes / (1024.0 * 1024.0)))
        .field("levels", (int64_t) levels)
        .field("contents", (int64_t) contents)
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
    stats.releasedContents += composer->stats().released;
    traceCompleted();
    requestAssets(fresh, false);
    stats.relinks++;
    countWorld();
    placeNeeded = true;
}

auto ViewScene::traceCompleted() -> void {
    for (const auto& completed : composer->stats().completed) {
        auto kind = completed.kind == ContentKind::Prototype ? "prototype" : "level";
        TRACE_EVENT("Scene", "ContentComposed", completed.path)
            .text(std::format("{} {} composed with every pack it names, in {} composes", kind, completed.path, completed.rounds))
            .field("kind", std::string(kind))
            .field("rounds", (int64_t) completed.rounds);
    }
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
            count += countPrims(world, world.contents[(size_t) entity.linkedContent], true);
            continue;
        }
        count++;
        if (entity.prototype >= 0) {
            count += countPrims(world, world.contents[(size_t) entity.prototype], false);
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
    snapshot->lodChoices = choices;
    snapshot->lodHeuristic = heuristicOn;
    snapshot->unloadDelay = unloadDelaySeconds;
    if (keptLevelsChanged) {
        keptLevelsChanged = false;
        auto delay = std::chrono::milliseconds((int64_t) (unloadDelaySeconds * 1000.0));
        auto kept = std::make_shared<std::map<std::string, std::map<uint32_t, std::chrono::steady_clock::time_point>>>();
        for (const auto& [path, levels] : levelsKept) {
            auto& out = (*kept)[path];
            for (const auto& [level, since] : levels) {
                out[level] = since + delay;
            }
        }
        keptPublished = std::move(kept);
    }
    snapshot->keptLevels = keptPublished;
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
    snapshot->thread.placeNeeded = placeNeeded;
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
        snapshot->prototypes = world.prototypeCount();
        snapshot->lodLevels = world.lodLevels;
    }
    snapshot->idle = inFlight.empty() && !composeNeeded && !placeNeeded;
    // The snapshot this replaces, when the main thread never took it, is freed here after the lock, not under it: update() waits on it.
    std::shared_ptr<const SceneSnapshot> replaced;
    {
        std::lock_guard lock(mutex);
        replaced = std::exchange(published, std::move(snapshot));
        working = false;
    }
    replaced.reset();
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
    const auto& byAsset = current->draws->drawsByAsset();
    if (auto it = byAsset.find(assetId); it != byAsset.end()) {
        return it->second;
    }
    return {};
}

auto ViewScene::drawCount(const std::string& assetId) const -> size_t {
    const auto& byAsset = current->draws->drawsByAsset();
    if (auto it = byAsset.find(assetId); it != byAsset.end()) {
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
    const auto& byPrim = current->draws->slotsByPrim();
    if (prim >= byPrim.size()) {
        return out;
    }
    for (auto i : byPrim[prim]) {
        const auto& bounds = current->draws->drawn.meshInstances[i].worldBounds;
        if (bounds.valid()) {
            out.min = glm::min(out.min, bounds.min);
            out.max = glm::max(out.max, bounds.max);
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
    for (const auto& instance : current->draws->drawn.meshInstances) {
        if (!instance.alive || !instance.worldBounds.valid() || !under(primPath(instance.prim))) {
            continue;
        }
        out.min = glm::min(out.min, instance.worldBounds.min);
        out.max = glm::max(out.max, instance.worldBounds.max);
    }
    return out;
}

auto ViewScene::transform(const std::string& path) const -> std::optional<PrimTransform> {
    if (!current->draws->placements) {
        return std::nullopt;
    }
    auto found = findTransform(*current->draws->placements, path);
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
// colour takes the display colour of the first mesh that asks for it in a pass, as it always has. The entry is kept while its
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

auto ViewScene::collectChanges(const std::set<uint32_t>& meshesNow, const std::set<uint32_t>& materialsNow, const std::set<uint32_t>& texturesNow, SceneChanges& changes) -> void {
    // What no placement uses any more leaves the libraries and the renderer at once (a level not wanted waited for the unload delay as a
    // level, its placement holding its entries), and a mesh's pack is released (asked for again if wanted later).
    const auto& meshesKept = meshesNow;
    const auto& materialsKept = materialsNow;
    const auto& texturesKept = texturesNow;

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
    // Entries made this pass but not used (a material replaced within it) and entries not kept are dropped; a dropped mesh's pack too.
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
        if (!meshLibrary.find(asset) && !meshNames.contains(asset)) {
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
