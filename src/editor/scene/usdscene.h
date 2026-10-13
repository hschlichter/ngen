#pragma once

#include "scenehandles.h"
#include "scenetypes.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum class SceneLayerRole : uint8_t {
    Root,
    Sublayer,
    Referenced,
    Session,
    Unknown,
};

struct SceneLayerInfo {
    LayerHandle handle;
    std::string identifier;
    std::string displayName;
    SceneLayerRole role = SceneLayerRole::Unknown;
    bool dirty = false;
    bool readOnly = false;
    bool muted = false;
};

struct PrimRuntimeRecord {
    PrimHandle handle;
    PrimHandle parent;
    PrimHandle firstChild;
    PrimHandle nextSibling;
    std::string path;
    std::string name;
    uint64_t flags = 0;
    bool active = true;
    bool loaded = true;
    bool visible = true;
};

struct TransformCacheRecord {
    Transform local;
    glm::mat4 world = glm::mat4(1.0f);
    uint32_t lastFrame = 0;
};

struct SceneDirtySet {
    std::vector<PrimHandle> primsResynced;
    std::vector<PrimHandle> transformDirty;
    std::vector<PrimHandle> boundsDirty;
    std::vector<PrimHandle> assetsDirty;

    bool empty() const { return primsResynced.empty() && transformDirty.empty() && boundsDirty.empty() && assetsDirty.empty(); }

    void clear() {
        primsResynced.clear();
        transformDirty.clear();
        boundsDirty.clear();
        assetsDirty.clear();
    }
};

struct AssetBindingCacheRecord {
    MeshHandle mesh;
    MaterialHandle material;
    uint32_t revision = 0;
};

// Prim flag bits
constexpr uint64_t PrimFlagRenderable = 1 << 0;
constexpr uint64_t PrimFlagLight = 1 << 1;
constexpr uint64_t PrimFlagCamera = 1 << 2;
constexpr uint64_t PrimFlagXformable = 1 << 3;

struct SceneEditRequestContext {
    enum class Purpose : uint8_t {
        Authoring,
        Preview,
        Procedural,
        Debug,
    } purpose = Purpose::Authoring;
};

class MeshLibrary;
class MaterialLibrary;

class USDScene {
public:
    USDScene();
    ~USDScene();

    USDScene(const USDScene&) = delete;
    USDScene& operator=(const USDScene&) = delete;

    // Lifecycle
    bool open(const char* path);
    // Create an anonymous in-memory stage with sane defaults (Y-up). No file backing
    // until the user saves. Useful for starting from scratch or launching ngen
    // without a scene argument.
    bool newScene();
    void close();
    bool isOpen() const;

    // Per-frame
    void beginFrame();
    void processChanges();
    void endFrame();
    const SceneDirtySet& dirtySet() const;

    uint32_t frameIndex() const;

    // Stage's authored up-axis as a world-space unit vector. USD natively supports
    // either Y-up or Z-up; the engine preserves whichever the stage declares rather
    // than forcing one convention. Callers that need a vertical reference (camera,
    // shadow lookAt, grid orientation) read this.
    glm::vec3 worldUp() const;

    // Filesystem directory of the stage's root layer (parent_path of the layer's
    // identifier). Returned as an absolute path; empty if no stage is open. The
    // asset browser scopes its scan here.
    std::string rootLayerDirectory() const;

    // Asset binding (call after processChanges, before extraction)
    void updateAssetBindings(MeshLibrary& meshLib, MaterialLibrary& matLib);

    // Prim access
    PrimHandle findPrim(const char* path) const;
    const PrimRuntimeRecord* getPrimRecord(PrimHandle h) const;
    const TransformCacheRecord* getTransform(PrimHandle h) const;
    const AssetBindingCacheRecord* getAssetBinding(PrimHandle h) const;
    const LightDesc* getLightDesc(PrimHandle h) const;
    std::span<const PrimRuntimeRecord> allPrims() const;

    // Hierarchy
    PrimHandle firstChild(PrimHandle h) const;
    PrimHandle nextSibling(PrimHandle h) const;

    // Layer management
    std::span<const SceneLayerInfo> layers() const;
    LayerHandle findLayerByRole(SceneLayerRole role) const;
    void setEditTarget(LayerHandle layer);
    LayerHandle currentEditTarget() const;
    LayerHandle sessionLayer() const;
    void clearSessionLayer();
    LayerHandle addSubLayer(const char* filepath);
    void setLayerMuted(LayerHandle layer, bool muted);
    bool saveLayer(LayerHandle layer);
    bool saveAllDirty();
    // Export the root layer's opinions to `path`. Intended for saving an anonymous
    // stage for the first time (File → Save As); SdfLayer::Export preserves the in-
    // memory layer identity, so subsequent saves still need to go through this path
    // until we wire up a proper "reopen after save" flow.
    bool exportRootLayerTo(const char* path) const;
    // True when the root layer has no filesystem backing (e.g. fresh newScene()).
    bool hasAnonymousRootLayer() const;

    // Editing — routes to correct layer based on purpose
    bool setTransform(PrimHandle h, const Transform& value, const SceneEditRequestContext& ctx = {});
    bool setVisibility(PrimHandle h, bool visible, const SceneEditRequestContext& ctx = {});
    // Author primvars:displayColor on a UsdGeomGprim (or legacy displayColor attr). Clears
    // the prim's cached mesh binding so the next extract re-tessellates with the new tint,
    // and marks the stage for resync so extract actually runs.
    bool setDisplayColor(PrimHandle h, const glm::vec3& color, const SceneEditRequestContext& ctx = {});
    // Current authored primvars:displayColor, or white if unauthored / non-gprim.
    glm::vec3 getDisplayColor(PrimHandle h) const;
    PrimHandle createPrim(const char* parentPath, const char* name, const char* typeName, const SceneEditRequestContext& ctx = {});
    PrimHandle createReferencePrim(const char* parentPath, const char* name, const char* referenceAsset, const SceneEditRequestContext& ctx = {});
    bool removePrim(PrimHandle h, const SceneEditRequestContext& ctx = {});

    // Return a child name under `parentPath` that doesn't collide with existing children.
    // Appends `_1`, `_2`, … to `baseName` until unique. Used by the Add Child menu so
    // creating two Sphere Lights in a row yields SphereLight / SphereLight_1.
    std::string uniqueChildName(const char* parentPath, const char* baseName) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
