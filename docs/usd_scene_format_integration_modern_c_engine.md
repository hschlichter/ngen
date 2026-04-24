# USD Scene Format Integration (Modern C++ Engine)

## 1. Overview

If the engine already has a **scene system** above the renderer, then **USD** should usually be treated as a **scene description and interchange format**, not
as the engine's in-memory scene model.

That means the architecture should generally look like this:

```text
USD / USDA / USDC / USDZ files
    ↓
USD Import / Translation Layer
    ↓
Engine Scene Assets / Prefabs / Scene Instances
    ↓
Live Scene System
    ↓
Render Extraction
    ↓
Renderer
```

This is the most important design decision.

The engine should avoid making raw USD objects the core runtime scene representation unless there is a very strong toolchain reason to do so.

---

## 2. Main Role of USD in the Engine

In this context, USD is best used for one or more of these roles:

- **authoring/import format** for scenes and assets
- **interchange format** between DCC tools and the engine
- **prefab/level format** for static or semi-static content
- **editor serialization format** if the editor is built around scene composition

It is less ideal as:

- the low-level runtime ECS storage format
- the renderer's frame-facing representation
- the direct owner of gameplay state

A clean mental model is:

> USD describes content; the scene system owns live runtime state.

---

## 3. Recommended Engine Position

```text
Engine
 ├── Asset System
 │    ├── Mesh Assets
 │    ├── Material Assets
 │    ├── Texture Assets
 │    ├── Scene Assets
 │    └── USD Importer
 │
 ├── Scene System
 │    ├── Entities
 │    ├── Components
 │    ├── Hierarchy
 │    ├── Spatial System
 │    └── Runtime Scene Instances
 │
 ├── Editor / Tools
 │    ├── Scene Editing
 │    ├── Prefab Editing
 │    └── USD Export
 │
 └── Renderer
      ├── Render World
      ├── Render Graph
      ├── Resource System
      └── RHI
```

In this model:

- USD lives mainly in the **asset pipeline and tools layer**
- the **scene system** consumes translated data
- the **renderer** never talks directly to USD

---

## 4. Why USD Should Not Be the Renderer's Native Format

The renderer wants:

- flattened data
- GPU-friendly structures
- efficient per-frame extraction
- backend-agnostic resource references

USD provides:

- hierarchical scene description
- composition
- variants
- layers
- references
- metadata-rich authored data

These are valuable, but they are not the same thing as the renderer's runtime needs.

So the correct approach is usually:

```text
USD stage/prim hierarchy
    ↓ translate
Engine scene entities/components
    ↓ extract
Render world / draw packets
```

---

## 5. Recommended Integration Model

There are three common ways to integrate USD.

### Model A: Offline Import to Engine-Native Scene Assets

```text
USD file
    ↓ importer
Engine scene asset
    ↓ instantiate
Live scene
```

Best for:

- game runtime
- fast loading
- minimal dependency on USD at runtime
- engines with their own asset pipeline

Advantages:

- simpler runtime
- full control over memory layout
- no dependency on USD scene traversal during gameplay

Tradeoff:

- less direct fidelity to USD composition semantics at runtime

---

### Model B: Hybrid Import + Authoring Link

```text
USD file
    ↓ importer
Engine scene asset + source metadata
    ↓ instantiate
Live scene
```

Editor may retain a link back to USD source paths, prim paths, variants, or layers.

Best for:

- editor workflows
- reimport pipelines
- preserving source-of-truth data

Advantages:

- good toolchain flexibility
- runtime still stays engine-native

This is often the best general-purpose approach.

---

### Model C: Direct USD-Backed Scene Runtime

```text
USD stage
    ↓ live translation/query
Scene/runtime systems
```

Best for:

- DCC-like tools
- heavy scene composition workflows
- applications where USD itself is the product model

Tradeoffs:

- more complexity
- more runtime dependency on USD semantics
- more difficult gameplay/system integration
- can be overkill for a game-focused engine

For most engines, this is not the first design to choose.

---

## 6. What Should Be Imported from USD

A USD integration layer can map authored USD data into engine concepts.

Typical mappings:

### Scene Graph / Hierarchy
- USD prim hierarchy
- parent/child transforms
- named nodes

### Geometry
- meshes
- submeshes
- topology
- normals, UVs, tangents
- skinning data if supported

### Materials
- material bindings
- shader/material graph references
- parameters and textures

### Transforms
- local transforms
- xform ops
- inherited transforms

### Cameras
- projection data
- clipping planes
- focal properties

### Lights
- directional/point/spot/light metadata

### Instancing
- prototype/instance relationships

### Variants
- asset variants
- LOD or model variants if you choose to support them

### Metadata
- names, tags, custom properties, editor annotations

---

## 7. Engine-Side Representation After Import

The imported result should generally become one or more engine-native structures.

Example:

```text
USD Scene Asset
 ├── Node records
 ├── Transform records
 ├── Mesh instance records
 ├── Material bindings
 ├── Camera records
 ├── Light records
 ├── Variant metadata
 └── Custom property table
```

Instantiated into runtime scene as:

```text
Scene
 ├── Entities
 ├── TransformComponents
 ├── MeshRendererComponents
 ├── LightComponents
 ├── CameraComponents
 └── Metadata / tags
```

This keeps runtime systems simple.

---

## 8. USD and Prefabs / Scene Assets

USD fits naturally into a **scene asset** or **prefab** model.

A good design is:

```text
USD file
    ↓ import
SceneAsset
    ↓ instantiate
SceneInstance
```

Where:

### SceneAsset
- imported static description
- shared references to meshes/materials/textures
- hierarchy template
- optional USD source metadata

### SceneInstance
- runtime entities/components created from the asset
- transform overrides
- gameplay-added state

This is often cleaner than trying to mutate raw USD structures at runtime.

---

## 9. USD Composition vs Engine Composition

USD has rich composition semantics:

- references
- payloads
- sublayers
- variants
- inherits
- instancing

The engine must decide how much of this to preserve.

### Recommended approach

Preserve only the semantics that meaningfully help your workflow.

For example:

- **references** → imported child scene assets or prefabs
- **variants** → asset variant sets or editor-selectable options
- **instancing** → shared scene asset with multiple scene instances
- **custom metadata** → tags/property bags

Do not feel forced to reproduce every USD composition rule in the runtime scene system.

---

## 10. tinyusdz vs OpenUSD

If the goal is to use USD as a **scene file format** for a custom engine, then using **tinyusdz** instead of the full OpenUSD library can make sense depending
on scope.

### tinyusdz is appealing when:

- you want a smaller dependency footprint
- you mainly need parsing/import
- you do not need the full composition/editing/tooling stack
- you want tighter control over integration
- you prefer a simpler embeddable library

### OpenUSD is appealing when:

- you need full USD composition semantics
- you need parity with DCC/USD ecosystem behavior
- you need robust layering, variants, references, payloads, schemas
- you want stronger compatibility with large existing USD pipelines
- your editor/tooling is deeply USD-centric

So the right question is not just "USD or not," but:

> Do you need USD as a file parser and interchange layer, or do you need the full USD scene composition system at runtime and in tools?

---

## 11. Practical Recommendation on tinyusdz

For a custom engine with a separate scene system, **tinyusdz is often a reasonable first choice** if your initial goal is:

- importing USD/USDZ content
- mapping it into your own scene assets
- avoiding a large dependency stack

This works especially well if you treat USD as:

- import source
- asset exchange format
- optional editor serialization format

It is less suitable if you want:

- full fidelity USD composition behavior
- advanced stage editing workflows
- strong interoperability with complex studio pipelines without approximation

A sensible strategy is:

### Phase 1
Use tinyusdz as an importer/translator.

### Phase 2
Build engine-native scene assets and runtime instantiation.

### Phase 3
Evaluate whether full OpenUSD is actually required for your editor/toolchain.

This avoids overcommitting too early.

---

## 12. Recommended Module Layout

```text
Assets/
 ├── SceneAsset.h
 ├── MeshAsset.h
 ├── MaterialAsset.h
 ├── TextureAsset.h
 └── Importers/
      ├── USDImporter.h
      ├── USDSceneTranslator.h
      └── USDMaterialTranslator.h

Scene/
 ├── Scene.h
 ├── Entity.h
 ├── TransformSystem.h
 ├── SpatialSystem.h
 ├── SceneInstantiation.h
 └── SceneMetadata.h

Editor/
 ├── ReimportSystem.h
 ├── VariantEditor.h
 └── USDExport.h
```

The important split:

- **USDImporter** reads USD/tinyusdz/OpenUSD data
- **USDSceneTranslator** maps it into engine-native assets
- **SceneInstantiation** creates live entities/components from scene assets

---

## 13. Suggested Translation Boundary

A clean boundary is to introduce an intermediate imported scene description.

```cpp
struct ImportedSceneNode {
    std::string name;
    int parentIndex;
    Transform localTransform;
    MeshHandle mesh;
    MaterialHandle material;
    NodeFlags flags;
};

struct ImportedSceneAsset {
    std::vector<ImportedSceneNode> nodes;
    std::vector<ImportedLight> lights;
    std::vector<ImportedCamera> cameras;
    PropertyTable metadata;
};
```

Flow:

```text
USD parser
    ↓
ImportedSceneAsset
    ↓
Engine SceneAsset
    ↓
Runtime Scene Instance
```

This gives you:

- a stable importer boundary
- parser independence
- easier support for other formats later

This is a strong argument in favor of starting with tinyusdz.

---

## 14. Runtime Instantiation

Once imported, a scene asset can be instantiated into the live scene system.

```cpp
class SceneInstantiator {
public:
    SceneInstance instantiate(const SceneAsset& asset, Scene& scene);
};
```

Instantiation tasks:

- create entities
- attach transforms
- attach mesh renderer components
- attach lights/cameras
- set hierarchy
- apply metadata/tags

At this point, USD is no longer directly involved in rendering.

---

## 15. Variants and Overrides

USD variants are useful, but runtime handling should be explicit.

Possible mapping:

```text
USD variants
    ↓
Engine asset variants
    ↓
Scene instantiation options
```

Example:

- `modelVariant = damaged/clean`
- `lodVariant = high/medium/low`
- `materialVariant = snow/desert`

The engine may expose these as asset or prefab configuration options rather than raw USD variant APIs.

---

## 16. Instancing

USD supports instancing, and the engine should preserve that concept where useful.

Recommended runtime mapping:

- shared mesh/material assets remain shared
- repeated authored scene structures become scene asset instances or instanced renderables
- render extraction can later convert repeated mesh renderers into GPU instancing opportunities

This means USD instancing should first map into **scene-level sharing**, and then optionally into **render-level instancing**.

---

## 17. Materials and Shader Graph Concerns

Material translation is one of the hardest parts of any scene import pipeline.

USD may describe material relationships in ways that do not directly match your engine's material model.

Recommended approach:

- translate authored material bindings into engine material assets
- map recognized parameters into your material system
- preserve unsupported properties as metadata where possible
- avoid making the renderer understand USD material semantics directly

A clean split is:

```text
USD material description
    ↓
Material translator
    ↓
Engine MaterialAsset / MaterialInstance
```

---

## 18. Editor Workflow Considerations

If your editor wants reimport support, keep source links.

For example, a scene asset may store:

- source file path
- prim path mapping
- import settings
- chosen variants
- custom translation rules

This enables:

- reimporting from USD after DCC changes
- preserving editor overrides where possible
- maintaining stable asset pipelines

This hybrid design is usually more practical than making the live scene directly be a USD stage.

---

## 19. Serialization Strategy

There are two good strategies.

### Strategy A: USD as source, engine format as runtime/editor save

```text
USD source
    ↓ import
Engine asset format
    ↓ runtime/editor serialization
Engine-native scene save
```

### Strategy B: USD as both source and editor scene save

```text
Editor scene
    ↓ export/save
USD
```

Strategy A is simpler and usually better for a custom engine.

Strategy B only makes sense if the editor is deliberately USD-centric.

---

## 20. Recommended First Implementation

For your architecture, a strong first implementation would be:

### Use USD as an import and interchange format
### Keep runtime scene data engine-native
### Keep render data completely separate from USD
### Start with tinyusdz as the parser/reader
### Add OpenUSD later only if composition/tooling needs prove it necessary

That gives you:

- manageable complexity
- small dependency surface
- clean engine architecture
- future upgrade path

---

## 21. Suggested Final Architecture

```text
USD File (.usd/.usda/.usdc/.usdz)
    ↓
tinyusdz-based parser/importer
    ↓
ImportedSceneAsset
    ↓
Engine SceneAsset / Prefab
    ↓
Scene Instantiation
    ↓
Live Scene System
    ↓
Render Extraction
    ↓
Renderer / RenderGraph / RHI
```

This is likely the cleanest architecture for a custom engine that already distinguishes between:

- scene system
- render world
- render graph
- resource system

---

## 22. What to Avoid

Avoid these early design traps:

- making USD prim objects the runtime entity model
- making the renderer consume USD structures directly
- coupling gameplay systems to USD APIs
- reproducing every USD composition semantic in runtime unnecessarily
- adopting full OpenUSD before you know you actually need it

---

## 23. Key Takeaways

- USD should usually sit in the **asset/tooling layer**, not the renderer
- the **scene system** should remain engine-native
- a translation boundary is essential
- **tinyusdz** is a good fit if you mainly want parsing/import and a smaller integration surface
- **OpenUSD** becomes more attractive only if you need full composition/editor/stage behavior

---

## 24. Mental Model

> USD is the authored description of a scene; the engine imports that description into its own scene assets and runtime world model, then the renderer consumes
> extracted render data from that runtime scene.

