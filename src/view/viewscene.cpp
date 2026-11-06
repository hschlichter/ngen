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
    desc->format = texture->format == PackedTextureFormat::Rgb9e5 ? TextureFormat::Rgb9e5 : TextureFormat::Rgba8Srgb;
    desc->levels.assign(texture->levels.begin(), texture->levels.end());
    if (texture->irradiance) {
        std::array<glm::vec3, 9> irradiance;
        for (size_t i = 0; i < irradiance.size(); i++) {
            irradiance[i] = glm::vec3((*texture->irradiance)[i * 3], (*texture->irradiance)[(i * 3) + 1], (*texture->irradiance)[(i * 3) + 2]);
        }
        desc->irradiance = irradiance;
    }
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
    post(HeuristicRequest{.on = on});
}

auto ViewScene::setLodPreference(LodPreference preference) -> void {
    post(HeuristicRequest{.on = std::nullopt, .prefer = preference});
}

auto ViewScene::resetStreamingCounters() -> void {
    post(StreamingResetRequest{});
}

auto ViewScene::setLodTuning(std::optional<float> bias, std::optional<float> hysteresis, std::optional<bool> blending) -> void {
    post(TuningRequest{.bias = bias, .hysteresis = hysteresis, .blending = blending});
}

auto ViewScene::overrideLodThresholds(const std::string& rootPath, std::optional<uint32_t> heuristic, LodOverride thresholds) -> void {
    post(TuningRequest{.path = rootPath, .heuristic = heuristic, .thresholds = std::move(thresholds)});
}

auto ViewScene::resetLodOverrides(const std::string& rootPath) -> void {
    post(TuningRequest{.path = rootPath, .reset = true});
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
            wake.wait(lock, stop, woken);
            if (stop.stop_requested()) {
                return;
            }
            toFree = std::move(retired);
            retired.clear();
            // Woken only to free what the main thread let go of: no batch, so nothing is published (which would hand back another).
            if (requests.empty() && !answered && !postedCamera.has_value()) {
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
        measureStreaming();
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
        refreshLodGroups(lod->root);
        placeNeeded = true;
        evaluateNeeded = true;
    } else if (auto* heuristic = std::get_if<HeuristicRequest>(&request)) {
        if (heuristic->on) {
            heuristicOn = *heuristic->on;
            refreshLodGroups({});
            placeNeeded = true;
        }
        if (heuristic->prefer && *heuristic->prefer != lodPreferenceNow) {
            lodPreferenceNow = *heuristic->prefer;
            TRACE_EVENT("Scene", "LodPreferenceChanged", id).text(std::format("LOD roots measure by {} first", lodPreferenceName(lodPreferenceNow))).field("prefer", std::string(lodPreferenceName(lodPreferenceNow)));
            refreshLodGroups({});
            placeNeeded = true;
        }
        evaluateNeeded = true;
    } else if (std::holds_alternative<StreamingResetRequest>(request)) {
        auto roots = std::move(streamingNow.roots);
        streamingNow = {};
        streamingNow.roots = std::move(roots);
        releasedOf.clear();
        // A root late now counts from the reset.
        auto now = std::chrono::steady_clock::now();
        for (auto& [path, times] : rootTimes) {
            if (times.lateSince) {
                times.lateSince = now;
            }
        }
        streamingChanged = true;
        TRACE_EVENT("Scene", "StreamingCountersReset", id).text("streaming counters reset");
    } else if (auto* tuningRequest = std::get_if<TuningRequest>(&request)) {
        applyTuning(*tuningRequest);
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
    streamingStates.clear();
    residentSets.clear();
    streamingNow = {};
    rootTimes.clear();
    releasedOf.clear();
    streamingChanged = true;
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
        if (!refreshed && !meshNames.contains(assetId)) {
            // Asked for by a level no longer resident: dropped by the pass that finds no placement naming it.
            streamingNow.arrivedUnwanted++;
            streamingChanged = true;
        }
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

auto lodWindowArea(float fovY, float aspect) -> float {
    float windowHeight = 2.0f * std::tan(fovY * 0.5f);
    return windowHeight * windowHeight * aspect;
}

auto lodMeasure(const LodHeuristic& heuristic, const glm::mat4& rootToWorld, double metersPerUnit, const glm::vec3& eye, float windowArea) -> float {
    if (heuristic.kind == LodHeuristicKind::Distance) {
        auto target = heuristic.center;
        if (heuristic.box) {
            auto eyeRoot = glm::vec3(glm::inverse(rootToWorld) * glm::vec4(eye, 1.0f));
            const auto& box = *heuristic.box;
            if (glm::all(glm::greaterThanEqual(eyeRoot, box[0])) && glm::all(glm::lessThanEqual(eyeRoot, box[1]))) {
                return 0.0f;
            }
            target = glm::clamp(eyeRoot, box[0], box[1]);
        }
        auto world = glm::vec3(rootToWorld * glm::vec4(target, 1.0f));
        return (float) (glm::length(world - eye) / metersPerUnit);
    }
    auto worldMin = glm::vec3(rootToWorld * glm::vec4(heuristic.extent[0], 1.0f));
    auto worldMax = glm::vec3(rootToWorld * glm::vec4(heuristic.extent[1], 1.0f));
    auto radius = glm::length(worldMax - worldMin) * 0.5f;
    auto center = glm::vec3(rootToWorld * glm::vec4((heuristic.extent[0] + heuristic.extent[1]) * 0.5f, 1.0f));
    auto distance = glm::length(eye - center);
    if (distance <= 1e-6f) {
        return 1e30f;
    }
    float r = radius / distance;
    return glm::pi<float>() * r * r / windowArea;
}

auto lodOverrideOf(const LodTuning& tuning, const std::string& rootKey, uint32_t heuristic) -> const LodOverride* {
    auto root = tuning.overrides.find(rootKey);
    if (root == tuning.overrides.end()) {
        return nullptr;
    }
    auto found = root->second.find(heuristic);
    if (found == root->second.end()) {
        return nullptr;
    }
    return &found->second;
}

auto tunedLodHeuristic(const LodHeuristic& heuristic, const LodOverride* override, const LodTuning& tuning) -> LodHeuristic {
    auto tuned = heuristic;
    if (override != nullptr) {
        if (override->thresholds) {
            tuned.thresholds = *override->thresholds;
        }
        if (override->blendThresholds) {
            tuned.blendThresholds = *override->blendThresholds;
        }
        if (override->unloadThresholds) {
            tuned.unloadThresholds = *override->unloadThresholds;
        }
    }
    if (!tuning.blending) {
        tuned.blendThresholds.clear();
    }
    if (tuning.bias != 1.0f) {
        // As if `bias` times closer: a distance's thresholds reached `bias` times farther, a screen size's at a size `bias` squared smaller.
        float scale = heuristic.kind == LodHeuristicKind::Distance ? tuning.bias : 1.0f / (tuning.bias * tuning.bias);
        for (auto& threshold : tuned.thresholds) {
            threshold *= scale;
        }
        for (auto& threshold : tuned.blendThresholds) {
            threshold *= scale;
        }
        for (auto& threshold : tuned.unloadThresholds) {
            threshold *= scale;
        }
    }
    return tuned;
}

auto ViewScene::authoredRootOf(const std::string& path) const -> std::optional<std::pair<std::string, const std::vector<LodHeuristic>*>> {
    if (auto group = lodGroupByPath.find(path); group != lodGroupByPath.end()) {
        const auto& info = lodGroupInfo[group->second];
        return std::pair{info.key, &info.heuristics};
    }
    if (auto placed = placedRoots.find(path); placed != placedRoots.end()) {
        return std::pair{placed->second.row.key, &placed->second.row.heuristics};
    }
    return std::nullopt;
}

namespace {

// Whether thresholds go the way UsdLod reads them: a distance's ascending, a screen size's descending.
auto lodThresholdsOrdered(LodHeuristicKind kind, const std::vector<float>& thresholds) -> bool {
    for (size_t t = 1; t < thresholds.size(); t++) {
        bool ordered = kind == LodHeuristicKind::Distance ? thresholds[t] >= thresholds[t - 1] : thresholds[t] <= thresholds[t - 1];
        if (!ordered) {
            return false;
        }
    }
    return true;
}

} // namespace

auto ViewScene::applyTuning(const TuningRequest& request) -> void {
    auto next = std::make_shared<LodTuning>(*tuning);
    if (request.bias) {
        next->bias = std::max(0.01f, *request.bias);
    }
    if (request.hysteresis) {
        next->hysteresis = std::clamp(*request.hysteresis, 0.0f, 0.9f);
    }
    if (request.blending) {
        next->blending = *request.blending;
    }
    std::string changedRoot;
    if (request.reset && request.path.empty()) {
        next->overrides.clear();
    } else if (request.reset || request.thresholds) {
        auto root = authoredRootOf(request.path);
        if (!root) {
            TRACE_WARNING("Scene", "LodTuningRefused", request.path).text(std::format("{}: no LOD root placed at this path, nothing to override", request.path));
        } else if (request.reset) {
            next->overrides.erase(root->first);
            changedRoot = root->first;
        } else {
            const auto& heuristics = *root->second;
            // Unload radii alone name the streaming heuristic when no heuristic is given; the rest the imaging one.
            bool unloadOnly = request.thresholds->unloadThresholds && !request.thresholds->thresholds && !request.thresholds->blendThresholds;
            auto domain = unloadOnly ? LodDomain::Streaming : LodDomain::Imaging;
            auto index = request.heuristic ? (int32_t) *request.heuristic : chooseLodHeuristic(heuristics, domain, lodPreferenceNow);
            if (index < 0 || (size_t) index >= heuristics.size()) {
                TRACE_WARNING("Scene", "LodTuningRefused", request.path).text(std::format("{}: no heuristic {} to override ({} authored)", request.path, index, heuristics.size()));
            } else {
                auto kind = heuristics[(size_t) index].kind;
                const auto& asked = *request.thresholds;
                // Unload radii: one per load radius in force, ordered, on its coarser side.
                const auto* current = lodOverrideOf(*next, root->first, (uint32_t) index);
                auto load = asked.thresholds ? *asked.thresholds : current != nullptr && current->thresholds ? *current->thresholds
                                                                                                             : heuristics[(size_t) index].thresholds;
                bool unloadFits = true;
                if (asked.unloadThresholds && !asked.unloadThresholds->empty()) {
                    unloadFits = asked.unloadThresholds->size() == load.size() && lodThresholdsOrdered(kind, *asked.unloadThresholds);
                    for (size_t k = 0; unloadFits && k < load.size(); k++) {
                        unloadFits = kind == LodHeuristicKind::Distance ? (*asked.unloadThresholds)[k] >= load[k] : (*asked.unloadThresholds)[k] <= load[k];
                    }
                }
                if ((asked.thresholds && !lodThresholdsOrdered(kind, *asked.thresholds)) || (asked.blendThresholds && !lodThresholdsOrdered(kind, *asked.blendThresholds))) {
                    TRACE_WARNING("Scene", "LodTuningRefused", request.path)
                        .text(std::format("{}: thresholds must {} as UsdLod reads them; nothing changed", request.path, kind == LodHeuristicKind::Distance ? "ascend" : "descend"));
                } else if (!unloadFits) {
                    TRACE_WARNING("Scene", "LodTuningRefused", request.path)
                        .text(std::format("{}: unload thresholds must be one per load threshold ({}), in order, each at or past its load threshold; nothing changed", request.path, load.size()));
                } else {
                    auto& entry = next->overrides[root->first][(uint32_t) index];
                    if (asked.thresholds) {
                        entry.thresholds = asked.thresholds;
                    }
                    if (asked.blendThresholds) {
                        entry.blendThresholds = asked.blendThresholds;
                    }
                    if (asked.unloadThresholds) {
                        entry.unloadThresholds = asked.unloadThresholds;
                    }
                    changedRoot = root->first;
                }
            }
        }
    }
    if (*next == *tuning) {
        return;
    }
    tuning = std::move(next);
    TRACE_EVENT("Scene", "LodTuningChanged", id)
        .text(std::format("LOD tuning: bias {:.2f}, hysteresis {:.0f}%, blending {}, {} authored roots overridden{}",
                          tuning->bias,
                          tuning->hysteresis * 100.0f,
                          tuning->blending ? "on" : "off",
                          tuning->overrides.size(),
                          changedRoot.empty() ? std::string() : std::format(" ({} changed)", changedRoot)))
        .field("bias", (double) tuning->bias)
        .field("hysteresis", (double) tuning->hysteresis)
        .field("blending", tuning->blending)
        .field("overrides", (int64_t) tuning->overrides.size())
        .field("root", changedRoot);
    refreshLodGroups({});
    placeNeeded = true;
    evaluateNeeded = true;
}

auto ViewScene::evaluateLevels() -> bool {
    if (!composer) {
        return false;
    }
    auto start = std::chrono::steady_clock::now();
    auto next = std::make_shared<SceneLodChoices>();
    uint32_t evaluated = 0;
    uint32_t finer = 0;
    uint32_t coarser = 0;
    uint32_t requested = 0;
    uint32_t releasedNow = 0;
    uint32_t findings = 0;
    bool changed = false;
    // A root no longer placed forgets its state, and the composer its resident levels: placed again, it starts afresh.
    for (auto it = residentSets.begin(); it != residentSets.end();) {
        if (placedRoots.contains(it->first)) {
            ++it;
            continue;
        }
        composer->resetResidentLevels(it->first);
        streamingStates.erase(it->first);
        it = residentSets.erase(it);
    }
    for (auto it = streamingStates.begin(); it != streamingStates.end();) {
        it = placedRoots.contains(it->first) ? std::next(it) : streamingStates.erase(it);
    }
    auto metersPerUnit = composer->world().metersPerUnit;
    auto windowArea = camera ? lodWindowArea(camera->fovY, camera->aspect) : 1.0f;
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
        } else if (choice.heuristic = chooseLodHeuristic(root.heuristics, LodDomain::Imaging, lodPreferenceNow); choice.heuristic < 0) {
            choice.level = root.defaultIndex;
            choice.reason = "no usable imaging heuristic";
        } else if (!camera) {
            choice.level = root.defaultIndex;
            choice.reason = "no camera";
        } else {
            auto override = lodOverrideOf(*tuning, root.key, (uint32_t) choice.heuristic);
            auto heuristic = tunedLodHeuristic(root.heuristics[(size_t) choice.heuristic], override, *tuning);
            choice.kind = heuristic.kind;
            choice.measure = lodMeasure(heuristic, root.worldTransform, metersPerUnit, camera->position, windowArea);
            auto previous = heuristicLevels.contains(root.path) ? heuristicLevels[root.path] : root.defaultIndex;
            // A streaming root draws one level: inside a band it wants the finer one.
            auto pick = pickLodLevel(heuristic.kind, heuristic.thresholds, heuristic.blendThresholds, choice.measure, levelCount, previous, tuning->hysteresis);
            choice.level = pick.level;
            heuristicLevels[root.path] = choice.level;
            choice.reason = heuristic.kind == LodHeuristicKind::Distance ? "distance" : "screen size";
        }

        // Residency: the streaming state's level and every coarser one. The state comes from the streaming heuristic's radii, with the
        // previous state as its hysteresis; without one, from the level picked to draw. A level set by hand is resident too, as the tool asks
        // for it. Nothing else: a level picked finer than the state isn't loaded early, it draws late.
        std::vector<LodHeuristic> tunedHeuristics;
        tunedHeuristics.reserve(root.heuristics.size());
        for (size_t i = 0; i < root.heuristics.size(); i++) {
            tunedHeuristics.push_back(tunedLodHeuristic(root.heuristics[i], lodOverrideOf(*tuning, root.key, (uint32_t) i), *tuning));
        }
        uint32_t from = choice.level;
        choice.streamingHeuristic = chooseLodHeuristic(root.heuristics, LodDomain::Streaming, lodPreferenceNow);
        if (!heuristicOn) {
            choice.residentReason = "heuristic off: the level drawn and every coarser one";
        } else if (choice.streamingHeuristic < 0) {
            choice.residentReason = "no usable streaming heuristic: the imaging pick and every coarser level";
        } else if (!camera) {
            choice.residentReason = "no camera: the level drawn and every coarser one";
        } else {
            const auto& streaming = tunedHeuristics[(size_t) choice.streamingHeuristic];
            choice.streamingMeasure = lodMeasure(streaming, root.worldTransform, metersPerUnit, camera->position, windowArea);
            auto previous = streamingStates.find(root.path);
            auto state = streamingState(streaming.kind,
                                        streaming.thresholds,
                                        streaming.unloadThresholds,
                                        choice.streamingMeasure,
                                        levelCount,
                                        previous != streamingStates.end() ? std::optional<uint32_t>(previous->second) : std::nullopt);
            streamingStates[root.path] = state;
            from = state;
            choice.residentReason = "streaming radii";
            if (handLevels.contains(root.path) && choice.level < from) {
                from = choice.level;
                choice.residentReason = "streaming radii, and the level set by hand";
            }
        }
        choice.state = from;
        std::set<uint32_t> resident;
        for (uint32_t level = from; level < levelCount; level++) {
            resident.insert(level);
            choice.resident.push_back(level);
        }

        // The authoring contract, against the streaming root nearest above it (its parent cell).
        const std::vector<LodHeuristic>* parentHeuristics = nullptr;
        std::vector<LodHeuristic> parentTuned;
        for (auto slash = root.path.rfind('/'); slash != std::string::npos && slash > 0; slash = root.path.rfind('/', slash - 1)) {
            auto above = placedRoots.find(root.path.substr(0, slash));
            if (above == placedRoots.end()) {
                continue;
            }
            const auto& parentRow = above->second.row;
            for (size_t i = 0; i < parentRow.heuristics.size(); i++) {
                parentTuned.push_back(tunedLodHeuristic(parentRow.heuristics[i], lodOverrideOf(*tuning, parentRow.key, (uint32_t) i), *tuning));
            }
            parentHeuristics = &parentTuned;
            break;
        }
        choice.findings = checkLodAuthoring(tunedHeuristics, levelCount, parentHeuristics);
        findings += (uint32_t) choice.findings.size();
        evaluated++;

        // The composer: the level drawn, held only where it isn't the root's default, and the resident levels.
        bool held = choice.level != root.defaultIndex || handLevels.contains(root.path);
        auto pushed = composerLevels.find(root.path);
        uint32_t before = pushed != composerLevels.end() ? pushed->second : root.defaultIndex;
        if (held && (pushed == composerLevels.end() || pushed->second != choice.level)) {
            composer->setLodLevel(root.path, choice.level);
            composerLevels[root.path] = choice.level;
            rootsToCheck.insert(root.path);
            changed = true;
        } else if (!held && pushed != composerLevels.end()) {
            composer->resetLodLevel(root.path);
            composerLevels.erase(pushed);
            rootsToCheck.insert(root.path);
            changed = true;
        }
        auto& residentBefore = residentSets[root.path];
        if (resident != residentBefore) {
            // A root placed for the first time has the composer's default loaded: its default level and its coarsest.
            std::set<uint32_t> loadedBefore = residentBefore;
            if (loadedBefore.empty()) {
                loadedBefore = {std::min(root.defaultIndex, levelCount - 1), levelCount - 1};
            }
            auto& released = releasedOf[root.path];
            for (auto level : resident) {
                if (!loadedBefore.contains(level)) {
                    requested++;
                    streamingNow.loads++;
                    if (released.contains(level)) {
                        streamingNow.reloads++;
                        streamingNow.roots[root.path].reloads++;
                    }
                }
            }
            for (auto level : loadedBefore) {
                if (!resident.contains(level)) {
                    releasedNow++;
                    streamingNow.released++;
                    stats.releasedLevels++;
                    released.insert(level);
                }
            }
            composer->setResidentLevels(root.path, resident);
            residentBefore = std::move(resident);
            rootsToCheck.insert(root.path);
            streamingChanged = true;
            changed = true;
        }
        if (choice.level < before) {
            finer++;
        } else if (choice.level > before) {
            coarser++;
        }
        (*next)[root.path] = std::move(choice);
    }
    if (streamingNow.findings != findings) {
        streamingNow.findings = findings;
        streamingChanged = true;
    }
    stats.lodRootsEvaluated = evaluated;
    stats.lodFiner = finer;
    stats.lodCoarser = coarser;
    stats.lodEvaluations++;
    stats.lodEvaluateMs = msSince(start);
    choices = std::move(next);
    if (!changed) {
        return false;
    }
    TRACE_EVENT("Scene", "ResidencyChanged", id)
        .text(std::format("LOD levels: {} roots finer, {} coarser of {} evaluated; {} levels requested, {} released", finer, coarser, stats.lodRootsEvaluated, requested, releasedNow))
        .field("finer", (int64_t) finer)
        .field("coarser", (int64_t) coarser)
        .field("roots", (int64_t) stats.lodRootsEvaluated)
        .field("requested", (int64_t) requested)
        .field("released", (int64_t) releasedNow);
    return true;
}

// Measures what streaming did, from the placed roots' rows as the last pass left them: when each level became ready, the lead of each switch
// of the selected level, and the time drawn coarser than selected or nothing. It measures; nothing here changes what is loaded or drawn.
auto ViewScene::measureStreaming() -> void {
    auto now = std::chrono::steady_clock::now();
    uint32_t lateNow = 0;
    uint32_t holesNow = 0;
    for (auto it = rootTimes.begin(); it != rootTimes.end();) {
        if (placedRoots.contains(it->first)) {
            ++it;
            continue;
        }
        if (it->second.lateSince) {
            streamingNow.lateSeconds += std::chrono::duration<double>(now - *it->second.lateSince).count();
        }
        streamingNow.roots.erase(it->first);
        it = rootTimes.erase(it);
        streamingChanged = true;
    }
    for (const auto& [path, placed] : placedRoots) {
        const auto& row = placed.row;
        auto levelCount = row.levels.size();
        auto& times = rootTimes[path];
        auto& measured = streamingNow.roots[path];
        times.readySince.resize(levelCount);
        if (measured.ready.size() != levelCount) {
            measured.ready.assign(levelCount, false);
            measured.lead.assign(levelCount, -1.0);
            streamingChanged = true;
        }
        for (size_t level = 0; level < levelCount; level++) {
            bool ready = level < row.levelWaits.size() && row.levelWaits[level] && row.levelWaits[level]->total() == 0;
            if (ready && !times.readySince[level]) {
                times.readySince[level] = now;
            } else if (!ready) {
                times.readySince[level].reset();
            }
            if (measured.ready[level] != ready) {
                measured.ready[level] = ready;
                streamingChanged = true;
            }
        }
        // A switch of the level picked to draw: its lead, 0 when the level isn't ready (a late switch).
        auto selected = row.wanted;
        if (times.selected && *times.selected != selected && selected < levelCount) {
            double lead = 0.0;
            if (times.readySince[selected]) {
                lead = std::chrono::duration<double>(now - *times.readySince[selected]).count();
            } else {
                streamingNow.lateSwitches++;
            }
            measured.lead[selected] = lead;
            streamingNow.leads.push_back(lead);
            streamingChanged = true;
        }
        times.selected = selected;
        bool hole = !row.drawn;
        bool late = row.drawn && *row.drawn > selected;
        if (hole || late) {
            if (!times.lateSince) {
                times.lateSince = now;
            }
        } else if (times.lateSince) {
            streamingNow.lateSeconds += std::chrono::duration<double>(now - *times.lateSince).count();
            times.lateSince.reset();
            streamingChanged = true;
        }
        if (hole && !measured.hole) {
            streamingNow.holes++;
        }
        if (measured.late != late || measured.hole != hole) {
            measured.late = late;
            measured.hole = hole;
            streamingChanged = true;
        }
        lateNow += late ? 1 : 0;
        holesNow += hole ? 1 : 0;
    }
    if (streamingNow.lateNow != lateNow || streamingNow.holesNow != holesNow) {
        streamingNow.lateNow = lateNow;
        streamingNow.holesNow = holesNow;
        streamingChanged = true;
    }
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
        .text(std::format("unloaded {} levels (out of their roots' resident levels), {} composed contents no level reaches, {} mesh packs ({:.1f} MiB)", levels, contents, meshes, (double) bytes / (1024.0 * 1024.0)))
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
    snapshot->lodPreference = lodPreferenceNow;
    snapshot->lodTuning = tuning;
    // Shared with the snapshot before unless they changed (the same for the layer stack and the World's LOD levels below).
    const auto* before = published.get();
    if (before != nullptr && *before->lodHandLevels == handLevels) {
        snapshot->lodHandLevels = before->lodHandLevels;
    } else {
        snapshot->lodHandLevels = std::make_shared<std::unordered_map<std::string, uint32_t>>(handLevels);
    }
    snapshot->metersPerUnitOfLod = composer ? composer->world().metersPerUnit : 1.0;
    if (streamingChanged) {
        streamingChanged = false;
        auto published = std::make_shared<SceneStreaming>(streamingNow);
        // Roots late now count up to the publish.
        auto now = std::chrono::steady_clock::now();
        for (const auto& [path, times] : rootTimes) {
            if (times.lateSince) {
                published->lateSeconds += std::chrono::duration<double>(now - *times.lateSince).count();
            }
        }
        streamingPublished = std::move(published);
    }
    snapshot->streaming = streamingPublished;
    snapshot->thread = stats;
    snapshot->environment.reset();
    if (!domesByPrim.empty()) {
        const auto& [prim, dome] = *domesByPrim.begin();
        SceneEnvironment environment{
            .prim = primPaths[prim - 1],
            .texture = dome.light.texture,
            .textureFormat = dome.light.textureFormat,
            .poleAxis = dome.light.poleAxis,
            .color = dome.light.color,
            .intensity = dome.light.intensity,
            .exposure = dome.light.exposure,
            .placed = drawn.environment,
            .domesIgnored = (uint32_t) domesWarned.size(),
        };
        if (auto pack = texturePacks.find(dome.light.texture); pack != texturePacks.end()) {
            environment.imageHeld = pack->second->format == TextureFormat::Rgb9e5 && pack->second->irradiance.has_value();
            environment.width = pack->second->width;
            environment.height = pack->second->height;
        }
        snapshot->environment = std::move(environment);
    }
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
        assetIdList = std::make_shared<std::vector<std::string>>(ids.begin(), ids.end());
    }
    snapshot->assetIds = assetIdList;
    if (composer) {
        const auto& world = composer->world();
        if (before != nullptr && *before->layerStack == world.layerStack) {
            snapshot->layerStack = before->layerStack;
        } else {
            snapshot->layerStack = std::make_shared<std::vector<std::string>>(world.layerStack);
        }
        snapshot->upAxis = world.upAxis;
        snapshot->metersPerUnit = world.metersPerUnit;
        snapshot->prims = worldPrims;
        snapshot->prototypes = world.prototypeCount();
        if (before != nullptr && *before->lodLevels == world.lodLevels) {
            snapshot->lodLevels = before->lodLevels;
        } else {
            snapshot->lodLevels = std::make_shared<std::unordered_map<std::string, uint32_t>>(world.lodLevels);
        }
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
    // The root's bounds: its first screen-size heuristic's extent, else a distance heuristic's box, else its center.
    std::array<glm::vec3, 2> bounds = {glm::vec3(0.0f), glm::vec3(0.0f)};
    bool found = false;
    for (const auto& heuristic : root.heuristics) {
        if (heuristic.kind == LodHeuristicKind::ScreenSize) {
            bounds = heuristic.extent;
            found = true;
            break;
        }
    }
    for (size_t i = 0; i < root.heuristics.size() && !found; i++) {
        const auto& heuristic = root.heuristics[i];
        if (heuristic.kind == LodHeuristicKind::Distance) {
            bounds = heuristic.box.value_or(std::array<glm::vec3, 2>{heuristic.center, heuristic.center});
            found = true;
        }
    }
    auto worldMin = glm::vec3(root.worldTransform * glm::vec4(bounds[0], 1.0f));
    auto worldMax = glm::vec3(root.worldTransform * glm::vec4(bounds[1], 1.0f));
    auto radius = glm::length(worldMax - worldMin) * 0.5f;
    auto center = glm::vec3(root.worldTransform * glm::vec4((bounds[0] + bounds[1]) * 0.5f, 1.0f));
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

namespace viewconfig {

config::Bool purposeGuide{"scene.purposes.guide", false, "Load and draw prims of purpose guide (helpers: bounds, markers) beside the default ones."};
config::Bool purposeProxy{"scene.purposes.proxy", false, "Load and draw prims of purpose proxy (stand-ins for heavy geometry) beside the default ones."};

} // namespace viewconfig
