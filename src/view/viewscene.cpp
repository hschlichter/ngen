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
            }
        } else if (kind && *kind == ScenePackKind::Material) {
            if (auto material = readMaterialPack(asset->bytes)) {
                materialPacks[assetId] = std::move(*material);
            }
        } else {
            texturePacks[assetId] = std::move(asset->bytes);
        }
    }
    // The meshes and materials of a repacked file that weren't repacked by id are fetched again by the rebuild.
    auto fetchAgain = [&](const std::string& assetId) {
        return !repackedIds.contains(assetId) && repackedFiles.contains(assetId.substr(0, assetId.find('#')));
    };
    std::erase_if(meshPacks, [&](const auto& entry) { return fetchAgain(entry.first); });
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
    for (const auto& [prim, range] : drawn.primToInstance) {
        if (instance >= range.first && instance < range.first + range.count) {
            return primPath(prim);
        }
    }
    return {};
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
    if (auto it = drawn.primToInstance.find(prim); it != drawn.primToInstance.end()) {
        for (uint32_t i = it->second.first; i < it->second.first + it->second.count; i++) {
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
    for (const auto& [prim, range] : drawn.primToInstance) {
        if (!under(primPath(prim))) {
            continue;
        }
        for (uint32_t i = range.first; i < range.first + range.count; i++) {
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
    for (const auto& [prim, range] : drawn.primToInstance) {
        const auto& primPathOf = primPaths[prim - 1];
        if (!isUnder(primPathOf)) {
            continue;
        }
        auto world = unitScale * transforms.at(primPathOf).world;
        for (uint32_t i = range.first; i < range.first + range.count; i++) {
            auto& instance = drawn.meshInstances[i];
            instance.worldTransform = world;
            instance.worldBounds = meshLibrary->bounds(instance.mesh).transformed(world);
        }
    }
    for (auto& light : drawn.lights) {
        const auto& lightPath = primPaths[light.primHandle.index - 1];
        if (isUnder(lightPath)) {
            if (auto lit = transforms.find(lightPath); lit != transforms.end()) {
                light.worldTransform = unitScale * lit->second.world;
            }
        }
    }
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
        texturePacks[assetId] = std::move(bytes);
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

// A material: by its asset id, once; with no material bound, one per display colour, shared (a scene of many unbound prims would
// otherwise fill the renderer's texture slots with 1x1 textures of the same few colours). A bound material that authors neither a
// texture nor a colour takes the display colour of the first mesh that asks for it, as it always has.
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
        bool textured = false;
        if (auto bytes = texturePacks.find(pack->second.baseColorTexture); bytes != texturePacks.end()) {
            if (auto texture = readPackedTexture(bytes->second)) {
                desc.texWidth = (int) texture->width;
                desc.texHeight = (int) texture->height;
                desc.texMipLevels = texture->mipLevels;
                const auto* levels = reinterpret_cast<const uint8_t*>(texture->levels.data());
                desc.texPixels.assign(levels, levels + texture->levels.size());
                textured = true;
            }
        }
        if (!textured && pack->second.hasBaseColor) {
            const auto& c = pack->second.baseColor;
            desc.baseColorFactor = glm::vec4(c[0], c[1], c[2], 1.0f);
        }
        const auto& material = pack->second;
        if (material.opacityThreshold > 0.0f) {
            desc.opacityThreshold = material.opacityThreshold;
            desc.opacity = material.opacity;
            desc.opacityChannel = material.opacityChannel;
            desc.opacityRaw = material.opacityRaw;
            if (!material.opacityTexture.empty() && material.opacityTexture == material.baseColorTexture && textured) {
                desc.opacityFromBaseColor = true;
            } else if (auto bytes = texturePacks.find(material.opacityTexture); bytes != texturePacks.end()) {
                if (auto texture = readPackedTexture(bytes->second)) {
                    desc.opacityWidth = (int) texture->width;
                    desc.opacityHeight = (int) texture->height;
                    desc.opacityMipLevels = texture->mipLevels;
                    const auto* levels = reinterpret_cast<const uint8_t*>(texture->levels.data());
                    desc.opacityPixels.assign(levels, levels + texture->levels.size());
                }
            }
        }
    }
    if (desc.texPixels.empty() && desc.baseColorFactor == glm::vec4(1.0f) && displayColor) {
        desc.baseColorFactor = glm::vec4((*displayColor)[0], (*displayColor)[1], (*displayColor)[2], 1.0f);
    }
    // No texture: the constant colour as a 1x1 texel, so it takes the same sampling path as a texture.
    if (desc.texPixels.empty()) {
        desc.texWidth = 1;
        desc.texHeight = 1;
        desc.texPixels = {encodeSrgb(desc.baseColorFactor.r), encodeSrgb(desc.baseColorFactor.g), encodeSrgb(desc.baseColorFactor.b), 255};
    }
    auto materials = std::const_pointer_cast<MaterialLibrary>(materialLibrary);
    auto handle = materials->add(std::move(desc));
    materialByAsset[key] = handle;
    return handle;
}

// The renderer's inputs from the World: a draw instance per submesh of every drawn mesh, and the distant lights. World transforms are
// computed as the view always has: in float, parent times local, then scaled to metres by the scene's metersPerUnit.
auto ViewScene::extract() -> void {
    auto start = std::chrono::steady_clock::now();
    drawn.clear();
    primPaths.clear();
    materialByAsset.clear();
    transforms.clear();
    drawsByAsset.clear();
    assetsByPrim.clear();
    auto meshes = std::make_shared<MeshLibrary>();
    meshLibrary = meshes;
    materialLibrary = std::make_shared<MaterialLibrary>();
    const auto& w = world();
    unitScale = glm::mat4(1.0f);
    if (w.metersPerUnit != 1.0) {
        unitScale = glm::scale(glm::mat4(1.0f), glm::vec3((float) w.metersPerUnit));
    }

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
        auto pack = meshPacks.find(entity.meshAsset);
        if (pack == meshPacks.end() || pack->second.submeshes.empty()) {
            return true;
        }
        const auto& meshPack = pack->second;
        std::optional<std::array<float, 3>> displayColor;
        if (meshPack.hasDisplayColor) {
            displayColor = meshPack.displayColor;
        }

        MeshDesc desc;
        desc.vertices.reserve(meshPack.vertices.size());
        for (const auto& vertex : meshPack.vertices) {
            desc.vertices.push_back(toVertex(vertex));
        }
        desc.indices = meshPack.indices;
        desc.doubleSided = meshPack.doubleSided;
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
        for (const auto& submesh : meshPack.submeshes) {
            SubMesh out = {.indexOffset = submesh.indexOffset, .indexCount = submesh.indexCount};
            if (submesh.subsetPath.empty()) {
                out.material = material(visited.materialAsset, displayColor);
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
                out.material = material(subsetMaterial, std::nullopt);
                useMaterial(subsetMaterial);
            }
            desc.submeshes.push_back(out);
        }
        auto meshHandle = meshes->add(std::move(desc));
        const auto* meshData = meshes->get(meshHandle);
        auto worldBounds = meshes->bounds(meshHandle).transformed(worldTransform);

        primPaths.push_back(visited.path);
        auto prim = (uint32_t) primPaths.size();
        auto first = (uint32_t) drawn.meshInstances.size();
        for (size_t s = 0; s < meshData->submeshes.size(); s++) {
            const auto& sub = meshData->submeshes[s];
            drawn.meshInstances.push_back({
                .mesh = meshHandle,
                .material = sub.material,
                .worldTransform = worldTransform,
                .worldBounds = worldBounds,
                .indexOffset = sub.indexOffset,
                .indexCount = sub.indexCount,
                .primFirst = (s == 0),
                .doubleSided = meshData->doubleSided,
            });
        }
        drawn.primToInstance[prim] = {.first = first, .count = (uint32_t) meshData->submeshes.size()};
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
    inputsRevision++;
    auto ms = msSince(start);
    TRACE_EVENT("Scene", "WorldExtracted", id)
        .text(std::format("draws built: {} instances of {} meshes, {} materials, {} lights in {:.0f} ms", drawn.meshInstances.size(), meshLibrary->count(), materialLibrary->count(), drawn.lights.size(), ms))
        .field("instances", (int64_t) drawn.meshInstances.size())
        .field("meshes", (int64_t) meshLibrary->count())
        .field("materials", (int64_t) materialLibrary->count())
        .field("lights", (int64_t) drawn.lights.size())
        .field("ms", ms);
}
