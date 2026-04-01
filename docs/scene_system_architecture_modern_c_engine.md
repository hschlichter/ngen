# Scene System Architecture (Modern C++ Engine)

## 1. Overview

A **scene system** sits above the rendering system and is responsible for organizing the world in a form that gameplay, tools, simulation, and rendering can all use.

It should not be thought of as part of the renderer itself. Instead, it acts as the layer that owns and manages:

- entities and their relationships
- transforms and hierarchy
- renderable components
- lights, cameras, probes, volumes
- visibility and spatial organization
- the data that will later be transformed into render-facing structures

A useful mental model is:

```
Gameplay / Editor / Simulation
    ↓
Scene System
    ↓
Render Extraction / Render World
    ↓
Renderer / Render Graph / RHI
```

The scene system answers questions like:

- what exists in the world?
- where is it?
- how is it parented?
- what is visible to a camera?
- what data should be sent to the renderer this frame?

---

## 2. Main Responsibilities

A scene system usually owns or coordinates:

- entity lifetime
- transform propagation
- component storage
- spatial partitioning
- visibility data
- scene queries
- render extraction
- scene serialization
- editor interaction

It should provide a stable world model that systems can read from and update, while keeping the renderer insulated from gameplay/editor concerns.

---

## 3. High-Level Position in the Engine

```
Engine
 ├── Gameplay Systems
 ├── Physics
 ├── Animation
 ├── Audio
 ├── Editor Tools
 ├── Scene System
 │    ├── Entity System
 │    ├── Transform System
 │    ├── Spatial System
 │    ├── Visibility System
 │    ├── Camera / Light / Probe components
 │    └── Render Extraction
 └── Renderer
      ├── RenderScene / RenderWorld
      ├── RenderGraph
      ├── Resource System
      └── RHI
```

This separation is important:

- the **scene system** models the world
- the **renderer** draws a frame from extracted scene data

---

## 4. Scene System vs Render World

A common mistake is to make the renderer read directly from full gameplay scene objects.

That usually leads to:

- poor cache behavior
- too much coupling
- hard-to-manage lifetime issues
- renderer dependence on editor/game logic

A better split is:

### Scene System
Stores rich engine data:

- entity relationships
- authoring data
- gameplay components
- editor state
- simulation state

### Render World
Stores flattened rendering data:

- visible mesh instances
- light lists
- camera/view data
- draw packets
- GPU-friendly state

Flow:

```
Scene System
    ↓ extract / build
Render World
    ↓ consume
Renderer
```

---

## 5. Core Data Model

A practical scene system usually centers around entities and components.

### Minimal model

```text
Scene
 ├── Entities
 ├── Components
 ├── Hierarchy
 ├── Spatial index
 └── Scene subsystems
```

### Typical entity/component categories

- TransformComponent
- MeshRendererComponent
- LightComponent
- CameraComponent
- ReflectionProbeComponent
- EnvironmentComponent
- DecalComponent
- VolumeComponent
- AnimationComponent
- PhysicsBodyComponent
- ScriptComponent

The renderer should only care about a subset of these.

---

## 6. Entity Layer

The entity layer provides:

- stable IDs/handles
- creation and destruction
- tagging/naming
- parent/child relationships

Example:

```cpp
using Entity = uint32_t;

class Scene {
public:
    Entity createEntity();
    void destroyEntity(Entity e);

    bool isAlive(Entity e) const;
};
```

A handle-based design is usually better than raw pointers for world objects.

---

## 7. Transform System

The transform system is one of the most important parts of the scene.

Responsibilities:

- local transforms
- world transforms
- hierarchy propagation
- dirty tracking
- bounds updates

Example split:

```cpp
struct TransformComponent {
    Vec3 position;
    Quat rotation;
    Vec3 scale;
};

struct WorldTransformComponent {
    Mat4 world;
    bool dirty;
};
```

Hierarchy model:

```text
Parent Entity
 └── Child Entity
      └── Grandchild Entity
```

The scene system should update world transforms before render extraction.

---

## 8. Scene Hierarchy

Hierarchy is useful for:

- parenting objects
- attaching cameras/lights/meshes
- editor organization
- prefab grouping

But hierarchy should not become the only access pattern.

Use hierarchy for:

- transform inheritance
- logical grouping

Do not rely on it for:

- visibility queries
- fast rendering iteration
- large world search

Those need dedicated data structures.

---

## 9. Spatial System

A scene system usually needs a spatial index on top of raw components.

Examples:

- bounding volume hierarchy (BVH)
- octree
- grid / hashed grid
- loose octree
- portal/sector system

Used for:

- frustum culling
- ray casts / picking
- overlap queries
- nearest-neighbor search
- streaming decisions

Typical flow:

```text
Scene objects update bounds
    ↓
Spatial system updates index
    ↓
Visibility/query systems consume it
```

---

## 10. Visibility System

A visibility system determines what matters for a view.

This usually includes:

- frustum culling
- distance culling
- layer/mask filtering
- occlusion culling
- shadow caster selection

This often lives in the scene system or at the boundary between scene and renderer.

A clean split is:

- **scene system** provides bounds, masks, spatial queries
- **render extraction / renderer** builds per-view visible sets

---

## 11. Renderable Components

The scene contains authoring/runtime-facing render components.

Example:

```cpp
struct MeshRendererComponent {
    MeshHandle mesh;
    MaterialHandle material;
    AABB localBounds;
    RenderLayerMask layers;
    bool visible = true;
    bool castsShadow = true;
};
```

This is not yet a draw call.

During extraction, it may become one or more flat render packets.

---

## 12. Lights, Cameras, and Volumes

The scene system usually also owns components for:

### Cameras
- transform
- projection
- exposure settings
- output/view flags

### Lights
- directional, point, spot
- intensity/color
- shadow settings
- influence bounds

### Probes / Environment
- reflection probes
- irradiance volumes
- fog volumes
- post-process volumes

These are authored and updated in the scene system, then extracted into render-facing representations.

---

## 13. Render Extraction

This is one of the most important boundaries in the engine.

**Render extraction** transforms scene data into render data.

Example:

```text
Scene Entity + Components
    ↓
Extracted Render Instance
    ↓
Draw Packet / Visible Object
```

The extraction phase may:

- gather active cameras
- collect visible mesh renderers
- compute sort keys
- flatten transforms/material references
- build light lists
- prepare shadow casters
- generate render world data for the frame

A possible structure:

```cpp
class RenderExtractor {
public:
    void extract(const Scene& scene, RenderWorld& out, const ViewParams& view);
};
```

---

## 14. Render World Structure

A render world is a renderer-friendly snapshot or cache derived from the scene.

Example:

```text
RenderWorld
 ├── Cameras
 ├── Visible mesh instances
 ├── Draw packets
 ├── Lights
 ├── Shadow casters
 ├── Environment data
 └── Debug draw data references
```

This world is often rebuilt fully or partially each frame.

The key point is that it is **not** the full gameplay scene.

---

## 15. Scene Queries

The scene system should expose generic query mechanisms.

Examples:

- ray cast against scene bounds
- query all lights affecting a volume
- find entities in radius
- get visible renderables in frustum
- pick object under cursor

A query interface might look like:

```cpp
class SceneQuerySystem {
public:
    bool raycast(const Ray& ray, HitResult& out) const;
    void overlapSphere(Vec3 center, float radius, SmallVector<Entity>& out) const;
};
```

These queries are useful to gameplay, tools, editor, and rendering support systems.

---

## 16. Scene Serialization and Prefabs

A scene system often also handles:

- loading/saving scenes
- prefab instantiation
- stable references
- object naming and metadata

This is usually above the renderer, but it affects how scene entities and components are organized.

Example split:

```text
Scene Asset
 ├── serialized entities/components
 ├── prefab references
 └── environment settings
```

At runtime these become the live scene.

---

## 17. Editor Interaction

If the engine has tools/editor support, the scene system is usually the central model for:

- hierarchy view
- selection
- transform manipulation
- scene inspection
- undo/redo integration
- editor-only entities/components

The renderer should not own editor state directly.

Instead:

- editor modifies scene state
- scene extraction feeds renderer
- debug/gizmo systems visualize editor state

---

## 18. Threading and Update Order

A common frame order is:

```text
Gameplay update
    ↓
Animation / Physics update
    ↓
Transform propagation
    ↓
Bounds update
    ↓
Spatial index update
    ↓
Render extraction
    ↓
Renderer executes frame
```

This keeps scene data coherent before rendering begins.

In a more advanced engine, extraction may build a separate render snapshot to decouple simulation and rendering threads.

---

## 19. Common Architectural Variants

### Variant A: Simple Scene
Best for small engines.

- one Scene object
- entity/component storage
- direct extraction each frame

### Variant B: ECS + Scene Services
- ECS stores components
- scene services provide hierarchy, queries, spatial index
- renderer extracts from ECS views

### Variant C: Scene + Render Scene split
- rich live scene for gameplay/editor
- render scene cache optimized for drawing
- often best for medium/large engines

---

## 20. What the Scene System Should Not Do

Avoid making the scene system responsible for:

- raw GPU resource ownership
- command buffer recording
- backend-specific rendering logic
- low-level frame graph scheduling

Those belong in the renderer.

Similarly, avoid making the renderer responsible for:

- gameplay object ownership
- scene hierarchy editing
- authoring metadata
- prefab logic

---

## 21. Minimal Class Sketch

```cpp
class Scene {
public:
    Entity createEntity();
    void destroyEntity(Entity e);

    TransformSystem& transforms();
    SpatialSystem& spatial();
    SceneQuerySystem& queries();

    template <typename T>
    T* get(Entity e);
};
```

```cpp
class RenderExtractor {
public:
    void extractScene(const Scene& scene, RenderWorld& renderWorld, const CameraComponent& camera);
};
```

```cpp
class RenderWorld {
public:
    void clear();

    std::vector<RenderMeshInstance> meshInstances;
    std::vector<RenderLight> lights;
    std::vector<DrawPacket> drawPackets;
};
```

---

## 22. Suggested Engine Hierarchy

```text
Engine
 ├── Scene System
 │    ├── Entity Registry
 │    ├── Transform System
 │    ├── Hierarchy System
 │    ├── Spatial System
 │    ├── Scene Query System
 │    ├── Component Stores
 │    └── Render Extraction
 │
 └── Renderer
      ├── Render World
      ├── Culling / Draw List Builder
      ├── Render Graph
      ├── Resource System
      └── RHI
```

---

## 23. Key Takeaways

- A scene system is the engine's **world model**
- It lives above the renderer
- It owns entities, transforms, hierarchy, and spatial data
- It feeds a render-facing representation through **extraction**
- The renderer should consume **flattened render data**, not raw gameplay/editor objects

---

## 24. Mental Model

> The scene system describes what exists in the world and how it is organized; the renderer consumes an extracted, GPU-friendly view of that world to draw a frame.

