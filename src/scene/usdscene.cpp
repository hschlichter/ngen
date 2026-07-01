#include "usdscene.h"
#include "material.h"
#include "mesh.h"
#include "primshapemesh.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/notice.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdLux/boundableLightBase.h>
#include <pxr/usd/usdLux/cylinderLight.h>
#include <pxr/usd/usdLux/diskLight.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/nonboundableLightBase.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/shadowAPI.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
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
    bool zUp = false;

    // Muted layer handles (kept alive so FindOrOpen works after muting)
    std::map<std::string, SdfLayerRefPtr> mutedLayerRefs;

    // Prim cache
    std::vector<PrimRuntimeRecord> prims;
    std::unordered_map<std::string, uint32_t> pathToIndex;

    // Transform cache (parallel to prims)
    std::vector<TransformCacheRecord> transforms;

    // Asset binding cache (parallel to prims)
    std::vector<AssetBindingCacheRecord> assetBindings;
    bool assetBindingsBuilt = false;

    // Deduplicates materials across meshes and GeomSubsets: a bound material
    // prim (e.g. Sponza's /root/mtl/*) is shared by hundreds of subsets, so we
    // extract + upload it once and reuse the handle. Keyed by material prim
    // path; mirrors the lifetime of matLib and is cleared on close().
    std::unordered_map<std::string, MaterialHandle> materialPrimCache;

    // Light cache (sparse: only prims with PrimFlagLight). Keyed by prim index.
    std::unordered_map<uint32_t, LightDesc> lights;

    // Layer info
    std::vector<SceneLayerInfo> layerInfos;
    LayerHandle editTarget;
    LayerHandle sessionLayerHandle;

    // Scratch buffers for change processing
    std::vector<SdfPath> scratchResynced;
    std::vector<SdfPath> scratchChanged;
    SceneDirtySet dirtySet;
    bool needsResync = false;

    // ── Stage management ─────────────────────────────────────────────────

    // Finish stage init: capture root/session layers, read stage metadata, seed the
    // default light, start the change listener, and populate the runtime caches.
    // Shared by open() and newScene(); always runs after `stage` is set.
    void postStageInit() {
        rootLayer = stage->GetRootLayer();
        sessionLayerRef = stage->GetSessionLayer();
        metersPerUnit = UsdGeomGetStageMetersPerUnit(stage);
        zUp = (UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->z);

        ensureDefaultDistantLight();

        changeListener.initialize(stage);
        rebuildLayerInfo();
        rebuildPrimCache();
        rebuildAllTransforms();
        rebuildAllLights();
    }

    bool open(const char* path) {
        stage = UsdStage::Open(path);
        if (!stage) {
            fprintf(stderr, "USDScene: failed to open stage: %s\n", path);
            return false;
        }
        postStageInit();
        printf("USDScene: opened %s (%zu prims)\n", path, prims.size());
        return true;
    }

    // Anonymous in-memory stage. Y-up by default. Root layer is a session-level
    // SdfLayer with no filesystem backing; the user saves to disk via an explicit
    // save-as flow. No prims initially beyond the default light ensureDefaultDistantLight
    // adds to the session layer.
    bool newScene() {
        stage = UsdStage::CreateInMemory();
        if (!stage) {
            fprintf(stderr, "USDScene: failed to create in-memory stage\n");
            return false;
        }
        UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
        UsdGeomSetStageMetersPerUnit(stage, 1.0);
        // Seed a default /World Xform so the Scene tree has a root node for the
        // right-click Add Child menu to hang children off — otherwise a brand-new
        // stage has no prim to target. Defined in the root layer (not session)
        // so it persists when the user saves the scene.
        auto world = stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
        if (world) {
            stage->SetDefaultPrim(world);
        }
        postStageInit();
        printf("USDScene: created new in-memory stage\n");
        return true;
    }

    // If the composed stage has no UsdLuxDistantLight, author one into the session layer so
    // the engine always has a shadow-casting light. Session-layer edits are non-persistent —
    // the user's source file is not modified.
    void ensureDefaultDistantLight() {
        for (const auto& prim : stage->Traverse()) {
            if (prim.IsA<UsdLuxDistantLight>()) {
                return;
            }
        }

        UsdEditContext ctx(stage, sessionLayerRef);
        auto distant = UsdLuxDistantLight::Define(stage, SdfPath("/__DefaultEngineLight"));
        if (!distant) {
            return;
        }
        distant.CreateIntensityAttr().Set(1.0f);
        distant.CreateColorAttr().Set(GfVec3f(1.0f, 0.95f, 0.88f));
        distant.CreateAngleAttr().Set(0.53f);

        // Orient the light's +Z axis (the "toward-light" direction the renderer reads)
        // upper-front-right in the stage's native coordinate frame. For Y-up scenes this
        // is (X+2Y+Z) normalized; for Z-up scenes (X+Y+2Z) normalized. The renderer no
        // longer rotates USD content into an "engine space", so this direction is what
        // the engine sees directly.
        auto dir = zUp ? glm::normalize(glm::vec3(1.0f, 1.0f, 2.0f)) : glm::normalize(glm::vec3(1.0f, 2.0f, 1.0f));
        auto sceneUp = zUp ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        auto xAxis = glm::normalize(glm::cross(sceneUp, dir));
        auto yAxis = glm::cross(dir, xAxis);
        GfMatrix4d m(xAxis.x, xAxis.y, xAxis.z, 0, yAxis.x, yAxis.y, yAxis.z, 0, dir.x, dir.y, dir.z, 0, 0, 0, 0, 1);
        UsdGeomXformable xformable(distant.GetPrim());
        xformable.AddTransformOp().Set(m);
    }

    void close() {
        changeListener.shutdown();
        prims.clear();
        pathToIndex.clear();
        transforms.clear();
        assetBindings.clear();
        assetBindingsBuilt = false;
        materialPrimCache.clear();
        lights.clear();
        layerInfos.clear();
        dirtySet.clear();
        stage = nullptr;
        rootLayer = nullptr;
        sessionLayerRef = nullptr;
        frame = 0;
    }

    // ── Light cache ──────────────────────────────────────────────────────

    void rebuildAllLights() {
        lights.clear();
        for (size_t i = 1; i < prims.size(); i++) {
            if ((prims[i].flags & PrimFlagLight) != 0) {
                updateLightForPrim((uint32_t) i);
            }
        }
    }

    void updateLightForPrim(uint32_t idx) {
        const auto& rec = prims[idx];
        auto prim = stage->GetPrimAtPath(SdfPath(rec.path));
        if (!prim) {
            return;
        }

        LightDesc desc;
        if (prim.IsA<UsdLuxDistantLight>()) {
            desc.kind = LightKind::Distant;
            UsdLuxDistantLight distant(prim);
            float angle = 0.53f;
            distant.GetAngleAttr().Get(&angle);
            desc.angle = angle;
        } else if (prim.IsA<UsdLuxSphereLight>()) {
            desc.kind = LightKind::Sphere;
        } else if (prim.IsA<UsdLuxRectLight>()) {
            desc.kind = LightKind::Rect;
        } else if (prim.IsA<UsdLuxDiskLight>()) {
            desc.kind = LightKind::Disk;
        } else if (prim.IsA<UsdLuxCylinderLight>()) {
            desc.kind = LightKind::Cylinder;
        } else if (prim.IsA<UsdLuxDomeLight>()) {
            desc.kind = LightKind::Dome;
        }
        // Non-distant lights are stored with the correct kind but the shading/shadow paths
        // only use Distant today — other kinds fall through to LightType::Directional with
        // whatever transform they have, which isn't meaningful yet. Filtered out in the
        // extractor so they don't masquerade as directional sources.

        UsdLuxLightAPI lightAPI(prim);
        if (lightAPI) {
            GfVec3f color(1.0f);
            lightAPI.GetColorAttr().Get(&color);
            desc.color = glm::vec3(color[0], color[1], color[2]);
            lightAPI.GetIntensityAttr().Get(&desc.intensity);
            lightAPI.GetExposureAttr().Get(&desc.exposure);
        }

        UsdLuxShadowAPI shadowAPI(prim);
        if (shadowAPI) {
            shadowAPI.GetShadowEnableAttr().Get(&desc.shadowEnable);
            GfVec3f sColor(0.0f);
            shadowAPI.GetShadowColorAttr().Get(&sColor);
            desc.shadowColor = glm::vec3(sColor[0], sColor[1], sColor[2]);
        }

        lights[idx] = desc;
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
        std::set<std::string> seen;
        uint32_t idx = 1;

        auto addLayer = [&](const SdfLayerHandle& layer, SceneLayerRole role, const std::string& name) {
            if (!layer || seen.count(layer->GetIdentifier())) {
                return;
            }
            seen.insert(layer->GetIdentifier());

            SceneLayerInfo info;
            info.handle = {.index = idx++};
            info.identifier = layer->GetIdentifier();
            info.displayName = name.empty() ? (layer->GetDisplayName().empty() ? layer->GetIdentifier() : layer->GetDisplayName()) : name;
            info.role = role;
            info.dirty = layer->IsDirty();
            info.readOnly = layer->GetFileFormat()->IsPackage();
            info.muted = stage->IsLayerMuted(layer->GetIdentifier());
            layerInfos.push_back(std::move(info));
        };

        // 1. Session layer
        addLayer(sessionLayerRef, SceneLayerRole::Session, "Session");
        sessionLayerHandle = layerInfos.back().handle;

        // 2. Root layer
        addLayer(rootLayer, SceneLayerRole::Root, "");
        LayerHandle rootHandle = layerInfos.back().handle;

        // 3. Sublayers of root
        for (const auto& subPath : rootLayer->GetSubLayerPaths()) {
            auto subLayer = SdfLayer::FindOrOpen(subPath);
            if (subLayer) {
                addLayer(subLayer, SceneLayerRole::Sublayer, "");
            }
        }

        // 4. Referenced/payloaded layers (used + muted, sorted by identifier for stability)
        std::map<std::string, SdfLayerHandle> referencedLayers;
        for (const auto& layer : stage->GetUsedLayers()) {
            referencedLayers[layer->GetIdentifier()] = layer;
        }
        for (const auto& mutedId : stage->GetMutedLayers()) {
            if (referencedLayers.count(mutedId)) {
                continue;
            }
            auto layer = SdfLayer::FindOrOpen(mutedId);
            if (layer) {
                referencedLayers[layer->GetIdentifier()] = layer;
            }
        }
        for (const auto& [id, layer] : referencedLayers) {
            addLayer(layer, SceneLayerRole::Referenced, "");
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
        if (prim.IsA<UsdGeomMesh>() || prim.IsA<UsdGeomCube>() || prim.IsA<UsdGeomSphere>() || prim.IsA<UsdGeomCylinder>() || prim.IsA<UsdGeomCone>()) {
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

        // Scene-unit → meter conversion is still applied: metersPerUnit is a uniform
        // scale, not a coordinate flip. Up-axis is *not* baked into world transforms —
        // consumers read USDScene::worldUp() when they need a vertical reference.
        if (metersPerUnit != 1.0) {
            auto scale = glm::scale(glm::mat4(1.0f), glm::vec3((float) metersPerUnit));
            for (size_t i = 1; i < transforms.size(); i++) {
                transforms[i].world = scale * transforms[i].world;
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

        bool resync = needsResync || !scratchResynced.empty();
        needsResync = false;

        if (resync) {
            rebuildPrimCache();
            rebuildAllTransforms();
            // Light cache is keyed by prim index. rebuildPrimCache reassigns indices,
            // so any stale entries from the previous prim set would either be missing
            // (light drops out of the extract) or point at a non-light prim that now
            // occupies the old slot. Rebuild so the cache tracks the new indices.
            rebuildAllLights();
            dirtySet.primsResynced.push_back({.index = 0}); // sentinel: full resync
            scratchResynced.clear();
            scratchChanged.clear();
            return;
        }

        if (scratchChanged.empty()) {
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

            assetBindings[i].mesh = extractMesh(prim, meshLib, matLib);
            // The per-prim material is derived from the mesh's first submesh so
            // consumers that want "the" material of a prim (e.g. the Properties
            // panel) still get a sensible value; per-subset materials live on
            // the mesh's submeshes and drive the actual draws.
            const auto* meshData = meshLib.get(assetBindings[i].mesh);
            assetBindings[i].material = (meshData != nullptr && !meshData->submeshes.empty()) ? meshData->submeshes.front().material : MaterialHandle{};
            assetBindings[i].revision++;
        }

        assetBindingsBuilt = true;
    }

    // Read a primvar with its index table applied (ComputeFlattened) and report
    // its interpolation. NewSponza authors st/normals as indexed, faceVarying
    // primvars — reading the raw value array and indexing it directly yields
    // scrambled UVs / normals, so flattening is required.
    template <typename T>
    static bool readPrimvarFlattened(const UsdGeomPrimvarsAPI& api, const TfToken& name, VtArray<T>& values, TfToken& interp) {
        auto pv = api.GetPrimvar(name);
        if (!pv || !pv.HasValue()) {
            return false;
        }
        interp = pv.GetInterpolation();
        return pv.ComputeFlattened(&values, UsdTimeCode::Default()) && !values.empty();
    }

    // Index into a flattened primvar for a given triangle corner, per the
    // primvar's interpolation. cornerIdx is the face-vertex (face-corner) index.
    static int primvarIndex(const TfToken& interp, int pointIdx, int cornerIdx, int faceIdx) {
        if (interp == UsdGeomTokens->constant) {
            return 0;
        }
        if (interp == UsdGeomTokens->uniform) {
            return faceIdx;
        }
        if (interp == UsdGeomTokens->faceVarying) {
            return cornerIdx;
        }
        return pointIdx; // vertex / varying
    }

    // sRGB-encode a linear color channel for storage in an R8G8B8A8_SRGB texel,
    // so the sampler's sRGB→linear decode reproduces the authored linear color.
    static uint8_t encodeSrgb(float c) {
        c = std::clamp(c, 0.0f, 1.0f);
        float s = (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
        return (uint8_t) std::lround(s * 255.0f);
    }

    // Adds a MeshDesc to the library with a single submesh spanning the whole
    // index buffer, bound to `material`. Used for procedural shapes and for USD
    // meshes that don't partition into materialBind GeomSubsets.
    static MeshHandle addSingleSubmeshMesh(MeshDesc desc, MaterialHandle material, MeshLibrary& meshLib) {
        if (!desc.vertices.empty()) {
            desc.submeshes.push_back({.indexOffset = 0, .indexCount = (uint32_t) desc.indices.size(), .material = material});
        }
        return meshLib.add(std::move(desc));
    }

    MeshHandle extractMesh(const UsdPrim& prim, MeshLibrary& meshLib, MaterialLibrary& matLib) {
        // Procedural shape schemas (Cube / Sphere / Cylinder / Cone) tessellate into a
        // MeshDesc on first extract. Each prim gets its own cached MeshDesc — we don't
        // share across prims even with identical parameters, because edits to `size` /
        // `radius` etc. would otherwise stomp one another. Cheap enough for now.
        // Base color comes from the material (extractMaterial folds displayColor into
        // baseColorFactor), so vertices are left white and untinted — see extractMesh's
        // polymesh path and the fallback texel in extractMaterial.
        if (auto cube = UsdGeomCube(prim); cube) {
            double size = 2.0;
            cube.GetSizeAttr().Get(&size, UsdTimeCode::Default());
            return addSingleSubmeshMesh(tessellateCube(size), extractMaterial(prim, matLib), meshLib);
        }
        if (auto sphere = UsdGeomSphere(prim); sphere) {
            double radius = 1.0;
            sphere.GetRadiusAttr().Get(&radius, UsdTimeCode::Default());
            return addSingleSubmeshMesh(tessellateSphere(radius), extractMaterial(prim, matLib), meshLib);
        }
        if (auto cyl = UsdGeomCylinder(prim); cyl) {
            double radius = 1.0, height = 2.0;
            cyl.GetRadiusAttr().Get(&radius, UsdTimeCode::Default());
            cyl.GetHeightAttr().Get(&height, UsdTimeCode::Default());
            TfToken axis = UsdGeomTokens->z;
            cyl.GetAxisAttr().Get(&axis, UsdTimeCode::Default());
            return addSingleSubmeshMesh(tessellateCylinder(radius, height, axis.GetText()), extractMaterial(prim, matLib), meshLib);
        }
        if (auto cone = UsdGeomCone(prim); cone) {
            double radius = 1.0, height = 2.0;
            cone.GetRadiusAttr().Get(&radius, UsdTimeCode::Default());
            cone.GetHeightAttr().Get(&height, UsdTimeCode::Default());
            TfToken axis = UsdGeomTokens->z;
            cone.GetAxisAttr().Get(&axis, UsdTimeCode::Default());
            return addSingleSubmeshMesh(tessellateCone(radius, height, axis.GetText()), extractMaterial(prim, matLib), meshLib);
        }

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

        UsdGeomPrimvarsAPI primvarsAPI(prim);

        // Normals: prefer primvars:normals (how NewSponza and most DCC exports
        // author them), fall back to the schema `normals` attribute, else flat
        // per-face normals are computed below. All indexed/flattened correctly.
        VtArray<GfVec3f> normals;
        TfToken normalsInterp;
        bool hasNormals = readPrimvarFlattened(primvarsAPI, TfToken("normals"), normals, normalsInterp);
        if (!hasNormals && geomMesh.GetNormalsAttr().Get(&normals, UsdTimeCode::Default()) && !normals.empty()) {
            normalsInterp = geomMesh.GetNormalsInterpolation();
            hasNormals = true;
        }

        // UVs: try the common primvar names, flattening the index table.
        VtArray<GfVec2f> uvs;
        TfToken uvInterp;
        bool hasUV = false;
        for (const auto* name : {"st", "st0", "st1", "UVMap"}) {
            if (readPrimvarFlattened(primvarsAPI, TfToken(name), uvs, uvInterp)) {
                hasUV = true;
                break;
            }
        }

        // Triangulate. Vertices are emitted per-corner in face order; we record
        // where each face's vertices land so the index buffer can afterwards be
        // assembled grouped by material (GeomSubset). Since one vertex is pushed
        // per index, the index value for a face's j-th corner is faceStart[f] + j.
        MeshDesc meshDesc;
        std::vector<uint32_t> faceStart(faceVertexCounts.size(), 0);
        std::vector<uint32_t> faceCount(faceVertexCounts.size(), 0);
        uint32_t fvIdx = 0;
        for (size_t f = 0; f < faceVertexCounts.size(); f++) {
            int count = faceVertexCounts[f];
            faceStart[f] = (uint32_t) meshDesc.vertices.size();
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

                // Compute flat face normal as fallback when normals aren't authored
                std::array<float, 3> faceNormal = {0.0f, 1.0f, 0.0f};
                if (!hasNormals) {
                    auto& p0 = points[indices[0]];
                    auto& p1 = points[indices[1]];
                    auto& p2 = points[indices[2]];
                    float e1x = p1[0] - p0[0], e1y = p1[1] - p0[1], e1z = p1[2] - p0[2];
                    float e2x = p2[0] - p0[0], e2y = p2[1] - p0[1], e2z = p2[2] - p0[2];
                    float nx = e1y * e2z - e1z * e2y;
                    float ny = e1z * e2x - e1x * e2z;
                    float nz = e1x * e2y - e1y * e2x;
                    float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (len > 0.0f) {
                        faceNormal = {nx / len, ny / len, nz / len};
                    }
                }

                for (int v = 0; v < 3; v++) {
                    Vertex vert = {};
                    auto& p = points[indices[v]];
                    vert.position = {p[0], p[1], p[2]};

                    if (hasNormals) {
                        int ni = primvarIndex(normalsInterp, indices[v], fvIndices[v], (int) f);
                        if (ni < (int) normals.size()) {
                            auto& n = normals[ni];
                            vert.normal = {n[0], n[1], n[2]};
                        }
                    } else {
                        vert.normal = faceNormal;
                    }

                    if (hasUV) {
                        int ui = primvarIndex(uvInterp, indices[v], fvIndices[v], (int) f);
                        if (ui < (int) uvs.size()) {
                            auto& uv = uvs[ui];
                            vert.texCoord = {uv[0], 1.0f - uv[1]};
                        }
                    }

                    // Base color comes from the material (texture or the
                    // baseColorFactor fallback texel), so vertices stay white
                    // and don't tint the sampled albedo.
                    vert.color = {1.0f, 1.0f, 1.0f};

                    meshDesc.vertices.push_back(vert);
                }
            }
            faceCount[f] = (uint32_t) meshDesc.vertices.size() - faceStart[f];
            fvIdx += count;
        }

        if (meshDesc.vertices.empty()) {
            return {};
        }

        // Assemble the index buffer so each material occupies a contiguous range
        // that draws with a single call. USD binds materials per-face via
        // GeomSubsets (Sponza partitions every mesh this way); a mesh with no
        // subsets is one submesh over all faces in original order.
        auto appendFace = [&](uint32_t f) {
            for (uint32_t j = 0; j < faceCount[f]; j++) {
                meshDesc.indices.push_back(faceStart[f] + j);
            }
        };

        UsdShadeMaterialBindingAPI binder(prim);
        auto subsets = binder.GetMaterialBindSubsets();
        if (!subsets.empty()) {
            std::vector<bool> assigned(faceCount.size(), false);
            for (const auto& subset : subsets) {
                VtArray<int> subsetFaces;
                subset.GetIndicesAttr().Get(&subsetFaces, UsdTimeCode::Default());
                SubMesh sm;
                sm.indexOffset = (uint32_t) meshDesc.indices.size();
                for (int f : subsetFaces) {
                    if (f < 0 || f >= (int) faceCount.size()) {
                        continue;
                    }
                    assigned[f] = true;
                    appendFace((uint32_t) f);
                }
                sm.indexCount = (uint32_t) meshDesc.indices.size() - sm.indexOffset;
                if (sm.indexCount > 0) {
                    sm.material = extractMaterial(subset.GetPrim(), matLib);
                    meshDesc.submeshes.push_back(sm);
                }
            }
            // Any faces outside the partition fall back to the mesh-level binding.
            SubMesh rest;
            rest.indexOffset = (uint32_t) meshDesc.indices.size();
            for (uint32_t f = 0; f < faceCount.size(); f++) {
                if (!assigned[f]) {
                    appendFace(f);
                }
            }
            rest.indexCount = (uint32_t) meshDesc.indices.size() - rest.indexOffset;
            if (rest.indexCount > 0) {
                rest.material = extractMaterial(prim, matLib);
                meshDesc.submeshes.push_back(rest);
            }
        } else {
            for (uint32_t f = 0; f < faceCount.size(); f++) {
                appendFace(f);
            }
            meshDesc.submeshes.push_back({
                .indexOffset = 0,
                .indexCount = (uint32_t) meshDesc.indices.size(),
                .material = extractMaterial(prim, matLib),
            });
        }

        return meshLib.add(std::move(meshDesc));
    }

    // Resolve a UsdUVTexture `file` input to an absolute, openable path.
    //
    // Windows-authored assets (e.g. Intel's NewSponza) reference textures with
    // backslash separators — @textures\foo.png@. On POSIX, USD's resolver treats
    // '\' as an ordinary filename character rather than a separator, so
    // GetResolvedPath() comes back empty and the texture silently fails to load,
    // leaving the surface untextured. Normalize the separators and re-anchor the
    // relative path against the layer that authored the opinion (mirroring
    // UsdImaging's UsdUVTexture handling), so any texture input resolves
    // regardless of how it was authored.
    static std::string resolveTexturePath(const UsdShadeInput& fileInput) {
        SdfAssetPath assetPath;
        if (!fileInput.Get(&assetPath, UsdTimeCode::Default())) {
            return {};
        }

        // Fast path: the resolver already located the asset.
        if (!assetPath.GetResolvedPath().empty()) {
            return assetPath.GetResolvedPath();
        }

        const std::string raw = assetPath.GetAssetPath();
        if (raw.empty()) {
            return {};
        }

        auto& resolver = ArGetResolver();

        // The path as authored may already be resolvable (POSIX-style relative,
        // absolute, or a URI understood by a custom resolver).
        if (auto resolved = resolver.Resolve(raw); !resolved.empty()) {
            return resolved;
        }

        // Otherwise normalize Windows separators and re-anchor against each layer
        // that contributes an opinion, strongest first.
        const std::string normalized = TfStringReplace(raw, "\\", "/");
        if (normalized != raw) {
            for (const auto& spec : fileInput.GetAttr().GetPropertyStack(UsdTimeCode::Default())) {
                const std::string anchored = SdfComputeAssetPathRelativeToLayer(spec->GetLayer(), normalized);
                if (auto resolved = resolver.Resolve(anchored); !resolved.empty()) {
                    return resolved;
                }
            }
            // Last resort: resolve the normalized path directly (search paths / CWD).
            if (auto resolved = resolver.Resolve(normalized); !resolved.empty()) {
                return resolved;
            }
        }

        return {};
    }

    static bool loadTextureFromResolvedPath(const std::string& resolvedPath, MaterialDesc& matDesc) {
        if (resolvedPath.empty()) {
            return false;
        }

        auto& resolver = ArGetResolver();
        auto asset = resolver.OpenAsset(ArResolvedPath(resolvedPath));
        if (!asset) {
            return false;
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

    // Follow a shading connection to the Shader that ultimately drives it,
    // passing through UsdShade NodeGraph wrappers. Material libraries like
    // NewSponza don't connect diffuseColor straight to the UsdUVTexture; they
    // route it through a per-texture NodeGraph whose `outputs:rgb` forwards the
    // inner shader's output. `source`/`sourceName` are the connection's source
    // connectable and output name (e.g. the NodeGraph and "rgb"). Returns an
    // invalid shader if the chain doesn't terminate at a Shader.
    static UsdShadeShader resolveConnectedShader(UsdShadeConnectableAPI source, TfToken sourceName) {
        for (int guard = 0; guard < 16 && source.GetPrim(); guard++) {
            if (source.GetPrim().IsA<UsdShadeShader>()) {
                return UsdShadeShader(source.GetPrim());
            }
            // NodeGraph pass-through: follow the like-named output inward.
            auto output = source.GetOutput(sourceName);
            if (!output) {
                break;
            }
            UsdShadeConnectableAPI next;
            TfToken nextName;
            UsdShadeAttributeType nextType;
            if (!output.GetConnectedSource(&next, &nextName, &nextType)) {
                break;
            }
            source = next;
            sourceName = nextName;
        }
        return UsdShadeShader();
    }

    static void extractShaderTexture(const UsdShadeShader& shader, const TfToken& inputName, MaterialDesc& matDesc) {
        auto input = shader.GetInput(inputName);
        if (!input) {
            return;
        }

        // Check if connected to a texture reader (possibly via a NodeGraph).
        UsdShadeConnectableAPI texSource;
        TfToken texSourceName;
        UsdShadeAttributeType texSourceType;
        if (input.GetConnectedSource(&texSource, &texSourceName, &texSourceType)) {
            UsdShadeShader texShader = resolveConnectedShader(texSource, texSourceName);
            if (texShader) {
                auto fileInput = texShader.GetInput(TfToken("file"));
                if (fileInput) {
                    if (loadTextureFromResolvedPath(resolveTexturePath(fileInput), matDesc)) {
                        return;
                    }
                }
            }
        }

        // No texture (or the texture failed to resolve/load) — fall back to a
        // constant color value if the input authors one.
        GfVec3f color;
        if (input.Get(&color, UsdTimeCode::Default())) {
            matDesc.baseColorFactor = glm::vec4(color[0], color[1], color[2], 1.0f);
        }
    }

    MaterialHandle extractMaterial(const UsdPrim& prim, MaterialLibrary& matLib) {
        // Try UsdPreviewSurface material binding first
        UsdShadeMaterialBindingAPI bindingAPI(prim);
        auto material = bindingAPI.ComputeBoundMaterial();

        // Deduplicate by the bound material prim's path: a single material
        // backs many prims / GeomSubsets, and its textures can be tens of MB
        // each — extract and upload it once, then reuse the handle.
        std::string cacheKey;
        if (material) {
            cacheKey = material.GetPrim().GetPath().GetString();
            auto it = materialPrimCache.find(cacheKey);
            if (it != materialPrimCache.end()) {
                return it->second;
            }
        }

        MaterialDesc matDesc;
        if (material) {
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
        }

        // Fall back to displayColor primvar
        if (matDesc.texPixels.empty() && matDesc.baseColorFactor == glm::vec4(1.0f)) {
            UsdGeomGprim gprim(prim);
            if (gprim) {
                VtArray<GfVec3f> displayColors;
                if (gprim.GetDisplayColorAttr().Get(&displayColors) && !displayColors.empty()) {
                    auto c = displayColors[0];
                    matDesc.baseColorFactor = glm::vec4(c[0], c[1], c[2], 1.0f);
                }
            }
        }

        // No texture: encode the constant baseColorFactor into a 1x1 texel so it
        // flows through the same sampling path as a real albedo map (vertices are
        // white, so the sampled color is the surface color). Defaults to white
        // when nothing was authored. sRGB-encoded to match the SRGB texel format.
        if (matDesc.texPixels.empty()) {
            matDesc.texWidth = 1;
            matDesc.texHeight = 1;
            matDesc.texPixels = {
                encodeSrgb(matDesc.baseColorFactor.r),
                encodeSrgb(matDesc.baseColorFactor.g),
                encodeSrgb(matDesc.baseColorFactor.b),
                255,
            };
        }

        MaterialHandle handle = matLib.add(std::move(matDesc));
        if (!cacheKey.empty()) {
            materialPrimCache[cacheKey] = handle;
        }
        return handle;
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

    // A prim is session-only if every primSpec contributing to it lives in the session
    // layer — i.e. the engine authored it (e.g. the default distant light). Edits to
    // such prims must target the session layer, otherwise writes to a weaker edit
    // target would be shadowed by the session-layer ops and fail the xformOp checks.
    //
    // NOTE: SdfPrimSpec::GetLayer() returns SdfLayerHandle (TfWeakPtr). Comparing a
    // TfWeakPtr with a TfRefPtr directly in this USD version recurses into operator==
    // and blows the stack, so convert to a SdfLayerHandle once and compare weak-to-weak.
    bool isSessionOnlyPrim(const UsdPrim& prim) const {
        auto stack = prim.GetPrimStack();
        if (stack.empty()) {
            return false;
        }
        SdfLayerHandle sessionHandle(sessionLayerRef);
        for (const auto& spec : stack) {
            if (spec->GetLayer() != sessionHandle) {
                return false;
            }
        }
        return true;
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
bool USDScene::newScene() {
    return m_impl->newScene();
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

const SceneDirtySet& USDScene::dirtySet() const {
    return m_impl->dirtySet;
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

const LightDesc* USDScene::getLightDesc(PrimHandle h) const {
    auto it = m_impl->lights.find(h.index);
    if (it == m_impl->lights.end()) {
        return nullptr;
    }
    return &it->second;
}

glm::vec3 USDScene::worldUp() const {
    return m_impl->zUp ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
}

std::string USDScene::rootLayerDirectory() const {
    if (!m_impl->rootLayer) {
        return {};
    }
    auto id = m_impl->rootLayer->GetIdentifier();
    if (id.empty()) {
        return {};
    }
    return std::filesystem::path(id).parent_path().string();
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

void USDScene::setLayerMuted(LayerHandle layer, bool muted) {
    for (const auto& info : m_impl->layerInfos) {
        if (info.handle == layer) {
            if (muted) {
                auto layerRef = SdfLayer::FindOrOpen(info.identifier);
                if (layerRef) {
                    m_impl->mutedLayerRefs[info.identifier] = layerRef;
                }
                m_impl->stage->MuteLayer(info.identifier);
            } else {
                m_impl->stage->UnmuteLayer(info.identifier);
                m_impl->mutedLayerRefs.erase(info.identifier);
            }
            m_impl->rebuildLayerInfo();
            m_impl->needsResync = true;
            return;
        }
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
    m_impl->needsResync = true;

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

bool USDScene::exportRootLayerTo(const char* path) const {
    if (!m_impl->rootLayer) {
        return false;
    }
    return m_impl->rootLayer->Export(path);
}

bool USDScene::hasAnonymousRootLayer() const {
    return m_impl->rootLayer && m_impl->rootLayer->IsAnonymous();
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

    if (ctx.purpose == SceneEditRequestContext::Purpose::Preview) {
        // Preview path: skip the USD layer write entirely. Just patch the
        // runtime transform cache so the renderer + scene query see the new
        // value this frame. The authored layer stays unchanged until a
        // subsequent Authoring edit commits.
        auto& xf = m_impl->transforms[h.index];
        xf.local = value;
        auto localMat = value.toMat4();
        if (rec->parent) {
            xf.world = m_impl->transforms[rec->parent.index].world * localMat;
        } else {
            xf.world = localMat;
        }
        // Descendants: recompute world from existing local + new parent.world.
        // updateTransformForPrim() reads back from USD, which still has the
        // OLD local for `h`. That's harmless for descendants since their *own*
        // local hasn't changed; we just need their world refreshed.
        for (size_t i = h.index + 1; i < m_impl->prims.size(); i++) {
            if (m_impl->prims[i].parent.index >= h.index) {
                m_impl->updateTransformForPrim(i);
            }
        }
        return true;
    }

    auto prim = m_impl->stage->GetPrimAtPath(SdfPath(rec->path));
    if (!prim) {
        return false;
    }

    auto layer = m_impl->isSessionOnlyPrim(prim) ? m_impl->sessionLayerRef : m_impl->chooseEditLayer(ctx);
    if (!layer) {
        return false;
    }

    {
        UsdEditContext editCtx(m_impl->stage, layer);
        UsdGeomXformable xformable(prim);
        if (!xformable) {
            return false;
        }

        // Prefer XformCommonAPI: writes to the existing translate/rotate/scale ops
        // (or creates the standard set on a fresh prim), preserving op-stack
        // structure and per-layer composition. Falls back to clobbering the stack
        // with a single transform op only when the prim's existing ops aren't
        // common-compatible (e.g. shear, custom matrix-only authoring).
        UsdGeomXformCommonAPI common(prim);
        if (common) {
            common.SetTranslate(GfVec3d(value.position.x, value.position.y, value.position.z));
            auto euler = glm::degrees(glm::eulerAngles(value.rotation));
            common.SetRotate(GfVec3f(euler.x, euler.y, euler.z), UsdGeomXformCommonAPI::RotationOrderXYZ);
            common.SetScale(GfVec3f(value.scale.x, value.scale.y, value.scale.z));
        } else {
            xformable.ClearXformOpOrder();
            auto op = xformable.AddTransformOp();
            auto m = value.toMat4();
            op.Set(GfMatrix4d(
                m[0][0],
                m[0][1],
                m[0][2],
                m[0][3],
                m[1][0],
                m[1][1],
                m[1][2],
                m[1][3],
                m[2][0],
                m[2][1],
                m[2][2],
                m[2][3],
                m[3][0],
                m[3][1],
                m[3][2],
                m[3][3]));
        }
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

    auto prim = m_impl->stage->GetPrimAtPath(SdfPath(rec->path));
    if (!prim) {
        return false;
    }

    auto layer = m_impl->isSessionOnlyPrim(prim) ? m_impl->sessionLayerRef : m_impl->chooseEditLayer(ctx);
    if (!layer) {
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

glm::vec3 USDScene::getDisplayColor(PrimHandle h) const {
    auto* rec = getPrimRecord(h);
    if (!rec) {
        return glm::vec3(1.0f);
    }
    auto prim = m_impl->stage->GetPrimAtPath(SdfPath(rec->path));
    if (!prim) {
        return glm::vec3(1.0f);
    }
    UsdGeomGprim gprim(prim);
    if (!gprim) {
        return glm::vec3(1.0f);
    }
    VtArray<GfVec3f> colors;
    gprim.GetDisplayColorAttr().Get(&colors, UsdTimeCode::Default());
    if (colors.empty()) {
        return glm::vec3(1.0f);
    }
    return glm::vec3(colors[0][0], colors[0][1], colors[0][2]);
}

bool USDScene::setDisplayColor(PrimHandle h, const glm::vec3& color, const SceneEditRequestContext& ctx) {
    auto* rec = getPrimRecord(h);
    if (!rec) {
        return false;
    }
    auto prim = m_impl->stage->GetPrimAtPath(SdfPath(rec->path));
    if (!prim) {
        return false;
    }
    auto layer = m_impl->isSessionOnlyPrim(prim) ? m_impl->sessionLayerRef : m_impl->chooseEditLayer(ctx);
    if (!layer) {
        return false;
    }

    UsdGeomGprim gprim(prim);
    if (!gprim) {
        return false;
    }

    {
        UsdEditContext editCtx(m_impl->stage, layer);
        VtArray<GfVec3f> colors = {GfVec3f(color.r, color.g, color.b)};
        gprim.CreateDisplayColorAttr().Set(colors);
    }

    // Procedural shape prims bake displayColor into their tessellated vertices; invalidate
    // the cached mesh binding so the next updateAssetBindings re-runs extractMesh and picks
    // up the new color. UsdGeomMesh would want similar treatment if we tessellated there,
    // but the existing mesh extractor already re-reads colors on full extract.
    // Clearing the mesh handle defeats the skip guard in updateAssetBindings, so the
    // prim re-extracts next pass. `needsResync` steers SceneUpdater into the full-
    // extract branch so updateAssetBindings actually runs.
    if (h.index < m_impl->assetBindings.size()) {
        m_impl->assetBindings[h.index].mesh = {};
    }
    m_impl->needsResync = true;
    return true;
}

PrimHandle USDScene::createReferencePrim(const char* parentPath, const char* name, const char* referenceAsset, const SceneEditRequestContext& ctx) {
    auto layer = m_impl->chooseEditLayer(ctx);
    if (!layer) {
        return {};
    }

    auto path = SdfPath(parentPath).AppendChild(TfToken(name));

    {
        UsdEditContext editCtx(m_impl->stage, layer);
        // Define as an Xform so the prim has a sensible default typeName. The referenced
        // layer contributes its own typeName via composition — USD picks the strongest.
        auto prim = m_impl->stage->DefinePrim(path, TfToken("Xform"));
        if (!prim) {
            return {};
        }
        prim.GetReferences().AddReference(referenceAsset);
    }
    return findPrim(path.GetText());
}

std::string USDScene::uniqueChildName(const char* parentPath, const char* baseName) const {
    if (!m_impl->stage) {
        return baseName;
    }
    auto parent = m_impl->stage->GetPrimAtPath(SdfPath(parentPath));
    if (!parent) {
        return baseName;
    }
    auto hasChild = [&](const std::string& candidate) {
        for (const auto& child : parent.GetChildren()) {
            if (child.GetName().GetString() == candidate) {
                return true;
            }
        }
        return false;
    };
    if (!hasChild(baseName)) {
        return baseName;
    }
    for (int i = 1; i < 10000; i++) {
        auto candidate = std::string(baseName) + "_" + std::to_string(i);
        if (!hasChild(candidate)) {
            return candidate;
        }
    }
    return baseName; // pathological; caller will see a DefinePrim conflict
}

bool USDScene::removePrim(PrimHandle h, const SceneEditRequestContext& ctx) {
    auto* rec = getPrimRecord(h);
    if (!rec) {
        return false;
    }

    auto prim = m_impl->stage->GetPrimAtPath(SdfPath(rec->path));
    auto layer = prim && m_impl->isSessionOnlyPrim(prim) ? m_impl->sessionLayerRef : m_impl->chooseEditLayer(ctx);
    if (!layer) {
        return false;
    }

    {
        UsdEditContext editCtx(m_impl->stage, layer);
        return m_impl->stage->RemovePrim(SdfPath(rec->path));
    }
}
