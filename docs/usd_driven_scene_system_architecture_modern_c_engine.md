# USD-Driven Scene System Architecture (Modern C++ Engine)

## 1. Overview

If the goal is to fully embrace **USD features such as layering, composition, references, payloads, and variants**, then the earlier scene-system model needs to
be adjusted.

In that case, USD should no longer be treated only as an import format. Instead, it becomes a **core scene description model** that the engine understands
directly.

The architecture then shifts from:

```text
File Format → Import → Engine Scene → Renderer
```

to something closer to:

```text
USD Stage / Layers / Composition
    ↓
Engine Scene Model / Scene Runtime Bridge
    ↓
Evaluation / Queries / Overrides
    ↓
Render Extraction
    ↓
Renderer
```

This is a fundamentally different design.

The scene system is no longer just an engine-native hierarchy with an importer at the front. It becomes a **USD-aware scene runtime** that uses a USD stage as
an authoritative or semi-authoritative source of world state.

---

## 2. Core Design Decision

There are two broad models when designing a USD-aware engine scene system.

### Model A: USD as Authoritative Scene Model

The USD stage is the canonical scene description.

Engine systems:
- read from the stage
- write overrides into layers
- build transient runtime caches for simulation/rendering

This is the most natural model if you want to preserve real USD semantics.

### Model B: USD as Persistent Composition Model + Engine Runtime Mirror

The USD stage remains authoritative for authored scene structure, layering, references, and overrides.

The engine builds a runtime mirror:
- cached entities/components
- fast spatial indices
- render-friendly structures
- simulation-friendly state

This mirror is synchronized with the USD stage.

For most engines, **Model B is the practical sweet spot**.

It preserves USD concepts without forcing every runtime system to operate directly on raw USD primitives all the time.

---

## 3. Recommended High-Level Architecture

```text
USD Stage
 ├── Root layer
 ├── Sublayers
 ├── References
 ├── Payloads
 ├── Variants
 └── Session / override layers
        ↓
USD Scene Runtime
 ├── Stage access
 ├── Composition queries
 ├── Layer edit routing
 ├── Change tracking
 ├── Runtime mirror / caches
 └── Scene queries
        ↓
Render Extraction
        ↓
Renderer
```

And in engine terms:

```text
Engine
 ├── USD Scene System
 │    ├── Stage Manager
 │    ├── Layer Manager
 │    ├── Prim Runtime Cache
 │    ├── Transform / Spatial Cache
 │    ├── Scene Query System
 │    ├── Edit / Override System
 │    └── Render Extraction Bridge
 │
 ├── Gameplay / Tools / Editor
 └── Renderer
```

---

## 4. What Changes Compared to a Pure Engine-Native Scene System

A non-USD-native scene system usually owns:

- entities
- component stores
- transform hierarchy
- serialized scene files

A USD-driven scene system instead needs to think in terms of:

- stage
- prims
- layers
- composed values
- authored opinions
- references/payloads
- variants
- edit targets
- runtime caches derived from composed scene state

This changes the mental model from:

> "What components does this entity have?"

into:

> "What is the composed value for this prim, and where should edits be authored?"

That is the key shift.

---

## 5. USD Stage as the Scene Backbone

In a USD-driven engine, the stage becomes the backbone of the world.

### The stage provides
- namespace / hierarchy
- composition
- references and payloads
- variant selection
- property values
- metadata
- layering and overrides

### The engine adds
- fast runtime caches
- simulation state
- scene queries
- render extraction
- tool interaction
- change propagation

So the engine scene system should be thought of as:

```text
USD Stage + Runtime Systems + Caches
```

not as a plain replacement for USD.

---

## 6. Layering as a First-Class Engine Feature

If layering matters, it should be exposed directly in the engine architecture.

A layer-aware engine can support concepts like:

- base environment layer
- gameplay placement layer
- mission-specific override layer
- editor/session override layer
- user/local working layer
- procedural/generated layer

Example:

```text
World.usda
 ├── BaseEnvironment.usda
 ├── Props.usda
 ├── Gameplay.usda
 ├── LightingOverrides.usda
 └── SessionOverrides.usda
```

In this model, the engine should not flatten these into a single opaque asset too early.

Instead it should preserve:

- where values came from
- where edits should go
- what layer stack is active

This is the main reason to build a USD-driven scene system rather than a simple importer.

---

## 7. Engine Systems Needed for a USD-Driven Scene

A USD-driven scene system generally needs several subsystems that would not exist in a simple import-only pipeline.

### 7.1 Stage Manager
Responsible for:
- opening/closing stages
- managing root/session layers
- resolving asset paths
- load policies
- stage lifetime

### 7.2 Layer Manager
Responsible for:
- inspecting active layer stack
- selecting edit targets
- creating override/session layers
- tracking save state
- managing per-user/per-tool layers

### 7.3 Prim Runtime Cache
Responsible for:
- mapping prim paths to runtime records
- caching derived data
- tracking dirty state from USD changes
- accelerating common queries

### 7.4 Transform Cache
Responsible for:
- world transform evaluation
- dirty propagation
- efficient repeated queries

### 7.5 Spatial / Visibility Cache
Responsible for:
- bounds cache
- culling structures
- picking/raycast support

### 7.6 Scene Edit System
Responsible for:
- writing edits back to the correct layer
- creating/renaming/removing prims
- authoring overrides
- variant selection edits

### 7.7 Render Extraction Bridge
Responsible for:
- traversing the composed scene or runtime cache
- extracting renderable data
- producing render-world structures

---

## 8. Runtime Mirror vs Direct Stage Traversal

One of the biggest design questions is whether runtime systems should work directly on the USD stage.

### Direct stage traversal everywhere
Pros:
- simplest conceptual alignment with USD
- always sees latest composed data

Cons:
- expensive for hot runtime loops
- awkward for simulation and rendering
- difficult to structure for data-oriented processing

### Runtime mirror / caches
Pros:
- fast iteration for rendering and simulation
- easier spatial indexing
- easier data-oriented processing
- can still preserve USD semantics at the authoring/composition level

Cons:
- needs synchronization and dirty tracking

For an engine, the best answer is usually:

> USD is authoritative for scene description, but runtime systems operate on synchronized caches and extracted views.

---

## 9. Suggested Scene Runtime Model

A good USD-driven scene system often looks like this:

```text
USDScene
 ├── StageHandle
 ├── LayerStackState
 ├── PrimIndex / PrimCache
 ├── TransformCache
 ├── BoundsCache
 ├── SpatialIndex
 ├── RuntimeObjectMap
 ├── EditContext
 └── ExtractionInterfaces
```

### Important point

This is not a classic ECS-first design.

Instead, the USD scene system becomes the source of truth for authored object structure, and ECS-style runtime components may exist only for dynamic subsystems
that need them.

---

## 10. Prim-Centric Thinking

In a USD-driven scene, the fundamental object is often not an `Entity`, but a **prim path** or a stable prim handle.

Example:

```cpp
struct PrimHandle {
    uint32_t index;
};
```

or directly:

```cpp
using PrimPath = std::string;
```

The engine may still expose higher-level wrappers:

```cpp
class SceneObject {
public:
    PrimHandle prim() const;
};
```

But internally, identity and hierarchy should align with USD.

This is important if you want edits, overrides, and layer-aware tooling to behave naturally.

---

## 11. Editing Model

A USD-aware scene system must separate **composed values** from **authored edits**.

For example:

- the current transform might be the composed result of several layers
- a user moving an object in the editor should author an opinion into a specific target layer
- a gameplay session might write temporary changes into a session layer

So editing should go through something like:

```cpp
class SceneEditContext {
public:
    void setEditTarget(LayerHandle layer);
    void setPrimTransform(PrimHandle prim, const Transform& t);
    void setVariantSelection(PrimHandle prim, std::string_view set, std::string_view value);
};
```

This is one of the strongest arguments against flattening USD too early.

---

## 12. Session Layers and Runtime Overrides

Session layers are especially valuable for an engine/editor workflow.

Use cases:
- temporary editor moves
- debugging overrides
- non-destructive testing
- per-user scene edits
- play-mode changes that should not dirty source assets

Possible model:

```text
Authored layers
    ↓
User/session override layer
    ↓
Composed scene in editor/runtime
```

This gives a powerful non-destructive workflow.

It also suggests that the scene system should expose concepts like:

- persistent authored edit
- temporary session edit
- procedural override

---

## 13. How Gameplay Fits In

Gameplay is the hardest part of a USD-driven architecture.

Not all gameplay state belongs in USD.

A useful split is:

### USD-backed state
- authored transforms
- static scene structure
- placed objects
- light/camera/environment settings
- variants and references

### Engine-runtime-only state
- transient simulation state
- AI state
- health, inventory, quest state
- temporary particle instances
- runtime-only spawned objects

So the engine likely needs a **hybrid runtime**:

```text
USD-authored scene objects
+ engine runtime objects/state
```

This means the scene system should support both:
- prim-backed objects
- runtime-only objects or overlays

without forcing everything into authored USD layers.

---

## 14. Dynamic Objects and Procedural Content

A pure USD-driven design is strongest for authored world structure.

Dynamic runtime objects need special handling.

Options:

### Option A: Runtime-only objects outside USD
Best for:
- bullets
- VFX instances
- temporary gameplay objects

### Option B: Procedural/session-authored USD prims
Best for:
- editor-created objects
- saved runtime edits
- procedural tools where persistence matters

### Option C: Hybrid
Some systems create runtime-only objects first, and later commit them into USD layers when needed.

This hybrid model is often the most flexible.

---

## 15. Render Extraction in a USD-Driven System

The renderer still should not consume USD directly.

Instead:

```text
USD Stage
    ↓
USD Scene Runtime / Caches
    ↓
Render Extraction
    ↓
RenderWorld / Draw Packets
    ↓
Renderer
```

Extraction can operate from:
- composed prim queries
- runtime caches
- visibility/spatial indices

The render-facing data remains engine-native and flat.

That part does not change.

---

## 16. Resource and Asset References

In a USD-native architecture, many scene objects will reference external assets through USD paths and relationships.

The scene system therefore needs strong integration with the asset system.

Example:

```text
USD prim
 ├── mesh reference
 ├── material binding
 ├── texture paths
 └── payload/reference arcs
        ↓
Asset resolution system
        ↓
Loaded engine assets/resources
```

The stage describes relationships; the engine asset system resolves and loads runtime assets.

---

## 17. Spatial and Query System with USD

A USD stage alone is not enough for efficient runtime scene queries.

The engine still needs:
- bounds caches
- spatial index
- pick/raycast helpers
- visibility structures

These should be built from the composed stage and kept up to date through change processing.

Typical flow:

```text
USD changes
    ↓
Dirty prim detection
    ↓
Transform/bounds cache updates
    ↓
Spatial index updates
    ↓
Render/query systems consume updated state
```

---

## 18. Change Processing

A USD-driven runtime needs a robust change pipeline.

When a layer change occurs, the engine may need to:

- detect changed prims/properties
- update transform caches
- rebuild bounds for affected objects
- update asset bindings
- rebuild spatial index entries
- invalidate render extraction caches
- notify editor/game systems

This suggests a dedicated system like:

```cpp
class USDChangeProcessor {
public:
    void processStageChanges(USDScene& scene);
};
```

Without this, performance and correctness will suffer.

---

## 19. Suggested Engine Hierarchy

```text
Engine
 ├── USD Scene System
 │    ├── Stage Manager
 │    ├── Layer Manager
 │    ├── Edit Context
 │    ├── Prim Runtime Cache
 │    ├── Transform Cache
 │    ├── Bounds / Spatial Cache
 │    ├── Query System
 │    ├── Change Processor
 │    └── Render Extraction Bridge
 │
 ├── Asset System
 │    ├── Mesh / Material / Texture assets
 │    └── Asset Resolver for USD references
 │
 ├── Gameplay Runtime
 │    ├── Runtime-only objects
 │    └── Simulation state
 │
 └── Renderer
      ├── RenderWorld
      ├── RenderGraph
      ├── Resource System
      └── RHI
```

---

## 20. ECS in a USD-Driven Engine

If the engine uses ECS, it should be positioned carefully.

A good pattern is:

### USD for scene composition and authored structure
### ECS for dynamic runtime behavior

Possible integration patterns:

#### Pattern A: Prim-backed ECS proxies
Visible/interactive USD prims get corresponding ECS entities or runtime proxies.

#### Pattern B: ECS only for dynamic systems
The USD scene remains separate; ECS is used only for gameplay/runtime state.

#### Pattern C: Full mirroring
Every relevant prim mirrors into ECS.

This can work, but may become heavy and difficult to keep coherent if overused.

For most USD-centric engines, **partial mirroring is better than full duplication**.

---

## 21. tinyusdz vs OpenUSD in This New Design

This is where the previous recommendation changes significantly.

If you want a truly **USD-driven scene system with layering and real composition semantics**, then the parser-only view is not enough.

You now care about:
- stage composition
- layering behavior
- edit targets
- references and payloads
- variants
- session layers
- change processing
- authored vs composed values

That pushes the architecture closer to needing **full USD scene semantics**, not just file parsing.

So while tinyusdz may still be useful for lightweight import scenarios, it is much less obviously the right foundation for a USD-native scene runtime.

In this architecture, the stronger conceptual fit is:

- a library/runtime that supports actual USD composition and layer-aware editing
- not just reading `.usd` files into plain structs

So the practical takeaway is:

> tinyusdz is suitable for USD-as-import; a USD-driven scene system strongly favors a fuller USD implementation model.

---

## 22. Recommended Practical Direction

If you want layering to be central, the most practical architecture is:

### 1. Make USD stage/layer concepts first-class in the scene system
### 2. Build runtime caches on top for performance
### 3. Keep renderer extraction engine-native
### 4. Keep dynamic gameplay state partly outside USD
### 5. Treat authored edits, session edits, and procedural overrides separately

This gives you the power of USD without forcing the renderer or gameplay code to become USD-shaped everywhere.

---

## 23. Minimal Class Sketch

```cpp
class USDScene {
public:
    StageHandle stage() const;

    PrimHandle findPrim(std::string_view path) const;
    ComposedPrimData getPrimData(PrimHandle prim) const;

    void processChanges();
    void rebuildSpatialCache();

    SceneEditContext& edits();
    SceneQuerySystem& queries();
};
```

```cpp
class SceneLayerManager {
public:
    LayerHandle rootLayer() const;
    LayerHandle sessionLayer() const;
    void setEditTarget(LayerHandle layer);
};
```

```cpp
class USDRenderExtractor {
public:
    void extract(const USDScene& scene, RenderWorld& out, const ViewParams& view);
};
```

---

## 24. Mental Model

> A USD-driven scene system is not just an importer; it is a layer-aware scene runtime where the USD stage defines authored world structure and composition,
> while the engine builds fast caches, query systems, and render extraction on top of that composed scene.

---

## 25. Key Takeaways

- If layering matters, USD should move from the asset-import edge toward the core of the scene system
- The scene system should become **prim/layer/composition aware**
- Runtime caches are still necessary for transforms, bounds, spatial queries, and rendering
- The renderer should still consume extracted engine-native data, not raw USD structures
- Gameplay/runtime-only state should not all be forced into USD
- This direction reduces the appeal of a lightweight parser-only approach and increases the value of a fuller USD runtime model

