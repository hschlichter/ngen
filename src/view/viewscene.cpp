#include "viewscene.h"

#include "assetclient.h"
#include "packedtexture.h"
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

auto pathDepth(const std::string& path) -> size_t {
    return (size_t) std::ranges::count(path, '/');
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

// Fetches ids from the asset server and hands each to `arrived`, or records it failed. Blocks.
template <typename Arrived>
auto fetch(AssetClient& assets, const std::vector<std::string>& ids, std::set<std::string>& failed, Arrived arrived) -> void {
    if (ids.empty()) {
        return;
    }
    assets.request(ids);
    assets.wait(ids);
    for (const auto& id : ids) {
        auto asset = assets.take(id);
        if (!asset) {
            failed.insert(id);
            std::string reason;
            for (const auto& line : assets.errors(id)) {
                reason += reason.empty() ? line : "; " + line;
            }
            TRACE_WARNING("Scene", "SceneAssetFailed", id).text(std::format("{} failed: {}", id, reason.empty() ? std::string("no reason given") : reason));
            continue;
        }
        arrived(id, std::move(asset->bytes));
    }
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

ViewScene::ViewScene(AssetClient& client)
    : assets(client) {
}

auto ViewScene::open(const std::string& sceneId) -> bool {
    // Every draw of the scene before goes: its slots die and are reused, lowest first, so a scene opened draws in visit order.
    for (auto& [key, slot] : slotOfDraw) {
        drawn.meshInstances[slot] = RenderMeshInstance{.primFirst = false, .alive = false};
        pending.instances.emplace_back(slot, drawn.meshInstances[slot]);
        freeSlots.insert(slot);
    }
    slotOfDraw.clear();
    id = sceneId;
    composer = std::make_unique<WorldComposer>(sceneId);
    requestedPacks.clear();
    meshPacks.clear();
    materialPacks.clear();
    texturePacks.clear();
    failedAssets.clear();
    TRACE_EVENT("Scene", "SceneRequested", sceneId.empty() ? "(empty scene)" : sceneId).text(sceneId.empty() ? std::string("an empty scene") : std::format("scene {} requested", sceneId));
    if (!sceneId.empty()) {
        compose();
        if (composer->world().layerStack.empty()) {
            TRACE_ERROR("Scene", "SceneFailed", sceneId).text(std::format("scene {}: its root layer can't be had", sceneId));
            return false;
        }
    }
    fetchAssets();
    extract();
    return true;
}

auto ViewScene::setLodLevel(const std::string& rootPath, uint32_t level) -> void {
    composer->setLodLevel(rootPath, level);
    rebuild();
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
    rebuild();
}

auto ViewScene::purposeShown(Purpose purpose) const -> bool {
    return shownPurposes.contains(purpose);
}

auto ViewScene::drawsPurpose(Purpose purpose) const -> bool {
    return shownPurposes.contains(purpose);
}

auto ViewScene::rebuild() -> void {
    if (!id.empty()) {
        compose();
    }
    fetchAssets();
    extract();
}

auto ViewScene::assetIds() const -> std::vector<std::string> {
    std::set<std::string> ids(requestedPacks.begin(), requestedPacks.end());
    for (const auto& [assetId, pack] : meshPacks) {
        ids.insert(assetId);
    }
    for (const auto& [assetId, pack] : materialPacks) {
        ids.insert(assetId);
    }
    for (const auto& [assetId, bytes] : texturePacks) {
        ids.insert(assetId);
    }
    ids.insert(failedAssets.begin(), failedAssets.end());
    return {ids.begin(), ids.end()};
}

auto ViewScene::refresh(const std::vector<std::string>& requested, bool force) -> size_t {
    auto start = std::chrono::steady_clock::now();
    auto ours = [this](const std::string& assetId) {
        return requestedPacks.contains(assetId) || meshPacks.contains(assetId) || materialPacks.contains(assetId) || texturePacks.contains(assetId) ||
               failedAssets.contains(assetId);
    };
    // Without force only the scene's own: another user of the client (the pipeline registry) fetches its assets itself.
    std::vector<std::string> ids;
    for (const auto& assetId : requested) {
        if (force || ours(assetId)) {
            ids.push_back(assetId);
        }
    }
    if (ids.empty()) {
        return 0;
    }
    size_t sceneAssets = (size_t) std::ranges::count_if(ids, ours);
    assets.request(ids, force);
    assets.wait(ids);
    std::set<std::string> repackedIds(ids.begin(), ids.end());
    std::set<std::string> repackedFiles;
    size_t failed = 0;
    for (const auto& assetId : ids) {
        if (!ours(assetId)) {
            // Repacked on the server; left in the client for whoever uses it.
            continue;
        }
        auto asset = assets.take(assetId);
        if (!asset) {
            failed++;
            std::string reason;
            for (const auto& line : assets.errors(assetId)) {
                reason += reason.empty() ? line : "; " + line;
            }
            TRACE_WARNING("Scene", "SceneAssetFailed", assetId).text(std::format("{}: {} failed, the scene keeps what it had: {}", assetId, force ? "repack" : "refresh", reason.empty() ? std::string("no reason given") : reason));
            continue;
        }
        failedAssets.erase(assetId);
        repackedFiles.insert(assetId.substr(0, assetId.find('#')));
        auto kind = scenePackKind(asset->bytes);
        if (kind && *kind == ScenePackKind::Layer) {
            if (auto error = composer->addPack(assetId, asset->bytes)) {
                TRACE_WARNING("Scene", "SceneAssetFailed", assetId).text(*error);
            }
        } else if (kind && *kind == ScenePackKind::Mesh) {
            if (auto mesh = readMeshPack(asset->bytes)) {
                meshPacks[assetId] = std::move(*mesh);
                // New content: a new library entry, so the renderer uploads it.
                meshLibrary.forget(assetId);
            }
        } else if (kind && *kind == ScenePackKind::Material) {
            if (auto material = readMaterialPack(asset->bytes)) {
                materialPacks[assetId] = std::move(*material);
            }
        } else if (auto desc = textureDesc(asset->bytes)) {
            texturePacks[assetId] = std::move(desc);
            textureLibrary.forget(assetId);
        }
    }
    // The meshes and materials of a repacked file that weren't repacked by id are fetched again by the rebuild.
    auto fetchAgain = [&](const std::string& assetId) {
        return !repackedIds.contains(assetId) && repackedFiles.contains(assetId.substr(0, assetId.find('#')));
    };
    std::erase_if(meshPacks, [&](const auto& entry) {
        if (!fetchAgain(entry.first)) {
            return false;
        }
        meshLibrary.forget(entry.first);
        return true;
    });
    std::erase_if(materialPacks, [&](const auto& entry) { return fetchAgain(entry.first); });
    if (sceneAssets > 0) {
        rebuild();
    }
    auto ms = msSince(start);
    TRACE_EVENT("Scene", "AssetsRefreshed", id)
        .text(std::format("{} assets {} ({} of the scene's, {} failed) and the scene rebuilt in {:.0f} ms", ids.size(), force ? "repacked" : "changed at their source fetched again", sceneAssets, failed, ms))
        .field("forced", force)
        .field("assets", (int64_t) ids.size())
        .field("scene_assets", (int64_t) sceneAssets)
        .field("failed", (int64_t) failed)
        .field("ms", ms);
    return sceneAssets;
}

auto ViewScene::worldUp() const -> glm::vec3 {
    if (world().upAxis == "Z") {
        return {0.0f, 0.0f, 1.0f};
    }
    return {0.0f, 1.0f, 0.0f};
}

auto ViewScene::primPath(uint32_t prim) const -> std::string {
    if (prim == 0 || prim > primPaths.size()) {
        return {};
    }
    return primPaths[prim - 1];
}

auto ViewScene::instancePath(uint32_t instance) const -> std::string {
    if (instance >= drawn.meshInstances.size() || !drawn.meshInstances[instance].alive) {
        return {};
    }
    return primPath(drawn.meshInstances[instance].prim);
}

auto ViewScene::drawsOf(const std::string& assetId) const -> std::vector<uint32_t> {
    if (auto it = drawsByAsset.find(assetId); it != drawsByAsset.end()) {
        return it->second;
    }
    return {};
}

auto ViewScene::drawCount(const std::string& assetId) const -> size_t {
    if (auto it = drawsByAsset.find(assetId); it != drawsByAsset.end()) {
        return it->second.size();
    }
    return 0;
}

auto ViewScene::assetsUnder(const std::string& path) const -> std::vector<std::string> {
    std::set<std::string> out;
    for (const auto& [prim, uses] : assetsByPrim) {
        const auto& candidate = primPaths[prim - 1];
        bool under = path == "/" || candidate == path || (candidate.size() > path.size() && candidate.starts_with(path) && candidate[path.size()] == '/');
        if (under) {
            out.insert(uses.begin(), uses.end());
        }
    }
    return {out.begin(), out.end()};
}

auto ViewScene::primBounds(uint32_t prim) const -> AABB {
    AABB out = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
    if (auto it = drawn.primInstances.find(prim); it != drawn.primInstances.end()) {
        for (auto i : it->second) {
            const auto& bounds = drawn.meshInstances[i].worldBounds;
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
    for (const auto& [prim, slots] : drawn.primInstances) {
        if (!under(primPath(prim))) {
            continue;
        }
        for (auto i : slots) {
            const auto& bounds = drawn.meshInstances[i].worldBounds;
            if (bounds.valid()) {
                out.min = glm::min(out.min, bounds.min);
                out.max = glm::max(out.max, bounds.max);
            }
        }
    }
    return out;
}

auto ViewScene::transform(const std::string& path) const -> std::optional<PrimTransform> {
    auto it = transforms.find(path);
    if (it == transforms.end()) {
        return std::nullopt;
    }
    return PrimTransform{.local = it->second.local, .world = unitScale * it->second.world, .xformable = it->second.xformable};
}

// The prim's new world transform, and the change of it applied to every prim under it, so a drag doesn't rebuild the draws: each draw
// instance and light under the prim gets its new world transform and bounds.
auto ViewScene::setLocalTransform(const std::string& path, const glm::mat4& local) -> bool {
    auto it = transforms.find(path);
    if (it == transforms.end() || !it->second.xformable) {
        return false;
    }
    localOverrides[path] = local;
    auto& entry = it->second;
    glm::mat4 parentWorld = entry.world * glm::inverse(entry.local);
    glm::mat4 newWorld = entry.resetsXformStack ? local : parentWorld * local;
    glm::mat4 change = newWorld * glm::inverse(entry.world);
    entry.local = local;
    entry.world = newWorld;
    auto prefix = path + "/";
    for (auto under = transforms.lower_bound(prefix); under != transforms.end() && under->first.starts_with(prefix); ++under) {
        under->second.world = change * under->second.world;
    }
    auto isUnder = [&](const std::string& candidate) {
        return candidate == path || candidate.starts_with(prefix);
    };
    for (const auto& [prim, slots] : drawn.primInstances) {
        const auto& primPathOf = primPaths[prim - 1];
        if (!isUnder(primPathOf)) {
            continue;
        }
        auto world = unitScale * transforms.at(primPathOf).world;
        for (auto i : slots) {
            auto& instance = drawn.meshInstances[i];
            instance.worldTransform = world;
            instance.worldBounds = meshBoundsOf[instance.mesh.index].transformed(world);
            pending.instances.emplace_back(i, instance);
        }
    }
    for (auto& light : drawn.lights) {
        const auto& lightPath = primPaths[light.primHandle.index - 1];
        if (isUnder(lightPath)) {
            if (auto lit = transforms.find(lightPath); lit != transforms.end()) {
                light.worldTransform = unitScale * lit->second.world;
                pending.lightsChanged = true;
                pending.lights = drawn.lights;
            }
        }
    }
    pending.slotCount = (uint32_t) drawn.meshInstances.size();
    inputsRevision++;
    return true;
}

// Composes in rounds: each names the layer packs it needs next (sublayers, references, payloads), which are fetched for the next.
auto ViewScene::compose() -> void {
    auto start = std::chrono::steady_clock::now();
    size_t rounds = 0;
    for (;;) {
        std::vector<std::string> fresh;
        for (const auto& needed : composer->compose()) {
            if (requestedPacks.insert(needed).second) {
                fresh.push_back(needed);
            }
        }
        if (fresh.empty()) {
            break;
        }
        rounds++;
        std::set<std::string> failed;
        fetch(assets, fresh, failed, [this](const std::string& packId, std::vector<std::byte> bytes) {
            if (auto error = composer->addPack(packId, bytes)) {
                TRACE_WARNING("Scene", "SceneAssetFailed", packId).text(*error);
            }
        });
        for (const auto& packId : failed) {
            composer->markFailed(packId);
        }
    }
    for (const auto& message : composer->messages()) {
        TRACE_WARNING("Scene", "CompositionMessage", id).text(message);
    }
    size_t entities = 0;
    world().visit([&entities](const VisitedEntity&) {
        entities++;
        return true;
    });
    auto ms = msSince(start);
    TRACE_EVENT("Scene", "PacksComposed", id)
        .text(std::format("{} composed: {} layer packs in {} rounds, {} prims, {} prototypes in {:.0f} ms", id, requestedPacks.size(), rounds, entities, world().prototypes.size(), ms))
        .field("packs", (int64_t) requestedPacks.size())
        .field("rounds", (int64_t) rounds)
        .field("prims", (int64_t) entities)
        .field("prototypes", (int64_t) world().prototypes.size())
        .field("ms", ms);
}

// The meshes and materials of everything drawn, then the textures those materials name.
auto ViewScene::fetchAssets() -> void {
    auto start = std::chrono::steady_clock::now();
    std::set<std::string> meshIds;
    std::set<std::string> materialIds;
    world().visit([&](const VisitedEntity& visited) {
        const auto& entity = *visited.entity;
        if (!entity.active || !entity.defined || entity.abstract || visited.inUndrawnLevel || !visited.visible) {
            return false;
        }
        if (entity.meshAsset.empty() || !drawsPurpose(visited.purpose)) {
            return true;
        }
        meshIds.insert(entity.meshAsset);
        if (!visited.materialAsset.empty()) {
            materialIds.insert(visited.materialAsset);
        }
        // A subset's own binding: the subset entities are the mesh's children.
        for (EntityId child : entity.children) {
            const auto& subset = visited.content->entities[child];
            if (subset.binding && !subset.binding->materialAsset.empty()) {
                materialIds.insert(subset.binding->materialAsset);
            }
        }
        return true;
    });
    std::vector<std::string> wanted;
    for (const auto& meshId : meshIds) {
        if (!meshPacks.contains(meshId) && !failedAssets.contains(meshId)) {
            wanted.push_back(meshId);
        }
    }
    for (const auto& materialId : materialIds) {
        if (!materialPacks.contains(materialId) && !failedAssets.contains(materialId)) {
            wanted.push_back(materialId);
        }
    }
    size_t received = wanted.size();
    fetch(assets, wanted, failedAssets, [this](const std::string& assetId, std::vector<std::byte> bytes) {
        auto kind = scenePackKind(bytes);
        if (kind && *kind == ScenePackKind::Mesh) {
            if (auto mesh = readMeshPack(bytes)) {
                meshPacks[assetId] = std::move(*mesh);
                return;
            }
        } else if (kind && *kind == ScenePackKind::Material) {
            if (auto material = readMaterialPack(bytes)) {
                materialPacks[assetId] = std::move(*material);
                return;
            }
        }
        failedAssets.insert(assetId);
        TRACE_WARNING("Scene", "SceneAssetFailed", assetId).text(std::format("{} isn't a mesh or material pack", assetId));
    });

    std::vector<std::string> textures;
    for (const auto& materialId : materialIds) {
        auto it = materialPacks.find(materialId);
        if (it == materialPacks.end()) {
            continue;
        }
        for (const auto& texture : {it->second.baseColorTexture, it->second.opacityTexture}) {
            if (!texture.empty() && !texturePacks.contains(texture) && !failedAssets.contains(texture) && std::ranges::find(textures, texture) == textures.end()) {
                textures.push_back(texture);
            }
        }
    }
    received += textures.size();
    fetch(assets, textures, failedAssets, [this](const std::string& assetId, std::vector<std::byte> bytes) {
        if (auto desc = textureDesc(bytes)) {
            texturePacks[assetId] = std::move(desc);
            return;
        }
        failedAssets.insert(assetId);
        TRACE_WARNING("Scene", "SceneAssetFailed", assetId).text(std::format("{} isn't a packed texture", assetId));
    });
    auto ms = msSince(start);
    TRACE_EVENT("Scene", "SceneAssetsReceived", id)
        .text(std::format("{} meshes, {} materials and {} textures drawn; {} fetched in {:.0f} ms", meshIds.size(), materialIds.size(), textures.size(), received, ms))
        .field("meshes", (int64_t) meshIds.size())
        .field("materials", (int64_t) materialIds.size())
        .field("textures", (int64_t) textures.size())
        .field("fetched", (int64_t) received)
        .field("ms", ms);
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
    if (auto pack = materialPacks.find(materialAsset); pack != materialPacks.end()) {
        const auto& material = pack->second;
        desc.baseColorTexture = material.baseColorTexture.empty() ? TextureHandle{} : texture(material.baseColorTexture);
        if (!desc.baseColorTexture && material.hasBaseColor) {
            const auto& c = material.baseColor;
            desc.baseColorFactor = glm::vec4(c[0], c[1], c[2], 1.0f);
        }
        if (material.opacityThreshold > 0.0f) {
            desc.opacityThreshold = material.opacityThreshold;
            desc.opacity = material.opacity;
            desc.opacityChannel = material.opacityChannel;
            desc.opacityRaw = material.opacityRaw;
            if (!material.opacityTexture.empty() && material.opacityTexture == material.baseColorTexture && desc.baseColorTexture) {
                desc.opacityFromBaseColor = true;
            } else if (!material.opacityTexture.empty()) {
                desc.opacityTexture = texture(material.opacityTexture);
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

// The renderer's inputs from the World: a draw instance per submesh of every drawn mesh, and the distant lights. World transforms are
// computed as the view always has: in float, parent times local, then scaled to metres by the scene's metersPerUnit.
auto ViewScene::extract() -> void {
    auto start = std::chrono::steady_clock::now();
    auto previousLights = std::move(drawn.lights);
    drawn.lights.clear();
    drawn.primInstances.clear();
    primPaths.clear();
    materialByAsset.clear();
    transforms.clear();
    drawsByAsset.clear();
    assetsByPrim.clear();
    const auto& w = world();
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
    std::set<uint32_t> meshesUsed;
    std::set<uint32_t> materialsUsed;
    std::set<uint32_t> texturesUsed;

    // World transforms by depth: an entity's parent is the last entity visited one level up.
    std::vector<glm::mat4> worldByDepth = {glm::mat4(1.0f)};
    bool hasDistantLight = false;
    w.visit([&](const VisitedEntity& visited) {
        const auto& entity = *visited.entity;
        auto depth = pathDepth(visited.path);
        if (worldByDepth.size() <= depth) {
            worldByDepth.resize(depth + 1);
        }
        const auto& parentWorld = worldByDepth[depth - 1];
        glm::mat4 local(visited.entity->localTransform);
        if (auto authored = localOverrides.find(visited.path); authored != localOverrides.end() && entity.xformable) {
            local = authored->second;
        }
        if (!entity.xformable) {
            worldByDepth[depth] = parentWorld;
        } else if (entity.resetsXformStack) {
            worldByDepth[depth] = local;
        } else {
            worldByDepth[depth] = parentWorld * local;
        }
        if (!entity.active || !entity.defined || entity.abstract || visited.inUndrawnLevel || !visited.visible) {
            return false;
        }
        transforms[visited.path] = {.local = local, .world = worldByDepth[depth], .xformable = entity.xformable, .resetsXformStack = entity.resetsXformStack};
        auto worldTransform = unitScale * worldByDepth[depth];

        if (auto light = visited.content->lights.find(visited.id); light != visited.content->lights.end()) {
            if (light->second.shape == LightShape::Distant) {
                hasDistantLight = true;
                primPaths.push_back(visited.path);
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

        if (entity.meshAsset.empty() || !drawsPurpose(visited.purpose)) {
            return true;
        }
        auto meshHandle = mesh(entity.meshAsset);
        if (!meshHandle) {
            return true;
        }
        const auto& meshPack = meshPacks.at(entity.meshAsset);
        const auto* meshData = meshLibrary.get(meshHandle);
        std::optional<std::array<float, 3>> displayColor;
        if (meshPack.hasDisplayColor) {
            displayColor = meshPack.displayColor;
        }

        // The assets this draw uses, for drawsOf: its mesh, the file defining it, and each submesh's material and that material's texture.
        std::vector<std::string> uses = {entity.meshAsset, entity.meshAsset.substr(0, entity.meshAsset.find('#'))};
        auto useMaterial = [&](const std::string& materialAsset) {
            if (materialAsset.empty()) {
                return;
            }
            uses.push_back(materialAsset);
            if (auto pack = materialPacks.find(materialAsset); pack != materialPacks.end()) {
                for (const auto& texture : {pack->second.baseColorTexture, pack->second.opacityTexture}) {
                    if (!texture.empty()) {
                        uses.push_back(texture);
                    }
                }
            }
        };
        std::vector<MaterialHandle> submeshMaterials;
        for (const auto& submesh : meshPack.submeshes) {
            if (submesh.subsetPath.empty()) {
                submeshMaterials.push_back(material(visited.materialAsset, displayColor));
                useMaterial(visited.materialAsset);
            } else {
                // The subset's own binding, unless an ancestor's is stronger; a subset has no display colour of its own.
                auto name = submesh.subsetPath.substr(submesh.subsetPath.rfind('/') + 1);
                std::string subsetMaterial = visited.materialAsset;
                for (EntityId child : entity.children) {
                    const auto& subset = visited.content->entities[child];
                    if (subset.name == name && subset.binding && !visited.strongBinding) {
                        subsetMaterial = subset.binding->materialAsset;
                    }
                }
                submeshMaterials.push_back(material(subsetMaterial, std::nullopt));
                useMaterial(subsetMaterial);
            }
        }
        auto worldBounds = meshBoundsOf[meshHandle.index].transformed(worldTransform);

        primPaths.push_back(visited.path);
        auto prim = (uint32_t) primPaths.size();
        meshesUsed.insert(meshHandle.index);
        // A prim with any cutout submesh casts its shadows per submesh, so the cutout one is cut there too.
        bool primHasCutout = false;
        for (auto handle : submeshMaterials) {
            const auto* materialData = materialLibrary.get(handle);
            if (materialData != nullptr && materialData->cutout()) {
                primHasCutout = true;
            }
        }
        for (size_t s = 0; s < meshData->submeshes.size(); s++) {
            const auto& sub = meshData->submeshes[s];
            auto materialHandle = submeshMaterials[s];
            const auto* materialData = materialLibrary.get(materialHandle);
            materialsUsed.insert(materialHandle.index);
            if (materialData != nullptr) {
                for (auto texture : {materialData->baseColorTexture, materialData->opacityTexture}) {
                    if (texture) {
                        texturesUsed.insert(texture.index);
                    }
                }
            }
            draws.push_back({
                .key = std::format("{}#{}", visited.path, s),
                .instance = {
                    .mesh = meshHandle,
                    .material = materialHandle,
                    .prim = prim,
                    .worldTransform = worldTransform,
                    .worldBounds = worldBounds,
                    .indexOffset = sub.indexOffset,
                    .indexCount = sub.indexCount,
                    .primFirst = (s == 0),
                    .doubleSided = meshData->doubleSided,
                    .cutout = materialData != nullptr && materialData->cutout(),
                    .primHasCutout = primHasCutout,
                },
            });
        }
        std::ranges::sort(uses);
        auto [last, end] = std::ranges::unique(uses);
        uses.erase(last, end);
        for (const auto& used : uses) {
            drawsByAsset[used].push_back(prim);
        }
        assetsByPrim[prim] = std::move(uses);
        return true;
    });

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
    collectChanges(meshesUsed, materialsUsed, texturesUsed);

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
        pending.instances.emplace_back(slot, drawn.meshInstances[slot]);
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
        pending.instances.emplace_back(slot, draw.instance);
        drawn.primInstances[draw.instance.prim].push_back(slot);
    }
    pending.slotCount = (uint32_t) drawn.meshInstances.size();
    if (drawn.lights != previousLights) {
        pending.lightsChanged = true;
        pending.lights = drawn.lights;
    }

    inputsRevision++;
    auto ms = msSince(start);
    TRACE_EVENT("Scene", "WorldExtracted", id)
        .text(std::format("draws built: {} instances of {} meshes, {} materials, {} textures, {} lights in {:.0f} ms", draws.size(), meshesUsed.size(), materialsUsed.size(), texturesUsed.size(), drawn.lights.size(), ms))
        .field("instances", (int64_t) draws.size())
        .field("meshes", (int64_t) meshesUsed.size())
        .field("materials", (int64_t) materialsUsed.size())
        .field("textures", (int64_t) texturesUsed.size())
        .field("lights", (int64_t) drawn.lights.size())
        .field("ms", ms);
}

auto ViewScene::collectChanges(const std::set<uint32_t>& meshesNow, const std::set<uint32_t>& materialsNow, const std::set<uint32_t>& texturesNow) -> void {
    for (auto index : texturesNow) {
        if (!usedTextures.contains(index)) {
            pending.texturesAdded.emplace_back(TextureHandle{index}, textureLibrary.shared(TextureHandle{index}));
        }
    }
    for (auto index : meshesNow) {
        if (!usedMeshes.contains(index)) {
            pending.meshesAdded.emplace_back(MeshHandle{index}, meshLibrary.shared(MeshHandle{index}));
        }
    }
    for (auto index : materialsNow) {
        if (!usedMaterials.contains(index)) {
            pending.materialsChanged.emplace_back(MaterialHandle{index}, *materialLibrary.get(MaterialHandle{index}));
        }
    }
    // What isn't used any more leaves the libraries; a handle is never reused, so the renderer can't confuse it with a later entry.
    for (auto index : usedMaterials) {
        if (!materialsNow.contains(index)) {
            pending.materialsRemoved.push_back(MaterialHandle{index});
        }
    }
    for (auto index : usedMeshes) {
        if (!meshesNow.contains(index)) {
            pending.meshesRemoved.push_back(MeshHandle{index});
        }
    }
    for (auto index : usedTextures) {
        if (!texturesNow.contains(index)) {
            pending.texturesRemoved.push_back(TextureHandle{index});
        }
    }
    // Entries made this extract but not used (a material replaced within it) and entries no longer used are dropped.
    std::vector<MeshHandle> dropMeshes;
    meshLibrary.each([&](MeshHandle handle, const MeshDesc&) {
        if (!meshesNow.contains(handle.index)) {
            dropMeshes.push_back(handle);
        }
    });
    for (auto handle : dropMeshes) {
        meshLibrary.remove(handle);
        meshBoundsOf.erase(handle.index);
    }
    std::vector<MaterialHandle> dropMaterials;
    materialLibrary.each([&](MaterialHandle handle, const MaterialDesc&) {
        if (!materialsNow.contains(handle.index)) {
            dropMaterials.push_back(handle);
        }
    });
    for (auto handle : dropMaterials) {
        materialLibrary.remove(handle);
    }
    std::vector<TextureHandle> dropTextures;
    textureLibrary.each([&](TextureHandle handle, const TextureDesc&) {
        if (!texturesNow.contains(handle.index)) {
            dropTextures.push_back(handle);
        }
    });
    for (auto handle : dropTextures) {
        textureLibrary.remove(handle);
    }
    usedMeshes = meshesNow;
    usedMaterials = materialsNow;
    usedTextures = texturesNow;
}

auto ViewScene::takeChanges() -> SceneChanges {
    auto changes = std::move(pending);
    pending = {};
    pending.slotCount = (uint32_t) drawn.meshInstances.size();
    return changes;
}
