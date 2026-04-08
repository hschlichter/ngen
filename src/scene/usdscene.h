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
    BaseWorld,
    Gameplay,
    Lighting,
    Procedural,
    UserWorking,
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
    void close();
    bool isOpen() const;

    // Per-frame
    void beginFrame();
    void processChanges();
    void endFrame();

    uint32_t frameIndex() const;

    // Asset binding (call after processChanges, before extraction)
    void updateAssetBindings(MeshLibrary& meshLib, MaterialLibrary& matLib);

    // Prim access
    PrimHandle findPrim(const char* path) const;
    const PrimRuntimeRecord* getPrimRecord(PrimHandle h) const;
    const TransformCacheRecord* getTransform(PrimHandle h) const;
    const AssetBindingCacheRecord* getAssetBinding(PrimHandle h) const;
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

    // Editing — routes to correct layer based on purpose
    bool setTransform(PrimHandle h, const Transform& value, const SceneEditRequestContext& ctx = {});
    bool setVisibility(PrimHandle h, bool visible, const SceneEditRequestContext& ctx = {});
    PrimHandle createPrim(const char* parentPath, const char* name, const char* typeName, const SceneEditRequestContext& ctx = {});
    bool removePrim(PrimHandle h, const SceneEditRequestContext& ctx = {});

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
