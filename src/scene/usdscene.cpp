#include "usdscene.h"
#include "material.h"
#include "mesh.h"

#include <pxr/base/tf/notice.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/notice.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdLux/boundableLightBase.h>
#include <pxr/usd/usdLux/nonboundableLightBase.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

PXR_NAMESPACE_USING_DIRECTIVE

// ── Change listener ──────────────────────────────────────────────────────────

class USDChangeListener : public TfWeakBase {
public:
    void initialize(const UsdStageRefPtr& stage) { m_key = TfNotice::Register(TfCreateWeakPtr(this), &USDChangeListener::onObjectsChanged, stage); }

    void shutdown() { TfNotice::Revoke(m_key); }

    void drain(std::vector<SdfPath>& outResynced, std::vector<SdfPath>& outChanged) {
        std::lock_guard lock(m_mutex);
        outResynced.swap(m_resynced);
        outChanged.swap(m_changed);
        m_resynced.clear();
        m_changed.clear();
    }

private:
    void onObjectsChanged(const UsdNotice::ObjectsChanged& notice, const UsdStageWeakPtr&) {
        std::lock_guard lock(m_mutex);
        for (const auto& path : notice.GetResyncedPaths()) {
            m_resynced.push_back(path);
        }
        for (const auto& path : notice.GetChangedInfoOnlyPaths()) {
            m_changed.push_back(path);
        }
    }

    TfNotice::Key m_key;
    std::mutex m_mutex;
    std::vector<SdfPath> m_resynced;
    std::vector<SdfPath> m_changed;
};

// ── Impl ─────────────────────────────────────────────────────────────────────

struct USDScene::Impl {
    UsdStageRefPtr stage;
    SdfLayerRefPtr rootLayer;
    SdfLayerRefPtr sessionLayerRef;

    USDChangeListener changeListener;
    uint32_t frame = 0;
    double metersPerUnit = 1.0;

    // Prim cache
    std::vector<PrimRuntimeRecord> prims;
    std::unordered_map<std::string, uint32_t> pathToIndex;

    // Transform cache (parallel to prims)
    std::vector<TransformCacheRecord> transforms;

    // Asset binding cache (parallel to prims)
    std::vector<AssetBindingCacheRecord> assetBindings;
    bool assetBindingsBuilt = false;

    // Layer info
    std::vector<SceneLayerInfo> layerInfos;
    LayerHandle editTarget;
    LayerHandle sessionLayerHandle;

    // Scratch buffers for change processing
    std::vector<SdfPath> scratchResynced;
    std::vector<SdfPath> scratchChanged;
    SceneDirtySet dirtySet;

    // ── Stage management ─────────────────────────────────────────────────

    bool open(const char* path) {
        stage = UsdStage::Open(path);
        if (!stage) {
            fprintf(stderr, "USDScene: failed to open stage: %s\n", path);
            return false;
        }

        rootLayer = stage->GetRootLayer();
        sessionLayerRef = stage->GetSessionLayer();
        metersPerUnit = UsdGeomGetStageMetersPerUnit(stage);

        changeListener.initialize(stage);
        rebuildLayerInfo();
        rebuildPrimCache();
        rebuildAllTransforms();

        printf("USDScene: opened %s (%zu prims)\n", path, prims.size());
        return true;
    }

    void close() {
        changeListener.shutdown();
        prims.clear();
        pathToIndex.clear();
        transforms.clear();
        assetBindings.clear();
        assetBindingsBuilt = false;
        layerInfos.clear();
        dirtySet.clear();
        stage = nullptr;
        rootLayer = nullptr;
        sessionLayerRef = nullptr;
        frame = 0;
    }

    // ── Layer info ───────────────────────────────────────────────────────

    void rebuildLayerInfo() {
        // Remember current edit target identifier so we can restore it
        std::string prevEditTargetId;
        for (const auto& info : layerInfos) {
            if (info.handle == editTarget) {
                prevEditTargetId = info.identifier;
                break;
            }
        }

        layerInfos.clear();
        uint32_t idx = 1;

        // Session layer
        {
            SceneLayerInfo info;
            info.handle = {.index = idx++};
            info.identifier = sessionLayerRef->GetIdentifier();
            info.displayName = "Session";
            info.role = SceneLayerRole::Session;
            info.dirty = sessionLayerRef->IsDirty();
            info.readOnly = false;
            info.muted = false;
            layerInfos.push_back(std::move(info));
            sessionLayerHandle = layerInfos.back().handle;
        }

        // Root layer
        LayerHandle rootHandle;
        {
            SceneLayerInfo info;
            info.handle = {.index = idx++};
            info.identifier = rootLayer->GetIdentifier();
            info.displayName = rootLayer->GetDisplayName().empty() ? rootLayer->GetIdentifier() : rootLayer->GetDisplayName();
            info.role = SceneLayerRole::Root;
            info.dirty = rootLayer->IsDirty();
            info.readOnly = rootLayer->GetFileFormat()->IsPackage();
            info.muted = false;
            layerInfos.push_back(std::move(info));
            rootHandle = layerInfos.back().handle;
        }

        // Sublayers of root
        for (const auto& subPath : rootLayer->GetSubLayerPaths()) {
            auto subLayer = SdfLayer::FindOrOpen(subPath);
            if (!subLayer) {
                continue;
            }

            SceneLayerInfo info;
            info.handle = {.index = idx++};
            info.identifier = subLayer->GetIdentifier();
            info.displayName = subLayer->GetDisplayName().empty() ? subLayer->GetIdentifier() : subLayer->GetDisplayName();
            info.role = SceneLayerRole::Unknown;
            info.dirty = subLayer->IsDirty();
            info.readOnly = subLayer->GetFileFormat()->IsPackage();
            info.muted = stage->IsLayerMuted(subLayer->GetIdentifier());
            layerInfos.push_back(std::move(info));
        }

        // Restore previous edit target, or fall back to root
        editTarget = rootHandle;
        if (!prevEditTargetId.empty()) {
            for (const auto& info : layerInfos) {
                if (info.identifier == prevEditTargetId) {
                    editTarget = info.handle;
                    break;
                }
            }
        }
    }

    SdfLayerRefPtr resolveLayer(LayerHandle h) const {
        if (!h) {
            return nullptr;
        }
        for (const auto& info : layerInfos) {
            if (info.handle == h) {
                if (info.role == SceneLayerRole::Session) {
                    return sessionLayerRef;
                }
                return SdfLayer::FindOrOpen(info.identifier);
            }
        }
        return nullptr;
    }

    // ── Prim cache ───────────────────────────────────────────────────────

    void rebuildPrimCache() {
        prims.clear();
        pathToIndex.clear();

        // Reserve index 0 as null
        prims.push_back({});

        auto range = UsdPrimRange::Stage(stage);
        for (const auto& prim : range) {
            auto pathStr = prim.GetPath().GetString();

            PrimRuntimeRecord rec;
            rec.handle = {.index = (uint32_t) prims.size()};
            rec.path = pathStr;
            rec.name = prim.GetName().GetString();
            rec.active = prim.IsActive();
            rec.loaded = prim.IsLoaded();
            rec.flags = classifyPrim(prim);

            UsdGeomImageable imageable(prim);
            if (imageable) {
                rec.visible = (imageable.ComputeVisibility() != UsdGeomTokens->invisible);
            }

            prims.push_back(std::move(rec));
            pathToIndex[pathStr] = prims.back().handle.index;
        }

        // Build hierarchy links
        for (size_t i = 1; i < prims.size(); i++) {
            auto& rec = prims[i];
            auto prim = stage->GetPrimAtPath(SdfPath(rec.path));
            if (!prim) {
                continue;
            }

            auto parentPrim = prim.GetParent();
            if (parentPrim && parentPrim.GetPath() != SdfPath::AbsoluteRootPath()) {
                auto it = pathToIndex.find(parentPrim.GetPath().GetString());
                if (it != pathToIndex.end()) {
                    rec.parent = {.index = it->second};

                    // Link as child
                    auto& parent = prims[it->second];
                    if (!parent.firstChild) {
                        parent.firstChild = rec.handle;
                    } else {
                        // Append as sibling
                        auto sibIdx = parent.firstChild.index;
                        while (prims[sibIdx].nextSibling) {
                            sibIdx = prims[sibIdx].nextSibling.index;
                        }
                        prims[sibIdx].nextSibling = rec.handle;
                    }
                }
            }
        }
    }

    static uint64_t classifyPrim(const UsdPrim& prim) {
        uint64_t flags = 0;
        if (prim.IsA<UsdGeomMesh>()) {
            flags |= PrimFlagRenderable;
        }
        if (prim.IsA<UsdLuxBoundableLightBase>() || prim.IsA<UsdLuxNonboundableLightBase>()) {
            flags |= PrimFlagLight;
        }
        if (prim.IsA<UsdGeomXformable>()) {
            flags |= PrimFlagXformable;
        }
        return flags;
    }

    // ── Transform cache ──────────────────────────────────────────────────

    void rebuildAllTransforms() {
        transforms.resize(prims.size());

        for (size_t i = 1; i < prims.size(); i++) {
            updateTransformForPrim(i);
        }

        // Apply metersPerUnit scale to all world transforms
        if (metersPerUnit != 1.0) {
            auto s = (float) metersPerUnit;
            auto scaleMat = glm::scale(glm::mat4(1.0f), glm::vec3(s));
            for (size_t i = 1; i < transforms.size(); i++) {
                transforms[i].world = scaleMat * transforms[i].world;
            }
        }
    }

    void updateTransformForPrim(size_t idx) {
        auto& rec = prims[idx];
        auto& xf = transforms[idx];

        auto prim = stage->GetPrimAtPath(SdfPath(rec.path));
        if (!prim) {
            return;
        }

        UsdGeomXformable xformable(prim);
        if (xformable) {
            bool resetsXformStack = false;
            GfMatrix4d localMat;
            xformable.GetLocalTransformation(&localMat, &resetsXformStack, UsdTimeCode::Default());

            // Copy USD local matrix into glm mat4
            glm::mat4 localGlm;
            for (int c = 0; c < 4; c++) {
                for (int r = 0; r < 4; r++) {
                    localGlm[c][r] = (float) localMat[c][r];
                }
            }

            // Decompose into position, rotation, scale
            xf.local.position = glm::vec3(localGlm[3]);
            glm::vec3 s;
            s.x = glm::length(glm::vec3(localGlm[0]));
            s.y = glm::length(glm::vec3(localGlm[1]));
            s.z = glm::length(glm::vec3(localGlm[2]));
            xf.local.scale = s;

            glm::mat3 rotMat(glm::vec3(localGlm[0]) / s.x, glm::vec3(localGlm[1]) / s.y, glm::vec3(localGlm[2]) / s.z);
            xf.local.rotation = glm::quat_cast(rotMat);

            if (rec.parent && !resetsXformStack) {
                xf.world = transforms[rec.parent.index].world * localGlm;
            } else {
                xf.world = localGlm;
            }
        } else {
            // Inherit parent world transform
            if (rec.parent) {
                xf.world = transforms[rec.parent.index].world;
            } else {
                xf.world = glm::mat4(1.0f);
            }
        }

        xf.lastFrame = frame;
    }

    void updateDirtyTransforms(std::span<const PrimHandle> dirty) {
        // Mark dirty prims and all descendants
        std::vector<bool> needsUpdate(prims.size(), false);
        for (auto h : dirty) {
            if (h.index < prims.size()) {
                markSubtreeDirty(h.index, needsUpdate);
            }
        }

        // Update in order (parents before children since prims are stored in traversal order)
        for (size_t i = 1; i < prims.size(); i++) {
            if (needsUpdate[i]) {
                updateTransformForPrim(i);
            }
        }
    }

    void markSubtreeDirty(uint32_t idx, std::vector<bool>& needsUpdate) {
        needsUpdate[idx] = true;
        auto child = prims[idx].firstChild;
        while (child) {
            markSubtreeDirty(child.index, needsUpdate);
            child = prims[child.index].nextSibling;
        }
    }

    // ── Change processing ────────────────────────────────────────────────

    void processChanges() {
        dirtySet.clear();
        changeListener.drain(scratchResynced, scratchChanged);

        if (scratchResynced.empty() && scratchChanged.empty()) {
            return;
        }

        // Structural resyncs — rebuild cache for now
        if (!scratchResynced.empty()) {
            rebuildPrimCache();
            rebuildAllTransforms();
            // Everything is dirty after a full rebuild
            return;
        }

        // Property changes — categorize
        for (const auto& path : scratchChanged) {
            auto primPath = path.GetPrimPath();
            auto it = pathToIndex.find(primPath.GetString());
            if (it == pathToIndex.end()) {
                continue;
            }

            PrimHandle h = {.index = it->second};
            auto propName = path.GetNameToken().GetString();

            if (propName.find("xformOp") != std::string::npos || propName == "xformOpOrder") {
                dirtySet.transformDirty.push_back(h);
            } else {
                dirtySet.assetsDirty.push_back(h);
            }
        }

        if (!dirtySet.transformDirty.empty()) {
            updateDirtyTransforms(dirtySet.transformDirty);
        }
    }

    // ── Asset binding cache ────────────────────────────────────────────────

    void updateAssetBindings(MeshLibrary& meshLib, MaterialLibrary& matLib) {
        assetBindings.resize(prims.size());

        for (size_t i = 1; i < prims.size(); i++) {
            auto& rec = prims[i];
            if (!(rec.flags & PrimFlagRenderable)) {
                continue;
            }
            if (assetBindingsBuilt && assetBindings[i].mesh) {
                continue;
            }

            auto prim = stage->GetPrimAtPath(SdfPath(rec.path));
            if (!prim) {
                continue;
            }

            assetBindings[i].mesh = extractMesh(prim, meshLib);
            assetBindings[i].material = extractMaterial(prim, matLib);
            assetBindings[i].revision++;
        }

        assetBindingsBuilt = true;
    }

    static MeshHandle extractMesh(const UsdPrim& prim, MeshLibrary& meshLib) {
        UsdGeomMesh geomMesh(prim);
        if (!geomMesh) {
            return {};
        }

        VtArray<GfVec3f> points;
        geomMesh.GetPointsAttr().Get(&points, UsdTimeCode::Default());
        if (points.empty()) {
            return {};
        }

        VtArray<int> faceVertexCounts;
        VtArray<int> faceVertexIndices;
        geomMesh.GetFaceVertexCountsAttr().Get(&faceVertexCounts, UsdTimeCode::Default());
        geomMesh.GetFaceVertexIndicesAttr().Get(&faceVertexIndices, UsdTimeCode::Default());

        // Get normals if available
        VtArray<GfVec3f> normals;
        geomMesh.GetNormalsAttr().Get(&normals, UsdTimeCode::Default());

        // Get UVs if available — try common primvar names
        VtArray<GfVec2f> uvs;
        UsdGeomPrimvarsAPI primvarsAPI(prim);
        for (const auto* name : {"st", "st0", "st1", "UVMap"}) {
            auto pv = primvarsAPI.GetPrimvar(TfToken(name));
            if (pv && pv.Get(&uvs, UsdTimeCode::Default()) && !uvs.empty()) {
                break;
            }
        }

        // Triangulate and build vertex/index arrays
        MeshDesc meshDesc;
        uint32_t fvIdx = 0;
        for (size_t f = 0; f < faceVertexCounts.size(); f++) {
            int count = faceVertexCounts[f];
            // Fan triangulation
            for (int t = 0; t < count - 2; t++) {
                int indices[3] = {
                    faceVertexIndices[fvIdx],
                    faceVertexIndices[fvIdx + t + 1],
                    faceVertexIndices[fvIdx + t + 2],
                };
                int fvIndices[3] = {
                    (int) fvIdx,
                    (int) (fvIdx + t + 1),
                    (int) (fvIdx + t + 2),
                };

                for (int v = 0; v < 3; v++) {
                    Vertex vert = {};
                    auto& p = points[indices[v]];
                    vert.position = {p[0], p[1], p[2]};

                    if (!normals.empty()) {
                        int ni = (normals.size() == points.size()) ? indices[v] : fvIndices[v];
                        if (ni < (int) normals.size()) {
                            auto& n = normals[ni];
                            vert.normal = {n[0], n[1], n[2]};
                        }
                    }

                    if (!uvs.empty()) {
                        int ui = (uvs.size() == points.size()) ? indices[v] : fvIndices[v];
                        if (ui < (int) uvs.size()) {
                            auto& uv = uvs[ui];
                            vert.texCoord = {uv[0], 1.0f - uv[1]};
                        }
                    }

                    vert.color = {1.0f, 1.0f, 1.0f};

                    meshDesc.indices.push_back((uint32_t) meshDesc.vertices.size());
                    meshDesc.vertices.push_back(vert);
                }
            }
            fvIdx += count;
        }

        if (meshDesc.vertices.empty()) {
            return {};
        }
        return meshLib.add(std::move(meshDesc));
    }

    static bool loadTextureFromAssetPath(const std::string& path, MaterialDesc& matDesc) {
        auto& resolver = ArGetResolver();

        // Try opening with the path as-is (it may already be resolved)
        auto asset = resolver.OpenAsset(ArResolvedPath(path));
        if (!asset) {
            // Try resolving first
            auto resolved = resolver.Resolve(path);
            if (resolved.empty()) {
                return false;
            }
            asset = resolver.OpenAsset(resolved);
            if (!asset) {
                return false;
            }
        }

        auto buffer = asset->GetBuffer();
        if (!buffer) {
            return false;
        }

        auto size = asset->GetSize();
        int w = 0, h = 0, channels = 0;
        auto* pixels = stbi_load_from_memory((const stbi_uc*) buffer.get(), (int) size, &w, &h, &channels, 4);
        if (!pixels) {
            return false;
        }

        matDesc.texWidth = w;
        matDesc.texHeight = h;
        matDesc.texPixels.assign(pixels, pixels + w * h * 4);
        stbi_image_free(pixels);
        return true;
    }

    static void extractShaderTexture(const UsdShadeShader& shader, const TfToken& inputName, MaterialDesc& matDesc) {
        auto input = shader.GetInput(inputName);
        if (!input) {
            return;
        }

        // Check if connected to a texture reader
        UsdShadeConnectableAPI texSource;
        TfToken texSourceName;
        UsdShadeAttributeType texSourceType;
        if (input.GetConnectedSource(&texSource, &texSourceName, &texSourceType)) {
            UsdShadeShader texShader(texSource.GetPrim());
            if (texShader) {
                auto fileInput = texShader.GetInput(TfToken("file"));
                if (fileInput) {
                    SdfAssetPath assetPath;
                    if (fileInput.Get(&assetPath, UsdTimeCode::Default())) {
                        auto path = assetPath.GetResolvedPath();
                        if (path.empty()) {
                            path = assetPath.GetAssetPath();
                        }
                        if (!path.empty()) {
                            loadTextureFromAssetPath(path, matDesc);
                            return;
                        }
                    }
                }
            }
        }

        // No texture — try to read a constant color value
        GfVec3f color;
        if (input.Get(&color, UsdTimeCode::Default())) {
            matDesc.baseColorFactor = glm::vec4(color[0], color[1], color[2], 1.0f);
        }
    }

    static MaterialHandle extractMaterial(const UsdPrim& prim, MaterialLibrary& matLib) {
        UsdShadeMaterialBindingAPI bindingAPI(prim);
        auto material = bindingAPI.ComputeBoundMaterial();
        if (!material) {
            return matLib.add({});
        }

        MaterialDesc matDesc;

        auto surfaceOutput = material.GetSurfaceOutput();
        if (surfaceOutput) {
            UsdShadeConnectableAPI source;
            TfToken sourceName;
            UsdShadeAttributeType sourceType;
            if (surfaceOutput.GetConnectedSource(&source, &sourceName, &sourceType)) {
                UsdShadeShader shader(source.GetPrim());
                if (shader) {
                    extractShaderTexture(shader, TfToken("diffuseColor"), matDesc);
                }
            }
        }

        return matLib.add(std::move(matDesc));
    }

    // ── Edit layer routing ─────────────────────────────────────────────────

    SdfLayerRefPtr chooseEditLayer(const SceneEditRequestContext& ctx) const {
        switch (ctx.purpose) {
            case SceneEditRequestContext::Purpose::Preview:
            case SceneEditRequestContext::Purpose::Debug:
                return sessionLayerRef;
            case SceneEditRequestContext::Purpose::Authoring:
            case SceneEditRequestContext::Purpose::Procedural:
            default:
                return resolveLayer(editTarget);
        }
    }

    // ── Layer dirty state refresh ────────────────────────────────────────

    void refreshLayerDirtyState() {
        for (auto& info : layerInfos) {
            auto layer = resolveLayer(info.handle);
            if (layer) {
                info.dirty = layer->IsDirty();
            }
        }
    }
};

// ── USDScene public API ──────────────────────────────────────────────────────

USDScene::USDScene() : m_impl(std::make_unique<Impl>()) {
}
USDScene::~USDScene() = default;

bool USDScene::open(const char* path) {
    return m_impl->open(path);
}
void USDScene::close() {
    m_impl->close();
}
bool USDScene::isOpen() const {
    return m_impl->stage != nullptr;
}

void USDScene::beginFrame() {
    m_impl->frame++;
}

void USDScene::processChanges() {
    m_impl->processChanges();
}
void USDScene::updateAssetBindings(MeshLibrary& meshLib, MaterialLibrary& matLib) {
    m_impl->updateAssetBindings(meshLib, matLib);
}

void USDScene::endFrame() {
    m_impl->refreshLayerDirtyState();
}

uint32_t USDScene::frameIndex() const {
    return m_impl->frame;
}

PrimHandle USDScene::findPrim(const char* path) const {
    auto it = m_impl->pathToIndex.find(path);
    if (it == m_impl->pathToIndex.end()) {
        return {};
    }
    return {.index = it->second};
}

const PrimRuntimeRecord* USDScene::getPrimRecord(PrimHandle h) const {
    if (h.index == 0 || h.index >= m_impl->prims.size()) {
        return nullptr;
    }
    return &m_impl->prims[h.index];
}

const TransformCacheRecord* USDScene::getTransform(PrimHandle h) const {
    if (h.index == 0 || h.index >= m_impl->transforms.size()) {
        return nullptr;
    }
    return &m_impl->transforms[h.index];
}

const AssetBindingCacheRecord* USDScene::getAssetBinding(PrimHandle h) const {
    if (h.index == 0 || h.index >= m_impl->assetBindings.size()) {
        return nullptr;
    }
    return &m_impl->assetBindings[h.index];
}

std::span<const PrimRuntimeRecord> USDScene::allPrims() const {
    if (m_impl->prims.size() <= 1) {
        return {};
    }
    return {m_impl->prims.data() + 1, m_impl->prims.size() - 1};
}

PrimHandle USDScene::firstChild(PrimHandle h) const {
    auto* rec = getPrimRecord(h);
    return rec ? rec->firstChild : PrimHandle{};
}

PrimHandle USDScene::nextSibling(PrimHandle h) const {
    auto* rec = getPrimRecord(h);
    return rec ? rec->nextSibling : PrimHandle{};
}

// ── Layer management ─────────────────────────────────────────────────────────

std::span<const SceneLayerInfo> USDScene::layers() const {
    return m_impl->layerInfos;
}

LayerHandle USDScene::findLayerByRole(SceneLayerRole role) const {
    for (const auto& info : m_impl->layerInfos) {
        if (info.role == role) {
            return info.handle;
        }
    }
    return {};
}

void USDScene::setEditTarget(LayerHandle layer) {
    auto sdfLayer = m_impl->resolveLayer(layer);
    if (sdfLayer) {
        m_impl->stage->SetEditTarget(UsdEditTarget(sdfLayer));
        m_impl->editTarget = layer;
    }
}

LayerHandle USDScene::currentEditTarget() const {
    return m_impl->editTarget;
}
LayerHandle USDScene::sessionLayer() const {
    return m_impl->sessionLayerHandle;
}

void USDScene::clearSessionLayer() {
    if (m_impl->sessionLayerRef) {
        m_impl->sessionLayerRef->Clear();
    }
}

LayerHandle USDScene::addSubLayer(const char* filepath) {
    // Try to open existing, otherwise create new
    auto layer = SdfLayer::FindOrOpen(filepath);
    if (!layer) {
        layer = SdfLayer::CreateNew(filepath);
    }
    if (!layer) {
        return {};
    }

    m_impl->rootLayer->GetSubLayerPaths().push_back(layer->GetIdentifier());
    m_impl->rebuildLayerInfo();
    m_impl->rebuildPrimCache();
    m_impl->rebuildAllTransforms();

    // Return handle of newly added layer (last in list)
    if (!m_impl->layerInfos.empty()) {
        return m_impl->layerInfos.back().handle;
    }
    return {};
}

bool USDScene::saveLayer(LayerHandle layer) {
    auto sdfLayer = m_impl->resolveLayer(layer);
    if (sdfLayer) {
        return sdfLayer->Save();
    }
    return false;
}

bool USDScene::saveAllDirty() {
    bool allOk = true;
    for (const auto& info : m_impl->layerInfos) {
        if (info.dirty && !info.readOnly) {
            auto sdfLayer = m_impl->resolveLayer(info.handle);
            if (sdfLayer && !sdfLayer->Save()) {
                allOk = false;
            }
        }
    }
    return allOk;
}

// ── Editing ──────────────────────────────────────────────────────────────────

bool USDScene::setTransform(PrimHandle h, const Transform& value, const SceneEditRequestContext& ctx) {
    auto* rec = getPrimRecord(h);
    if (!rec) {
        return false;
    }

    auto layer = m_impl->chooseEditLayer(ctx);
    if (!layer) {
        return false;
    }

    auto prim = m_impl->stage->GetPrimAtPath(SdfPath(rec->path));
    if (!prim) {
        return false;
    }

    {
        UsdEditContext editCtx(m_impl->stage, layer);
        UsdGeomXformable xformable(prim);
        if (!xformable) {
            return false;
        }

        // Find existing transform op or add one
        bool resetXformStack = false;
        auto ops = xformable.GetOrderedXformOps(&resetXformStack);
        UsdGeomXformOp transformOp;
        for (auto& op : ops) {
            if (op.GetOpType() == UsdGeomXformOp::TypeTransform) {
                transformOp = op;
                break;
            }
        }
        if (!transformOp) {
            transformOp = xformable.AddTransformOp();
        }

        auto m = value.toMat4();
        transformOp.Set(GfMatrix4d(
            m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3], m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]));
    }

    // Update cached transforms for this prim and descendants
    m_impl->updateTransformForPrim(h.index);
    for (size_t i = h.index + 1; i < m_impl->prims.size(); i++) {
        if (m_impl->prims[i].parent.index >= h.index) {
            m_impl->updateTransformForPrim(i);
        }
    }

    return true;
}

bool USDScene::setVisibility(PrimHandle h, bool visible, const SceneEditRequestContext& ctx) {
    auto* rec = getPrimRecord(h);
    if (!rec) {
        return false;
    }

    auto layer = m_impl->chooseEditLayer(ctx);
    if (!layer) {
        return false;
    }

    auto prim = m_impl->stage->GetPrimAtPath(SdfPath(rec->path));
    if (!prim) {
        return false;
    }

    {
        UsdEditContext editCtx(m_impl->stage, layer);
        UsdGeomImageable imageable(prim);
        if (!imageable) {
            return false;
        }

        imageable.GetVisibilityAttr().Set(visible ? UsdGeomTokens->inherited : UsdGeomTokens->invisible);
    }

    m_impl->prims[h.index].visible = visible;

    return true;
}

PrimHandle USDScene::createPrim(const char* parentPath, const char* name, const char* typeName, const SceneEditRequestContext& ctx) {
    auto layer = m_impl->chooseEditLayer(ctx);
    if (!layer) {
        return {};
    }

    auto path = SdfPath(parentPath).AppendChild(TfToken(name));

    {
        UsdEditContext editCtx(m_impl->stage, layer);
        auto prim = m_impl->stage->DefinePrim(path, TfToken(typeName));
        if (!prim) {
            return {};
        }
    }

    // The TfNotice will trigger a cache rebuild on next processChanges()
    // Return a handle by looking up the path (it won't exist until rebuild)
    return findPrim(path.GetText());
}

bool USDScene::removePrim(PrimHandle h, const SceneEditRequestContext& ctx) {
    auto* rec = getPrimRecord(h);
    if (!rec) {
        return false;
    }

    auto layer = m_impl->chooseEditLayer(ctx);
    if (!layer) {
        return false;
    }

    {
        UsdEditContext editCtx(m_impl->stage, layer);
        return m_impl->stage->RemovePrim(SdfPath(rec->path));
    }
}
