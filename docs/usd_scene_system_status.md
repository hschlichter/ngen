# USD Scene System — Architecture and Implementation Status

## 1. Context

The core architectural decision: **the USD stage IS the scene system**. There is no separate engine scene graph, entity system, or hierarchy. The composed
`UsdStage` owns world state directly. The engine builds runtime caches (transforms, bounds, spatial index) and render extraction on top of it. Layering is a
first-class engine feature.

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
- The render extraction layer does what Hydra's scene delegate does, but tailored to the engine's renderer

What IS borrowed from Hydra's design:

- Dirty-tracking pattern (`USDChangeListener` + `SceneDirtySet` mirrors Hydra's change tracking)
- Categorized scene primitives (geometry, lights, cameras as distinct extraction types)

### C++20 Containment

OpenUSD v26.03 headers are incompatible with GCC 15's libstdc++ in C++23 mode (`unique_ptr` incomplete type errors in `schemaRegistry.h`). All
`src/scene/usd*.cpp` files are compiled with `-std=c++20`. The rest of the engine stays C++23. The pimpl pattern in `usdscene.h` ensures no pxr headers leak
into engine-facing code.

### OpenUSD Build

OpenUSD is vendored as a git submodule at `external/openusd` (v26.03). Built separately with:

```bash
python3 external/openusd/build_scripts/build_usd.py \
  --no-python --no-imaging --no-tests --no-examples \
  --no-tutorials --no-tools --no-docs --no-materialx \
  --no-alembic --no-draco --no-openimageio --no-opencolorio \
  --no-openvdb --no-ptex --no-embree --no-prman \
  --onetbb --build-variant release \
  -j$(nproc) external/openusd_build
```

Output in `external/openusd_build/`. The Makefile links against these with rpath. `--onetbb` is required for modern compiler compatibility.

---

## 2. System Overview

```text
┌─────────────────────────────────────────────────────┐
│  USDScene (facade — pimpl, no pxr in header)        │
│                                                     │
│  ┌──────────────┐  ┌──────────────┐                 │
│  │ Layer        │  │ Stage        │                 │
│  │ Manager      │  │ Manager      │                 │
│  │              │  │              │                 │
│  │ - layer stack│  │ - UsdStagePtr│                 │
│  │ - edit target│  │ - open/close │                 │
│  │ - roles      │  │ - asset res. │                 │
│  └──────┬───────┘  └──────┬───────┘                 │
│         │                 │                          │
│         │    ┌────────────┴──────────┐               │
│         │    │ USDChangeListener     │               │
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
│  │ Edit API    │   │ SceneQuery       │              │
│  │             │   │ System           │              │
│  │ - edits via │   │                  │              │
│  │   UsdEdit   │   │ - raycast        │              │
│  │   Context   │   │ - frustum cull   │              │
│  │ - routing   │   │ - overlap        │              │
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

## 3. Implemented Architecture (Phases 0–4)

### 3.1 Handle Types — `src/scene/scenehandles.h`

```cpp
struct PrimHandle     { uint32_t index = 0; };
struct LayerHandle    { uint32_t index = 0; };
struct MeshHandle     { uint32_t index = 0; };
struct MaterialHandle { uint32_t index = 0; };
```

All use uint32_t index with zero as null. Explicit `operator bool()` and defaulted `operator==`.

### 3.2 Scene Types — `src/scene/scenetypes.h`

- `Transform` — position (vec3), rotation (quat), scale (vec3), with `toMat4()`
- `AABB` — min/max vec3, `valid()`, `transformed(mat4)`
- `Ray` — origin + direction
- `Frustum` — 6 planes

### 3.3 Mesh and Material Libraries — `src/scene/mesh.h`, `src/scene/material.h`

```cpp
struct MeshDesc {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

struct MaterialDesc {
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    int texWidth = 0, texHeight = 0;
    std::vector<uint8_t> texPixels;  // RGBA
};
```

`MeshLibrary` and `MaterialLibrary` store these by handle (1-based index). The renderer resolves handles through the libraries to get vertex/index/texture data
for GPU upload.

### 3.4 RenderWorld — `src/renderer/renderworld.h`

```cpp
struct RenderMeshInstance {
    MeshHandle mesh;
    MaterialHandle material;
    glm::mat4 worldTransform;
    AABB worldBounds;
};

struct RenderWorld {
    std::vector<RenderMeshInstance> meshInstances;
    std::vector<RenderLight> lights;
};
```

The renderer only ever sees `RenderWorld`. It never includes pxr headers. The boundary is absolute.

### 3.5 USDScene Facade — `src/scene/usdscene.h`

Single entry point for all scene access. Uses pimpl to keep pxr headers out of the engine.

**Public types defined in the header:**
- `SceneLayerRole` — Root, BaseWorld, Gameplay, Lighting, Procedural, UserWorking, Session, Unknown
- `SceneLayerInfo` — handle, identifier, displayName, role, dirty, readOnly, muted
- `PrimRuntimeRecord` — handle, parent, firstChild, nextSibling, path, name, flags, active, loaded
- `TransformCacheRecord` — local Transform, world mat4, lastFrame
- `AssetBindingCacheRecord` — MeshHandle, MaterialHandle, revision
- `SceneDirtySet` — vectors of dirty PrimHandles by category
- `SceneEditRequestContext` — Purpose enum (Authoring, Preview, Procedural, Debug)
- Prim flag constants: `PrimFlagRenderable`, `PrimFlagLight`, `PrimFlagCamera`, `PrimFlagXformable`

**Public API:**

```cpp
class USDScene {
    // Lifecycle
    bool open(const char* path);
    void close();
    bool isOpen() const;

    // Per-frame
    void beginFrame();
    void processChanges();
    void endFrame();

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
    bool saveLayer(LayerHandle layer);
    bool saveAllDirty();

    // Editing — routes to correct layer based on purpose
    bool setTransform(PrimHandle h, const Transform& value, const SceneEditRequestContext& ctx = {});
    bool setVisibility(PrimHandle h, bool visible, const SceneEditRequestContext& ctx = {});
    PrimHandle createPrim(const char* parentPath, const char* name, const char* typeName,
                          const SceneEditRequestContext& ctx = {});
    bool removePrim(PrimHandle h, const SceneEditRequestContext& ctx = {});
};
```

### 3.6 USDScene Implementation — `src/scene/usdscene.cpp`

All pxr usage is contained here. Compiled with `-std=c++20 -Wno-deprecated-declarations`.

**Internal structure (Impl):**

- **Stage management** — `UsdStage::Open()`, root/session layer setup, `metersPerUnit` from `UsdGeomGetStageMetersPerUnit()`
- **Change listener** — `TfWeakBase` subclass, registers `TfNotice` for `UsdNotice::ObjectsChanged`, accumulates changed `SdfPath`s behind a mutex, drained each
  frame
- **Prim cache** — Full rebuild on open via `UsdPrimRange::Stage()`. Classifies prims (`UsdGeomMesh` → Renderable,
  `UsdLuxBoundableLightBase`/`NonboundableLightBase` → Light, `UsdGeomXformable` → Xformable). Hierarchy stored as first-child/next-sibling linked list.
- **Transform cache** — Reads `UsdGeomXformable::GetLocalTransformation()`, computes world = parent.world * local. `metersPerUnit` applied as root scale. Dirty
  propagation marks entire subtrees.
- **Asset binding cache** — For renderable prims: extracts `UsdGeomMesh` geometry (points, faceVertexCounts, faceVertexIndices, normals, UVs via primvarsAPI),
  triangulates with fan triangulation, flips V coordinate for Vulkan (USD uses bottom-left origin). Extracts materials via
  `UsdShadeMaterialBindingAPI::ComputeBoundMaterial()` → follows `UsdPreviewSurface.diffuseColor` connection → `UsdUVTexture` file input → loads via
  `ArResolver::OpenAsset()` + stb_image.
- **Layer info** — Catalogs session layer, root layer, and sublayers with roles
- **Edit routing** — `chooseEditLayer()`: Preview/Debug → session layer, Authoring/Procedural → current edit target. Edit methods use scoped `UsdEditContext` to
  author opinions on the chosen layer.

**Key implementation details:**
- UV primvar lookup tries `st`, `st0`, `st1`, `UVMap` (different USD exporters use different names)
- Texture loading uses `ArResolvedPath` directly for .usdz archive paths
- Structural resyncs trigger full prim cache rebuild; property changes are categorized incrementally

### 3.7 Render Extractor — `src/scene/usdrenderextractor.h/.cpp`

Thin layer that reads from USDScene's public API. No pxr headers.

```cpp
void USDRenderExtractor::extract(const USDScene& scene, RenderWorld& out) {
    for (const auto& prim : scene.allPrims()) {
        if (!(prim.flags & PrimFlagRenderable)) continue;
        // read asset binding → mesh + material handles
        // read transform → world matrix
        // append RenderMeshInstance
    }
}
```

### 3.8 ImGui Debug UI — in `main.cpp`

- **USD Scene window** with layer stack panel (click to switch edit target, dirty/session indicators), Clear Session / Save All buttons, prim list with type
  tags

### 3.9 Frame Loop

```cpp
USDScene usdScene;
usdScene.open("scene.usda");
usdScene.updateAssetBindings(meshLib, matLib);
usdExtractor.extract(usdScene, renderWorld);
renderer.uploadRenderWorld(renderWorld, meshLib, matLib);

while (running) {
    usdScene.beginFrame();
    usdScene.processChanges();
    // ... input, camera ...
    renderer.render(cam, window);
    usdScene.endFrame();
}
```

---

## 4. Current File Layout

```text
src/
├── main.cpp                          — App entry, USD/glTF routing, ImGui layer UI
├── types.h                           — Vertex, MeshData, Scene (legacy glTF types)
├── camera.h/cpp                      — FPS camera
├── renderer/
│   ├── renderer.h/cpp                — uploadRenderWorld(), render loop
│   ├── renderworld.h                 — RenderMeshInstance, RenderLight, RenderWorld
│   ├── framegraph.h/cpp              — Declarative render pass system
│   ├── framegraphbuilder.h           — Pass setup
│   ├── framegraphcontext.h           — Pass execution
│   ├── framegraphresource.h          — Resource handles
│   ├── passnode.h                    — Pass node
│   └── resourcepool.h/cpp           — Transient texture pooling
├── scene/
│   ├── scenehandles.h                — PrimHandle, LayerHandle, MeshHandle, MaterialHandle
│   ├── scenetypes.h                  — Transform, AABB, Ray, Frustum
│   ├── mesh.h                        — MeshDesc, MeshLibrary
│   ├── material.h                    — MaterialDesc, MaterialLibrary
│   ├── usdscene.h                    — USDScene facade (no pxr headers)
│   ├── usdscene.cpp                  — Full USD implementation (C++20, pxr)
│   ├── usdrenderextractor.h/cpp      — Prim cache → RenderWorld
│   └── sceneloader.h/cpp            — glTF loader (legacy, still functional)
└── rhi/
    ├── rhidevice.h                   — Abstract GPU device
    ├── rhiswapchain.h                — Abstract swapchain
    ├── rhicommandbuffer.h            — Abstract command buffer
    ├── rhidebugui.h                  — Abstract debug UI
    ├── rhitypes.h                    — RHI enums, descriptors
    └── vulkan/                       — Vulkan 1.3 backend
```

---

## 5. Makefile Integration

```makefile
# OpenUSD
USD_DIR = external/openusd_build
USD_INCLUDE = -I$(USD_DIR)/include
USD_LDFLAGS = -L$(USD_DIR)/lib -Wl,-rpath,$(CURDIR)/$(USD_DIR)/lib \
    -lusd_usd -lusd_usdGeom -lusd_usdShade -lusd_usdLux \
    -lusd_sdf -lusd_pcp -lusd_tf -lusd_vt -lusd_gf -lusd_ar \
    -lusd_arch -lusd_plug -lusd_js -lusd_work -lusd_trace -lusd_pegtl -lusd_kind

# USD source files: compile with C++20
USD_SRCS = $(wildcard src/scene/usd*.cpp)
USD_OBJS = $(foreach obj, $(USD_SRCS:.cpp=.o), $(OUTDIR)/$(obj))

$(USD_OBJS): $(OUTDIR)/%.o: %.cpp
    $(CXX) -c -std=c++20 -O0 -g -Wall -MMD -fPIC ... -Wno-deprecated-declarations -o $@ $< $(INCLUDE) $(USD_INCLUDE)
```

Non-USD source files continue to use `-std=c++23`.

---

## 6. Phase Completion Status

| Phase | Status | Key Result |
|-------|--------|------------|
| 0 | Done | Core types, RenderWorld, renderer decoupled from scene format |
| 1 | Done | MeshDesc/MaterialDesc + handle-based libraries |
| 2 | Done | USDScene facade with stage, layers, prim cache, transform cache, change processing |
| 3 | Done | Full pipeline: USD → asset binding → render extraction → RenderWorld → Renderer. Textures via ArResolver + stb_image. metersPerUnit scaling. UV flip for Vulkan. |
| 4 | Done | Layer-aware edit API (setTransform, setVisibility, createPrim, removePrim) with routing by purpose. ImGui layer stack panel. |
| 5 | Not started | Bounds cache, spatial index, scene queries |

---

## 7. Remaining Phase: Spatial Index and Scene Queries

### Goal

Complete the runtime cache stack with per-prim world bounds, a spatial acceleration structure, and query APIs for picking, frustum culling, and overlap tests.
This enables the render extractor to skip offscreen prims and gives editor tools object picking.

### 7.1 Bounds Cache

Per-prim world AABB computed from mesh local bounds and the world transform. Updated incrementally when transforms or mesh geometry change.

**New file:** `src/scene/boundscache.h` / `.cpp`

```cpp
struct BoundsCacheRecord {
    AABB localBounds;
    AABB worldBounds;
    bool valid = false;
    uint32_t lastFrame = 0;
};

class BoundsCache {
public:
    void rebuild(const USDScene& scene, const MeshLibrary& meshLib);
    void updateDirty(const USDScene& scene, const MeshLibrary& meshLib,
                     std::span<const PrimHandle> dirtyPrims, uint32_t frame);

    const BoundsCacheRecord* get(PrimHandle h) const;
    std::span<const BoundsCacheRecord> all() const;

private:
    std::vector<BoundsCacheRecord> m_records;
};
```

**Local bounds source:** For each renderable prim, compute AABB from the mesh vertices in `MeshLibrary`. This avoids needing pxr headers — the bounds cache
operates on engine-side data only.

**World bounds:** `localBounds.transformed(worldTransform)` using the AABB::transformed() method from scenetypes.h.

**Update flow:**
1. On scene open → `rebuild()` computes all bounds
2. On transform dirty → recompute worldBounds for affected prims
3. On asset dirty → recompute localBounds from new mesh data, then worldBounds

### 7.2 Spatial Index

Acceleration structure for spatial queries. BVH (bounding volume hierarchy) is the recommended approach — good balance of build speed and query performance,
handles dynamic scenes with incremental updates.

**New file:** `src/scene/spatialindex.h` / `.cpp`

```cpp
class SpatialIndex {
public:
    void rebuild(const BoundsCache& bounds, std::span<const PrimRuntimeRecord> prims);
    void updateEntries(const BoundsCache& bounds, std::span<const PrimHandle> dirtyPrims);

    void queryFrustum(const Frustum& frustum, std::vector<PrimHandle>& outPrims) const;
    void queryRay(const Ray& ray, float maxDistance, std::vector<PrimHandle>& outPrims) const;
    void querySphere(glm::vec3 center, float radius, std::vector<PrimHandle>& outPrims) const;

private:
    // BVH nodes or grid cells
};
```

**Implementation options:**
- **BVH** — Binary tree of AABBs. Top-down build with SAH (surface area heuristic) or median split. Incremental refit when bounds change without topology
  changes. Full rebuild on structural resyncs.
- **Uniform grid** — Simpler, good for evenly distributed scenes. Less efficient for scenes with large scale variance.

BVH is recommended. For the initial implementation, a simple flat BVH with refit-on-dirty is sufficient.

### 7.3 Scene Query System

High-level query interface that combines bounds cache and spatial index.

**New file:** `src/scene/scenequery.h` / `.cpp`

```cpp
struct RaycastHit {
    PrimHandle prim;
    float distance = 0.0f;
    glm::vec3 position;
    glm::vec3 normal;
};

class SceneQuerySystem {
public:
    void rebuild(const USDScene& scene, const MeshLibrary& meshLib);
    void updateDirty(const USDScene& scene, const MeshLibrary& meshLib,
                     std::span<const PrimHandle> dirtyPrims, uint32_t frame);

    bool raycast(const Ray& ray, RaycastHit& outHit) const;
    void frustumCull(const Frustum& frustum, std::vector<PrimHandle>& outPrims) const;
    void overlapSphere(glm::vec3 center, float radius, std::vector<PrimHandle>& outPrims) const;

private:
    BoundsCache m_bounds;
    SpatialIndex m_spatial;
};
```

**Raycast pipeline:**
1. Broad phase: `spatialIndex.queryRay()` → candidate prims
2. Narrow phase: test ray against each candidate's world AABB for exact distance
3. Optional: test ray against actual mesh triangles for precise hits (needed for picking accuracy)

**Frustum culling pipeline:**
1. `spatialIndex.queryFrustum()` → visible prims
2. Used by `USDRenderExtractor` to skip offscreen geometry

### 7.4 Integration with Existing Systems

**USDScene changes:**
- Add `SceneQuerySystem` ownership or expose bounds/spatial as additional caches
- `processChanges()` propagates dirty sets to bounds cache and spatial index

**USDRenderExtractor changes:**
- Accept optional frustum parameter
- When provided, query visible prims from SceneQuerySystem instead of iterating all prims

**main.cpp changes:**
- Build frustum from camera projection + view matrices
- Pass to extractor for culled rendering
- Wire up raycast for mouse picking in ImGui debug UI

### 7.5 Frame Flow with Spatial Queries

```text
USDScene::beginFrame()
USDScene::processChanges()
    → prim cache update
    → transform cache update
    → bounds cache update (from dirty transforms + dirty assets)
    → spatial index refit (from dirty bounds)
USDScene::updateAssetBindings(meshLib, matLib)

// Frustum cull
frustum = buildFrustum(camera, projection)
sceneQuery.frustumCull(frustum, visiblePrims)

// Extract only visible
USDRenderExtractor::extract(scene, renderWorld, visiblePrims)

Renderer::render(renderWorld, camera)
USDScene::endFrame()
```

### 7.6 Verification

- Raycast: click in viewport → pick the correct prim, display its name/path in ImGui
- Frustum cull: rotate camera away from objects → confirm they're excluded from RenderWorld (track mesh instance count in ImGui)
- Overlap: query a sphere around a point → list nearby prims

### 7.7 Files Summary

| File | Purpose | Needs pxr? |
|------|---------|-----------|
| `src/scene/boundscache.h/cpp` | Per-prim AABB from mesh vertices + world transform | No |
| `src/scene/spatialindex.h/cpp` | BVH for spatial acceleration | No |
| `src/scene/scenequery.h/cpp` | Raycast, frustum cull, overlap queries | No |

None of these files need pxr headers — they operate entirely on engine-side cached data (PrimRuntimeRecord, TransformCacheRecord, MeshLibrary). They compile
with C++23 like the rest of the engine.

---

## 8. Key Risks

| Risk | Mitigation |
|------|-----------|
| OpenUSD large dependency | Submodule + separate build; only `src/scene/usd*.cpp` include pxr headers |
| C++23 / GCC 15 incompatibility | USD files compiled with C++20; pimpl isolates the boundary |
| glTF removal leaves no test content | glTF path kept alongside USD for now; offline conversion when ready |
| Per-frame full extraction cost | Fine at current scale; Phase 5 spatial index + frustum culling makes it incremental |
| BVH rebuild cost on structural resyncs | Full rebuild only on prim add/remove; refit handles transform/bounds changes |

---

## 9. Mental Model

> The USD stage is the scene. Layers decide where edits live. Runtime caches make it fast. Render extraction turns the composed result into flat data the
> renderer can consume. The renderer never knows USD exists.
