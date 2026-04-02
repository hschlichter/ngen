# Plan for a Layer-Aware USD Scene System (Modern C++ Engine)

## 1. Goal

Design a **USD-driven scene system** where **layering is a first-class engine concept**, while still preserving:

- efficient runtime queries
- fast render extraction
- clear edit routing
- separation between authored scene data and transient runtime state

This plan goes deeper on the earlier recommendation:

1. make USD stage/layer concepts first-class in the scene system
2. build runtime caches on top for performance
3. keep renderer extraction engine-native
4. keep dynamic gameplay state partly outside USD
5. treat authored edits, session edits, and procedural overrides separately

---

## 2. Core Design Principle

The most important architectural decision is this:

> The composed USD stage is the authoritative description of authored world state, but engine systems mostly operate on synchronized runtime caches and extracted views.

That gives the engine both:

- **USD correctness** for layering, references, variants, and edit targets
- **runtime performance** for culling, queries, rendering, and simulation support

---

## 3. Practical Engine Model

The engine should be organized into four layers of responsibility.

```text
USD Composition Layer
    ↓
Scene Runtime Layer
    ↓
Extraction / Query Layer
    ↓
Renderer / Gameplay Consumers
```

### 3.1 USD Composition Layer
Responsible for:

- stage lifetime
- root/session layer setup
- sublayer management
- references/payloads/variants
- authored vs composed value handling
- edit targets

### 3.2 Scene Runtime Layer
Responsible for:

- prim handles and lookup tables
- cached transforms
- cached bounds
- resolved asset bindings
- spatial index
- dirty tracking

### 3.3 Extraction / Query Layer
Responsible for:

- render extraction
- picking/raycast queries
- view visibility queries
- editor inspection

### 3.4 Consumers
Includes:

- renderer
- editor
- gameplay systems
- procedural tools
- debug systems

---

## 4. Scene Layering Model

If layering is key, the engine should expose explicit layer roles rather than treating layers as generic file internals.

A useful model is:

```text
Root Layer
 ├── Base world layers
 ├── Asset/reference layers
 ├── Mission / scenario layers
 ├── Lighting / lookdev layers
 ├── Procedural generated layers
 ├── User editable working layers
 └── Session override layer
```

### Recommended layer categories

#### Base Layers
Stable authored world content.

- terrain
- buildings
- static props
- environment setup

#### Domain Layers
Separated by function.

- gameplay placement
- lighting
- cinematics
- audio markers
- mission scripting metadata

#### Procedural Layers
Created by engine tools or world generation systems.

- generated foliage placement
- road decoration
- test scene population

#### User Working Layers
User-specific or task-specific authored changes.

- layout pass
- set dressing pass
- art polish pass

#### Session Layer
Temporary, non-destructive overrides.

- editor moves
- preview changes
- debug overrides
- play-in-editor changes

This categorization should be represented in engine code, not left as an informal naming convention.

---

## 5. Recommended Runtime Split

A layer-aware USD engine should distinguish three kinds of state.

### 5.1 Authored Scene State
Stored in USD layers.

Examples:

- object placement
- hierarchy
- asset references
- light settings
- camera settings
- environment values
- variants

### 5.2 Session / Override State
Stored in a session or temporary override layer.

Examples:

- preview transforms
- editor-only visibility toggles
- temporary variant switches
- debug edits

### 5.3 Runtime-Only State
Not authored to USD by default.

Examples:

- physics velocity
- health/ammo/inventory
- transient VFX
- bullets/projectiles
- AI internal state
- temporary spawned objects

This split is essential. Do not force all runtime state into USD.

---

## 6. Recommended Plan of Implementation

## Phase 1: Stage and Layer Foundations

Build the systems that make USD concepts first-class.

### Deliverables

- `USDScene` core object
- stage open/close/reload
- layer stack inspection
- session layer creation
- edit target selection
- prim lookup by path
- basic composed property reads

### Key rule

At the end of this phase, the engine should be able to answer:

- what layers are active?
- where did this value come from?
- where will an edit be written?
- what is the composed transform/asset binding for this prim?

### Suggested classes

```cpp
class USDScene;
class USDStageManager;
class USDLayerManager;
class SceneEditContext;
class PrimHandleTable;
```

---

## Phase 2: Runtime Caches and Dirty Processing

Build the fast runtime side that sits on top of USD.

### Deliverables

- prim runtime records
- transform cache
- bounds cache
- resolved asset binding cache
- dirty propagation pipeline
- spatial index integration

### Key rule

After this phase, hot loops should not need to walk raw composed scene data repeatedly.

### Suggested classes

```cpp
class PrimRuntimeCache;
class TransformCache;
class BoundsCache;
class AssetBindingCache;
class SpatialIndex;
class USDChangeProcessor;
```

---

## Phase 3: Query and Extraction Systems

Expose the scene to rendering and tools.

### Deliverables

- raycast/picking
- frustum visibility queries
- render extraction bridge
- camera/light extraction
- debug visualization hooks

### Key rule

The renderer should consume flattened extracted data, not raw USD objects.

### Suggested classes

```cpp
class SceneQuerySystem;
class VisibilitySystem;
class USDRenderExtractor;
class EditorSelectionBridge;
```

---

## Phase 4: Editing Workflow and Layer-Aware Tools

Make layering actually useful in practice.

### Deliverables

- per-layer edit routing
- move/rename/create/delete prim operations
- variant authoring
- user working layers
- save/reload of dirty layers
- editor UI for layer stack and edit target

### Key rule

Layering becomes valuable only when edits are predictable and controllable.

### Suggested classes

```cpp
class LayerEditRouter;
class SceneAuthoringService;
class VariantEditingService;
class LayerSaveService;
```

---

## Phase 5: Runtime Hybridization

Integrate dynamic engine runtime systems without breaking USD semantics.

### Deliverables

- runtime-only scene objects
- optional prim-backed gameplay proxies
- procedural layer writing policy
- commit/apply flow from runtime to authored layers when desired

### Key rule

Some objects belong in USD, some only in runtime. The engine must support both cleanly.

---

## 7. Proposed Core Types

Below is a more concrete sketch of the core runtime types.

### 7.1 Stable Prim Handle

The engine should not pass raw prim path strings everywhere in hot code.

```cpp
struct PrimHandle {
    uint32_t index = 0;

    explicit operator bool() const { return index != 0; }
    friend bool operator==(PrimHandle a, PrimHandle b) = default;
};
```

### 7.2 Layer Handle

```cpp
struct LayerHandle {
    uint32_t index = 0;

    explicit operator bool() const { return index != 0; }
    friend bool operator==(LayerHandle a, LayerHandle b) = default;
};
```

### 7.3 Prim Runtime Record

This is the cached engine-side representation of a USD prim.

```cpp
struct PrimRuntimeRecord {
    PrimHandle handle;
    PrimHandle parent;

    std::string path;
    std::string name;

    uint64_t flags = 0;
    uint32_t generation = 0;

    bool active = true;
    bool loaded = true;

    bool transformDirty = true;
    bool boundsDirty = true;
    bool assetDirty = true;
};
```

### 7.4 Transform Cache Record

```cpp
struct TransformCacheRecord {
    Transform local;
    Mat4 world;
    bool resetXformStack = false;
    uint32_t lastUpdatedFrame = 0;
};
```

### 7.5 Bounds Cache Record

```cpp
struct BoundsCacheRecord {
    AABB localBounds;
    AABB worldBounds;
    bool valid = false;
    uint32_t lastUpdatedFrame = 0;
};
```

### 7.6 Asset Binding Cache Record

```cpp
struct AssetBindingCacheRecord {
    MeshHandle mesh;
    MaterialHandle material;
    SkeletonHandle skeleton;
    uint32_t revision = 0;
};
```

---

## 8. Core Scene Object

The `USDScene` object should be the central façade for scene access.

```cpp
class USDScene {
public:
    bool open(const USDSceneOpenDesc& desc);
    void close();

    void beginFrame();
    void processChanges();
    void endFrame();

    PrimHandle findPrimByPath(std::string_view path) const;
    std::string_view getPath(PrimHandle prim) const;

    const PrimRuntimeRecord* getPrimRecord(PrimHandle prim) const;
    const TransformCacheRecord* getTransformRecord(PrimHandle prim) const;
    const BoundsCacheRecord* getBoundsRecord(PrimHandle prim) const;
    const AssetBindingCacheRecord* getAssetRecord(PrimHandle prim) const;

    SceneEditContext& edits();
    SceneQuerySystem& queries();
    USDLayerManager& layers();

private:
    std::unique_ptr<USDStageManager> m_stageManager;
    std::unique_ptr<USDLayerManager> m_layerManager;
    std::unique_ptr<PrimRuntimeCache> m_primCache;
    std::unique_ptr<TransformCache> m_transformCache;
    std::unique_ptr<BoundsCache> m_boundsCache;
    std::unique_ptr<AssetBindingCache> m_assetCache;
    std::unique_ptr<SpatialIndex> m_spatialIndex;
    std::unique_ptr<USDChangeProcessor> m_changeProcessor;
    std::unique_ptr<SceneEditContext> m_editContext;
    std::unique_ptr<SceneQuerySystem> m_querySystem;
};
```

This façade is useful because:

- tools can use it
- renderer extraction can use it
- gameplay bridges can use it
- underlying USD implementation can remain somewhat isolated

---

## 9. Stage and Layer Management

The layer manager should expose explicit engine-friendly operations.

```cpp
enum class SceneLayerRole : uint8_t {
    Root,
    BaseWorld,
    Gameplay,
    Lighting,
    Procedural,
    UserWorking,
    Session,
    Unknown
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
```

```cpp
class USDLayerManager {
public:
    LayerHandle rootLayer() const;
    LayerHandle sessionLayer() const;

    std::span<const SceneLayerInfo> layers() const;

    LayerHandle findByRole(SceneLayerRole role) const;
    LayerHandle createWorkingLayer(std::string_view name, SceneLayerRole role);

    bool setEditTarget(LayerHandle layer);
    LayerHandle currentEditTarget() const;

    bool saveLayer(LayerHandle layer);
    bool saveAllDirtyLayers();
};
```

This lets the engine present layers as meaningful objects, not just backend tokens.

---

## 10. Edit Routing

A strong layer-aware scene system needs explicit edit routing.

Example policy:

- moving a prop in layout mode writes to `UserWorking`
- changing preview visibility writes to `Session`
- procedural scatter tool writes to `Procedural`
- approved content move may later be baked/merged into a durable authored layer

Represent that with an edit router.

```cpp
struct SceneEditRequestContext {
    enum class Purpose : uint8_t {
        Authoring,
        Preview,
        Procedural,
        Runtime,
        Debug
    } purpose = Purpose::Authoring;

    bool persistent = true;
    bool userInitiated = true;
};
```

```cpp
class LayerEditRouter {
public:
    LayerHandle chooseLayerForTransformEdit(PrimHandle prim, const SceneEditRequestContext& ctx) const;
    LayerHandle chooseLayerForVariantEdit(PrimHandle prim, const SceneEditRequestContext& ctx) const;
    LayerHandle chooseLayerForPrimCreation(const SceneEditRequestContext& ctx) const;
};
```

This is important because the correct layer is a semantic decision, not just a technical one.

---

## 11. Scene Editing API

The authoring/edit API should separate **what you want to change** from **where it gets authored**.

```cpp
class SceneEditContext {
public:
    explicit SceneEditContext(USDScene& scene);

    bool setTransform(PrimHandle prim, const Transform& value, const SceneEditRequestContext& ctx);
    bool setVisibility(PrimHandle prim, bool visible, const SceneEditRequestContext& ctx);
    bool setVariantSelection(PrimHandle prim,
                             std::string_view variantSet,
                             std::string_view selection,
                             const SceneEditRequestContext& ctx);

    PrimHandle createPrim(std::string_view parentPath,
                          std::string_view name,
                          std::string_view typeName,
                          const SceneEditRequestContext& ctx);

    bool removePrim(PrimHandle prim, const SceneEditRequestContext& ctx);
};
```

This keeps tool/editor code clean.

---

## 12. Change Processing Pipeline

The scene system should explicitly process stage changes into runtime cache updates.

```text
USD notices / detected authored changes
    ↓
Changed prim/property collection
    ↓
Dirty flagging in runtime records
    ↓
Transform cache refresh
    ↓
Bounds refresh
    ↓
Asset binding refresh
    ↓
Spatial index update
    ↓
Render extraction invalidation
```

Suggested code structure:

```cpp
struct SceneDirtySet {
    std::vector<PrimHandle> transformDirty;
    std::vector<PrimHandle> boundsDirty;
    std::vector<PrimHandle> assetsDirty;
    std::vector<PrimHandle> structureDirty;
};
```

```cpp
class USDChangeProcessor {
public:
    void gatherChanges(USDScene& scene, SceneDirtySet& outDirty);
    void applyChanges(USDScene& scene, const SceneDirtySet& dirty);
};
```

This is where much of the runtime performance behavior will be determined.

---

## 13. Transform and Bounds Cache Sketch

These caches should be explicit systems, not incidental helper maps.

```cpp
class TransformCache {
public:
    void markDirty(PrimHandle prim);
    void update(const PrimRuntimeCache& prims,
                const USDStageManager& stage,
                std::span<const PrimHandle> dirtyPrims,
                uint32_t frameIndex);

    const TransformCacheRecord* get(PrimHandle prim) const;
};
```

```cpp
class BoundsCache {
public:
    void markDirty(PrimHandle prim);
    void update(const PrimRuntimeCache& prims,
                const TransformCache& transforms,
                const USDStageManager& stage,
                std::span<const PrimHandle> dirtyPrims,
                uint32_t frameIndex);

    const BoundsCacheRecord* get(PrimHandle prim) const;
};
```

These two systems are core to both scene queries and rendering.

---

## 14. Spatial Query Layer

Scene queries should operate on cached state, not raw stage traversal.

```cpp
struct RaycastHit {
    PrimHandle prim;
    float distance = 0.0f;
    Vec3 position;
    Vec3 normal;
};
```

```cpp
class SceneQuerySystem {
public:
    bool raycast(const Ray& ray, RaycastHit& outHit) const;
    void overlapSphere(Vec3 center, float radius, std::vector<PrimHandle>& outPrims) const;
    void collectVisible(const Frustum& frustum, std::vector<PrimHandle>& outPrims) const;
};
```

The implementation should be backed by the spatial index, which is refreshed from cache updates.

---

## 15. Render Extraction Bridge

The renderer should see an engine-native flat view.

```cpp
struct ExtractedRenderInstance {
    PrimHandle prim;
    MeshHandle mesh;
    MaterialHandle material;
    Mat4 worldTransform;
    AABB worldBounds;
    uint64_t sortKey = 0;
};
```

```cpp
struct ExtractedLight {
    PrimHandle prim;
    LightType type;
    Vec3 color;
    float intensity = 1.0f;
    Mat4 worldTransform;
};
```

```cpp
class USDRenderExtractor {
public:
    void extractView(const USDScene& scene,
                     const ViewParams& view,
                     RenderWorld& outWorld) const;
};
```

Suggested algorithm:

1. query visible prims from spatial system
2. filter to renderable/imageable prim types
3. resolve cached mesh/material bindings
4. fetch world transforms and bounds
5. build flat extracted records
6. hand off to draw-list building / renderer

This keeps the renderer cleanly separated from USD.

---

## 16. Dynamic Runtime Objects

To avoid forcing all transient state into USD, define a parallel runtime object model.

```cpp
using RuntimeObjectId = uint32_t;

struct RuntimeObject {
    RuntimeObjectId id = 0;
    Transform transform;
    RuntimeObjectFlags flags;
};
```

```cpp
class RuntimeObjectSystem {
public:
    RuntimeObjectId createObject(const RuntimeObjectDesc& desc);
    void destroyObject(RuntimeObjectId id);
};
```

Recommended usage:

- authored world content lives in USD scene
- bullets/VFX/etc live in runtime object system
- editor tools may optionally commit suitable runtime objects back into USD layers later

This hybridization prevents USD misuse.

---

## 17. Procedural Content Policy

Procedural systems should have an explicit writing policy.

Example options:

### Preview Mode
Writes only to session layer.

### Authoring Mode
Writes to procedural authored layer.

### Runtime Mode
Creates runtime-only objects.

Represent that clearly in code:

```cpp
enum class ProceduralOutputMode : uint8_t {
    SessionLayerPreview,
    AuthoredProceduralLayer,
    RuntimeOnly
};
```

This is critical when layering is a major workflow feature.

---

## 18. Editor UX Requirements from the Architecture

If layering is important, the scene/editor must surface it directly.

Minimum useful editor support:

- layer stack panel
- current edit target indicator
- authored vs composed value inspection
- per-prim source layer information
- dirty layer save controls
- mute/unmute layer controls
- session layer clear/apply controls

Without these, the architecture is technically correct but hard to use.

---

## 19. Where to Use ECS, If at All

A reasonable plan is:

### Do not make the authored USD scene itself be ECS-first
### Do use ECS or component-like systems for runtime-only simulation concerns
### Optionally create lightweight proxy components for selected prims

For example:

- rigid bodies may point back to `PrimHandle`
- selection state may point back to `PrimHandle`
- quest logic may reference scene paths or prim handles

This avoids a full one-to-one mirror of the entire stage into ECS.

---

## 20. Suggested Top-Level Modules

```text
Scene/
 ├── USDScene.h
 ├── USDSceneOpenDesc.h
 ├── PrimHandle.h
 ├── LayerHandle.h
 │
 ├── Stage/
 │    ├── USDStageManager.h
 │    ├── USDLayerManager.h
 │    ├── LayerEditRouter.h
 │    └── SceneEditContext.h
 │
 ├── Runtime/
 │    ├── PrimRuntimeCache.h
 │    ├── TransformCache.h
 │    ├── BoundsCache.h
 │    ├── AssetBindingCache.h
 │    ├── SpatialIndex.h
 │    └── USDChangeProcessor.h
 │
 ├── Query/
 │    ├── SceneQuerySystem.h
 │    └── VisibilitySystem.h
 │
 ├── Extraction/
 │    └── USDRenderExtractor.h
 │
 └── RuntimeObjects/
      └── RuntimeObjectSystem.h
```

This keeps the design coherent and scalable.

---

## 21. Example Frame Flow

A concrete engine frame could look like this:

```text
Begin frame
    ↓
Apply pending USD edits / tool changes
    ↓
Gather USD stage changes
    ↓
Update prim runtime cache
    ↓
Update transform cache
    ↓
Update bounds and asset bindings
    ↓
Update spatial index
    ↓
Run scene queries / selection / tools
    ↓
Extract render world
    ↓
Renderer builds frame
    ↓
End frame
```

This is the operational plan that ties the architecture together.

---

## 22. Recommended First Vertical Slice

A good first end-to-end prototype should support:

- open a USD scene with a root and session layer
- inspect layer stack
- find a prim by path
- move a prim via the editor into the session layer
- recompute transform and bounds caches
- query visible prims
- extract static meshes and lights into a render world
- draw them
- save/clear session edits

This slice validates the key design ideas without requiring the full engine to be complete.

---

## 23. Practical Recommendation About tinyusdz vs Full USD Runtime

In this deeper plan, layering is no longer a minor feature. It is central.

That means the engine needs strong support for:

- composition
- authored vs composed values
- layer stack behavior
- edit targets
- session layers
- live change processing

Because of that, a parser-only or importer-oriented approach is no longer the natural fit.

So the practical recommendation becomes:

> If layering and composition are central to the runtime scene system, the architecture should be designed around a fuller USD runtime model, with an internal abstraction layer to keep the engine code insulated from direct library spread.

That abstraction layer is important even if you later choose a specific USD implementation.

---

## 24. Key Takeaways

- layering must be surfaced as an engine feature, not just a file detail
- the composed stage should be authoritative for authored world state
- runtime caches are mandatory for performance
- edit routing needs explicit policy and code ownership
- render extraction must remain engine-native and flat
- dynamic runtime state should remain partly outside USD
- editor tooling must expose layer behavior clearly

---

## 25. Mental Model

> A layer-aware USD scene system is a composition runtime plus a set of engine caches and policies: USD decides what the world is, layers decide where edits live, caches make it fast, and extraction turns the result into something rendering and runtime systems can consume.

