# Plan: Layer-Aware USD Scene System for ngen

## 1. Context

The engine currently has a flat glTF loader producing `std::vector<MeshData>` consumed directly by the renderer. glTF has always been intermediate — it will be
replaced by USD as the scene format.

The core architectural decision: **the USD stage IS the scene system**. There is no separate engine scene graph, entity system, or hierarchy. The composed
`UsdStage` owns world state directly. The engine builds runtime caches (transforms, bounds, spatial index) and render extraction on top of it. Layering is a
first-class engine feature. OpenUSD (pxr) is required, integrated via **prebuilt libraries**.

```text
UsdStage (layers, composition, hierarchy, transforms, bindings)
    ↓
Runtime Caches (transform cache, bounds cache, spatial index)
    ↓
Render Extraction → RenderWorld
    ↓
Renderer (frame graph, RHI)
```

### Why OpenUSD, Not tinyusdz

tinyusdz is a lightweight USD parser suitable for import-only workflows. It cannot provide the layering, composition, edit targets, session layers, and change
notifications the engine needs. Since layering is a first-class engine feature and the USD stage is the authoritative scene model, full OpenUSD (pxr) is
required.

### Why Not Hydra

Hydra is USD's built-in rendering architecture (`HdSceneDelegate → HdRenderIndex → HdRenderDelegate`). It provides change-tracked scene-to-renderer translation
with pluggable render backends.

It is not the right fit here because:

- ngen already has its own renderer, frame graph, and Vulkan RHI — Hydra would overlap and conflict with all three
- Writing a custom `HdRenderDelegate` targeting the engine's RHI means adapting to Hydra's rendering model instead of the engine's own
- Hydra's value is strongest for DCC viewport tools (Houdini, Maya) that need pluggable renderers, not a game engine with a dedicated pipeline
- The render extraction layer in this plan does what Hydra's scene delegate does, but tailored to the engine's renderer

What IS borrowed from Hydra's design:

- Dirty-tracking pattern (`USDChangeProcessor` + `SceneDirtySet` mirrors Hydra's change tracking)
- Categorized scene primitives (geometry, lights, cameras as distinct extraction types)

---

## 2. System Overview

Six systems, layered with clear data flow:

```text
┌─────────────────────────────────────────────────────┐
│  USDScene (facade)                                  │
│                                                     │
│  ┌──────────────┐  ┌──────────────┐                 │
│  │ USDLayer     │  │ USDStage     │                 │
│  │ Manager      │  │ Manager      │                 │
│  │              │  │              │                 │
│  │ - layer stack│  │ - UsdStagePtr│                 │
│  │ - edit target│  │ - open/close │                 │
│  │ - roles      │  │ - asset res. │                 │
│  └──────┬───────┘  └──────┬───────┘                 │
│         │                 │                          │
│         │    ┌────────────┴──────────┐               │
│         │    │ USDChangeProcessor    │               │
│         │    │                       │               │
│         │    │ TfNotice listener     │               │
│         │    │ → SceneDirtySet       │               │
│         │    └────────────┬──────────┘               │
│         │                 │                          │
│  ┌──────┴─────────────────┴──────────────────┐       │
│  │          Runtime Caches                   │       │
│  │                                           │       │
│  │  PrimCache ──→ TransformCache ──→ Bounds  │       │
│  │  (prim records)  (world mats)    Cache    │       │
│  │       │                            │      │       │
│  │       └──→ AssetBindingCache       │      │       │
│  │            (mesh/mat handles)      │      │       │
│  │                                    ↓      │       │
│  │                             SpatialIndex  │       │
│  └───────────────────────────────────────────┘       │
│                                                     │
│  ┌─────────────┐   ┌──────────────────┐              │
│  │ SceneEdit   │   │ SceneQuery       │              │
│  │ Context     │   │ System           │              │
│  │             │   │                  │              │
│  │ - edits via │   │ - raycast        │              │
│  │   UsdEdit   │   │ - frustum cull   │              │
│  │   Context   │   │ - overlap        │              │
│  └─────────────┘   └──────────────────┘              │
└─────────────────────────────────────────────────────┘
          │
          ↓
┌─────────────────────┐     ┌──────────────────┐
│ USDRenderExtractor  │────→│ RenderWorld      │
│                     │     │                  │
│ - walks prim cache  │     │ - mesh instances │
│ - reads caches      │     │ - lights         │
│ - produces flat     │     │ - cameras        │
│   render data       │     └────────┬─────────┘
└─────────────────────┘              │
                                     ↓
                            ┌────────────────┐
                            │ Renderer       │
                            │ (frame graph,  │
                            │  RHI, GPU)     │
                            └────────────────┘
```

---

## 3. Core Interfaces

### 3.1 USDScene — the facade everything goes through

```cpp
class USDScene {
public:
    // Lifecycle
    bool open(const char* path);
    void close();

    // Per-frame
    void beginFrame();
    void processChanges();   // TfNotice → dirty sets → cache updates
    void endFrame();

    // Prim access (goes through PrimCache)
    PrimHandle findPrim(const char* path) const;
    const PrimRuntimeRecord* getPrimRecord(PrimHandle h) const;
    const TransformCacheRecord* getTransform(PrimHandle h) const;
    const BoundsCacheRecord* getBounds(PrimHandle h) const;
    const AssetBindingCacheRecord* getAssetBinding(PrimHandle h) const;

    // Subsystems
    USDLayerManager& layers();
    SceneEditContext& edits();
    SceneQuerySystem& queries();

private:
    USDStageManager       m_stage;
    USDLayerManager       m_layers;
    USDChangeProcessor    m_changeProcessor;
    PrimCache             m_primCache;
    TransformCache        m_transformCache;
    BoundsCache           m_boundsCache;
    AssetBindingCache     m_assetBindings;
    SpatialIndex          m_spatialIndex;
    SceneEditContext       m_editContext;
    SceneQuerySystem       m_querySystem;
};
```

`USDScene` is the single entry point. External code (main loop, editor, tools) never reaches past it to touch the stage or caches directly. This keeps pxr
headers contained to the `src/scene/usd*.cpp` implementation files.

### 3.2 USDStageManager — owns the UsdStage

```cpp
class USDStageManager {
public:
    bool open(const char* path);   // UsdStage::Open()
    void close();

    UsdStageRefPtr stage() const;

    // Session layer is auto-created on open
    SdfLayerRefPtr rootLayer() const;
    SdfLayerRefPtr sessionLayer() const;
};
```

Thin wrapper around `UsdStage` lifetime. On `open()`, it opens the stage and creates an anonymous session layer. All other systems receive the `UsdStageRefPtr`
from here.

### 3.3 USDLayerManager — layer stack awareness and edit routing

```cpp
enum class SceneLayerRole : uint8_t {
    Root, BaseWorld, Gameplay, Lighting,
    Procedural, UserWorking, Session
};

struct SceneLayerInfo {
    LayerHandle    handle;
    SdfLayerRefPtr layer;
    std::string    displayName;
    SceneLayerRole role;
    bool           dirty;
    bool           muted;
};

class USDLayerManager {
public:
    void initialize(const USDStageManager& stage);

    // Inspection
    std::span<const SceneLayerInfo> layers() const;
    LayerHandle findByRole(SceneLayerRole role) const;

    // Edit target — which layer receives authored edits
    void setEditTarget(LayerHandle layer);
    LayerHandle currentEditTarget() const;

    // Session layer shortcuts
    LayerHandle sessionLayer() const;
    void clearSessionLayer();

    // Persistence
    bool saveLayer(LayerHandle layer);     // SdfLayer::Save()
    bool saveAllDirty();
};
```

On `initialize()`, walks the stage's `SdfLayerStack` and catalogs each layer with a role (derived from naming convention or metadata). Exposes the stack to
editor UI and to the `LayerEditRouter`.

### 3.4 USDChangeProcessor — stage changes → dirty sets

Listens to `TfNotice` from the stage. Each frame, collects what changed and tells caches what to rebuild.

```cpp
struct SceneDirtySet {
    std::vector<PrimHandle> primsResynced;    // added/removed/reordered
    std::vector<PrimHandle> transformDirty;
    std::vector<PrimHandle> boundsDirty;
    std::vector<PrimHandle> assetsDirty;
};

class USDChangeProcessor {
public:
    void initialize(UsdStageRefPtr stage);     // registers TfNotice listener
    void gatherChanges(SceneDirtySet& out);    // drains pending notices
};
```

On `initialize()`, registers a `TfNotice::Listener` for `UsdNotice::ObjectsChanged`. The listener callback accumulates changed `SdfPath`s into an internal
pending set. On `gatherChanges()`, it drains the pending set, resolves paths to `PrimHandle`s via the `PrimCache`, categorizes changes (transform vs asset vs
structural), and populates the `SceneDirtySet`. The dirty set then drives all cache updates downstream.

### 3.5 PrimCache — fast engine-side prim records

Maps `SdfPath` → `PrimHandle`. Stores flat arrays of prim records for fast iteration.

```cpp
struct PrimRuntimeRecord {
    PrimHandle handle;
    PrimHandle parent;
    SdfPath    path;
    std::string name;
    uint64_t   flags;        // renderable, light, camera, etc.
    bool       active;
    bool       loaded;
};

class PrimCache {
public:
    void rebuild(UsdStageRefPtr stage);                  // full rebuild on open
    void applyResyncs(const std::vector<PrimHandle>& resynced,
                      UsdStageRefPtr stage);              // incremental

    PrimHandle findByPath(const SdfPath& path) const;
    const PrimRuntimeRecord* get(PrimHandle h) const;
    std::span<const PrimRuntimeRecord> all() const;

    // Hierarchy traversal
    PrimHandle firstChild(PrimHandle h) const;
    PrimHandle nextSibling(PrimHandle h) const;
};
```

On `rebuild()`, traverses the composed stage (`UsdPrimRange`), creates a `PrimRuntimeRecord` for each prim, and populates the path→handle map. Sets `flags` by
checking prim type (`UsdGeomMesh` → renderable, `UsdLuxLight` → light, etc.). Hierarchy is stored as first-child/next-sibling linked list via handles.

On `applyResyncs()`, handles added/removed prims incrementally without full rebuild.

### 3.6 TransformCache — composed world transforms

Reads from stage via `UsdGeomXformable`, propagates world matrices down the hierarchy.

```cpp
struct TransformCacheRecord {
    Transform local;       // decomposed SRT
    Mat4      world;       // composed world matrix
    uint32_t  lastFrame;   // frame stamp for lazy eval
};

class TransformCache {
public:
    void update(const PrimCache& prims,
                UsdStageRefPtr stage,
                std::span<const PrimHandle> dirtyPrims,
                uint32_t frame);

    const TransformCacheRecord* get(PrimHandle h) const;
};
```

On `update()`, for each dirty prim: calls `UsdGeomXformable::GetLocalTransformation()` to get the composed local transform, decomposes into SRT, computes world
matrix as `parent.world * local`, and stamps the frame. Dirty propagation: when a prim's transform is dirty, all descendants are also marked dirty (walked via
`PrimCache` hierarchy).

### 3.7 BoundsCache — per-prim world AABB

```cpp
struct BoundsCacheRecord {
    AABB     localBounds;
    AABB     worldBounds;
    bool     valid;
    uint32_t lastFrame;
};

class BoundsCache {
public:
    void update(const PrimCache& prims,
                const TransformCache& transforms,
                UsdStageRefPtr stage,
                std::span<const PrimHandle> dirtyPrims,
                uint32_t frame);

    const BoundsCacheRecord* get(PrimHandle h) const;
};
```

Local bounds are computed from mesh extent (`UsdGeomMesh::GetExtentAttr()`). World bounds are transformed local bounds using the world matrix from
`TransformCache`. Updated whenever transforms or mesh geometry change.

### 3.8 AssetBindingCache — resolved mesh/material per prim

```cpp
struct AssetBindingCacheRecord {
    MeshHandle     mesh;
    MaterialHandle material;
    uint32_t       revision;
};

class AssetBindingCache {
public:
    void update(const PrimCache& prims,
                UsdStageRefPtr stage,
                MeshLibrary& meshLib,
                MaterialLibrary& matLib,
                std::span<const PrimHandle> dirtyPrims);

    const AssetBindingCacheRecord* get(PrimHandle h) const;
};
```

For each dirty prim with the renderable flag: reads `UsdGeomMesh` topology (points, indices, normals, UVs) and registers/updates in `MeshLibrary` to get a
`MeshHandle`. Reads `UsdShadeMaterialBindingAPI::GetBoundMaterial()`, translates `UsdPreviewSurface` parameters into engine `MaterialDesc`, registers in
`MaterialLibrary` to get a `MaterialHandle`.

### 3.9 USDRenderExtractor — caches → RenderWorld

Reads from all caches, produces flat renderer-consumable data. This replaces what Hydra's scene delegate would do.

```cpp
class USDRenderExtractor {
public:
    void extract(const USDScene& scene, RenderWorld& out);
};
```

Algorithm:

1. Iterate `primCache.all()` (or frustum-culled subset from `SceneQuerySystem`)
2. Filter to renderable prims (flag check on `PrimRuntimeRecord::flags`)
3. For each: read `assetBindingCache.get(h)` → mesh + material handles
4. Read `transformCache.get(h)` → world matrix
5. Read `boundsCache.get(h)` → world AABB
6. Append `RenderMeshInstance` to `RenderWorld`
7. Similarly extract lights (`UsdLuxLight` prims → `RenderLight`) and cameras

### 3.10 RenderWorld — flat data the renderer consumes

```cpp
struct RenderMeshInstance {
    MeshHandle     mesh;
    MaterialHandle material;
    Mat4           worldTransform;
    AABB           worldBounds;
};

struct RenderLight {
    LightType type;
    Vec3      color;
    float     intensity;
    Mat4      worldTransform;
};

struct RenderWorld {
    std::vector<RenderMeshInstance> meshInstances;
    std::vector<RenderLight>       lights;
};
```

The renderer only ever sees `RenderWorld`. It never includes pxr headers. The boundary is absolute — USD is an implementation detail of the scene system,
invisible to the renderer.

### 3.11 SceneEditContext — layer-aware edits

```cpp
struct SceneEditRequestContext {
    enum class Purpose : uint8_t {
        Authoring, Preview, Procedural, Debug
    } purpose = Purpose::Authoring;
};

class SceneEditContext {
public:
    explicit SceneEditContext(USDScene& scene);

    bool setTransform(PrimHandle h, const Transform& value,
                      const SceneEditRequestContext& ctx);
    bool setVisibility(PrimHandle h, bool visible,
                       const SceneEditRequestContext& ctx);
    bool setVariantSelection(PrimHandle h,
                             const char* variantSet,
                             const char* selection,
                             const SceneEditRequestContext& ctx);
    PrimHandle createPrim(const char* parentPath, const char* name,
                          const char* typeName,
                          const SceneEditRequestContext& ctx);
    bool removePrim(PrimHandle h, const SceneEditRequestContext& ctx);

private:
    LayerEditRouter m_router;
};
```

Each edit method: (1) calls `m_router` with the prim and request context to determine the target `SdfLayer`, (2) creates a `UsdEditContext` scoped to that
layer, (3) authors the opinion. The `TfNotice` system then picks up the change, and `processChanges()` propagates it through caches on the next frame.

### 3.12 LayerEditRouter — policy-based layer selection

```cpp
class LayerEditRouter {
public:
    SdfLayerRefPtr chooseLayer(PrimHandle prim,
                               const SceneEditRequestContext& ctx,
                               const USDLayerManager& layers) const;
};
```

Routing policy:

- `Purpose::Authoring` → `layers.currentEditTarget()` (the user's chosen working layer)
- `Purpose::Preview` / `Purpose::Debug` → `layers.sessionLayer()` (non-destructive, temporary)
- `Purpose::Procedural` → `layers.findByRole(Procedural)` (generated content layer)

### 3.13 SceneQuerySystem — spatial queries

```cpp
struct RaycastHit {
    PrimHandle prim;
    float      distance;
    Vec3       position;
    Vec3       normal;
};

class SceneQuerySystem {
public:
    bool raycast(const Ray& ray, RaycastHit& outHit) const;
    void frustumCull(const Frustum& frustum,
                     std::vector<PrimHandle>& outPrims) const;
    void overlapSphere(Vec3 center, float radius,
                       std::vector<PrimHandle>& outPrims) const;
};
```

Backed by `SpatialIndex` (BVH or uniform grid), which is updated incrementally from `BoundsCache` dirty sets.

---

## 4. Frame Loop — How It All Fits Together

```cpp
// In main.cpp
USDScene scene;
scene.open("world.usda");

USDRenderExtractor extractor;
RenderWorld renderWorld;
Renderer renderer;

while (running) {
    // 1. USD scene processes stage changes and updates caches
    scene.beginFrame();
    scene.processChanges();

    // 2. Extract flat render data from caches
    renderWorld.clear();
    extractor.extract(scene, renderWorld);

    // 3. Renderer consumes RenderWorld (knows nothing about USD)
    renderer.render(renderWorld, camera);

    scene.endFrame();
}
```

Inside `processChanges()`:

```text
TfNotice (accumulated since last frame)
    ↓
USDChangeProcessor::gatherChanges() → SceneDirtySet
    ↓
PrimCache::applyResyncs()           (structural changes)
    ↓
TransformCache::update()            (dirty transforms + descendants)
    ↓
BoundsCache::update()               (dirty bounds)
    ↓
AssetBindingCache::update()         (dirty mesh/material bindings)
    ↓
SpatialIndex::update()              (dirty bounds entries)
```

---

## 5. Implementation Phases

### Phase 0: Core Types and RenderWorld Extraction Layer

Establish engine-side types and decouple the renderer from any specific scene format.

**New files:**
- `src/scene/scenehandles.h` — `PrimHandle`, `LayerHandle`, `MeshHandle`, `MaterialHandle`
- `src/scene/scenetypes.h` — `Transform`, `AABB`, `Ray`, `Frustum`
- `src/renderer/renderworld.h` — `RenderMeshInstance`, `RenderLight`, `RenderWorld`

**Modified files:**
- `src/renderer/renderer.cpp` — `uploadScene()` → `uploadRenderWorld(const RenderWorld&)`. Renderer no longer knows about Scene or MeshData.
- `src/main.cpp` — glTF output temporarily adapted into `RenderWorld`. Throwaway bridging code.

**Verify:** Same glTF models render identically through RenderWorld.

---

### Phase 1: Material System Foundation

Handle-based materials that USD material bindings can target.

**New files:**
- `src/scene/material.h` / `.cpp` — `MaterialDesc`, `MaterialLibrary` with deduplication
- `src/scene/meshdata.h` — `MeshAssetData`, `MeshLibrary`

**Modified files:**
- `src/renderer/renderer.cpp` — Descriptor sets via MaterialLibrary

**Verify:** glTF with multiple materials renders correctly.

---

### Phase 2: OpenUSD Integration — Stage, Layers, and Caches

The USD stage becomes the scene. Integrate OpenUSD via **prebuilt pxr libraries**.

**New files:**
- `src/scene/usdscene.h` / `.cpp` — `USDScene` facade
- `src/scene/usdstagemanager.h` / `.cpp` — Stage lifecycle, root/session layers
- `src/scene/usdlayermanager.h` / `.cpp` — Layer stack, roles, edit targets, save/clear
- `src/scene/primcache.h` / `.cpp` — Prim records, path→handle mapping, hierarchy
- `src/scene/transformcache.h` / `.cpp` — Composed world transforms via `UsdGeomXformable`
- `src/scene/usdchangeprocessor.h` / `.cpp` — `TfNotice` → `SceneDirtySet` → cache updates

**Modified files:**
- `Makefile` — Link prebuilt OpenUSD libs, add pxr include paths
- `src/main.cpp` — Opens .usda via `USDScene::open()`, frame loop calls processChanges

**Key OpenUSD APIs used:**
- `UsdStage::Open()`, `GetRootLayer()`, `GetSessionLayer()`
- `SdfLayer::CreateNew()`, `Save()`, `GetSubLayerPaths()`
- `UsdEditContext` for directing edits to specific layers
- `UsdGeomXformable::GetLocalTransformation()` for composed transforms
- `TfNotice::Register()` with `UsdNotice::ObjectsChanged`

**Verify:** Open multi-layer .usda. Inspect layer stack in ImGui. Transforms update when sublayer overrides an xform.

---

### Phase 3: Render Extraction from USD Stage

Closes the loop: USD stage → caches → RenderWorld → Renderer. glTF removed from runtime.

**New files:**
- `src/scene/usdrenderextractor.h` / `.cpp` — Prim cache → `RenderWorld`
- `src/scene/assetbindingcache.h` / `.cpp` — Per-prim resolved mesh/material handles

**Modified files:**
- `src/main.cpp` — Full USD frame loop. glTF bridging code removed.
- `src/scene/sceneloader.h` / `.cpp` — **Removed** from runtime (or kept as offline glTF→USD converter)

**Key OpenUSD APIs used:**
- `UsdGeomMesh` for geometry extraction (points, normals, UVs, indices)
- `UsdShadeMaterialBindingAPI::GetBoundMaterial()` for material resolution
- `UsdShadeShader` / `UsdPreviewSurface` for material parameter extraction

**Verify:** .usda scene with meshes, materials, lights renders correctly. **Key vertical slice.**

---

### Phase 4: Layer-Aware Editing and Session Layers

Make layering useful — edits route to correct layers, non-destructive workflows.

**New files:**
- `src/scene/sceneeditcontext.h` / `.cpp` — Layer-aware edit API via `UsdEditContext`
- `src/scene/layereditrouter.h` / `.cpp` — Policy-based layer selection per edit purpose

**ImGui debug UI additions:**
- Layer stack panel (list layers with roles and dirty state)
- Current edit target indicator
- Session layer save/clear controls
- Per-prim: show which layer an opinion comes from

**Verify:** Move prim → edit in session layer. Save/reload persists. Clear restores base.

---

### Phase 5: Spatial Index and Scene Queries

Efficient spatial queries for picking and culling.

**New files:**
- `src/scene/boundscache.h` / `.cpp` — Per-prim world AABB
- `src/scene/spatialindex.h` / `.cpp` — BVH/grid, incremental update
- `src/scene/scenequery.h` / `.cpp` — Raycast, frustum cull, overlap

**Verify:** Raycast pick objects. Frustum cull large scene — only visible prims extracted.

---

## 6. Phase Summary

| Phase | What | Key Result |
|-------|------|------------|
| 0 | Core types + RenderWorld | Renderer decoupled from scene format |
| 1 | Material system | Handle-based materials with dedup |
| 2 | OpenUSD stage + layers + caches | USD IS the scene; layer-aware |
| 3 | Render extraction from USD | Full pipeline: USD → RenderWorld → Renderer; glTF removed |
| 4 | Layer-aware editing | Session layers, edit routing, editor UX |
| 5 | Spatial index + queries | Picking, frustum culling, spatial queries |

---

## 7. File Overview

### New files by phase

**Phase 0:**
- `src/scene/scenehandles.h`
- `src/scene/scenetypes.h`
- `src/renderer/renderworld.h`

**Phase 1:**
- `src/scene/material.h` / `.cpp`
- `src/scene/meshdata.h`

**Phase 2:**
- `src/scene/usdscene.h` / `.cpp`
- `src/scene/usdstagemanager.h` / `.cpp`
- `src/scene/usdlayermanager.h` / `.cpp`
- `src/scene/primcache.h` / `.cpp`
- `src/scene/transformcache.h` / `.cpp`
- `src/scene/usdchangeprocessor.h` / `.cpp`

**Phase 3:**
- `src/scene/usdrenderextractor.h` / `.cpp`
- `src/scene/assetbindingcache.h` / `.cpp`

**Phase 4:**
- `src/scene/sceneeditcontext.h` / `.cpp`
- `src/scene/layereditrouter.h` / `.cpp`

**Phase 5:**
- `src/scene/boundscache.h` / `.cpp`
- `src/scene/spatialindex.h` / `.cpp`
- `src/scene/scenequery.h` / `.cpp`

### Modified files

- `src/renderer/renderer.cpp` — Phase 0: consume RenderWorld; Phase 1: MaterialLibrary
- `src/main.cpp` — Every phase: wires pipeline; glTF bridging removed Phase 3
- `src/scene/sceneloader.cpp` — Removed Phase 3 (or offline glTF→USD converter)
- `src/types.h` — Phase 0: superseded by RenderWorld + USD
- `Makefile` — Phase 2: prebuilt OpenUSD libs and include paths

---

## 8. Key Risks

| Risk | Mitigation |
|------|-----------|
| OpenUSD large dependency | Prebuilt libs; only `src/scene/usd*.cpp` include pxr headers |
| OpenUSD linking complexity | Prebuilt libs avoid building from source |
| glTF removal leaves no test content | Offline glTF→USD conversion script for existing models |
| Per-frame full extraction cost | Fine at current scale; Phase 5 spatial index makes it incremental |

---

## 9. Mental Model

> The USD stage is the scene. Layers decide where edits live. Runtime caches make it fast. Render extraction turns the composed result into flat data the
> renderer can consume. The renderer never knows USD exists.
