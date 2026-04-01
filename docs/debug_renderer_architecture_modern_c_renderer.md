# Debug Renderer Architecture (Modern C++ Renderer)

## 1. Overview

A **debug renderer** is a specialized rendering subsystem responsible for visualizing diagnostic information such as:

- lines, boxes, spheres
- collision shapes
- camera frusta
- light bounds
- navigation meshes
- editor gizmos

It is designed to be **decoupled from gameplay systems and the low-level GPU API**, while integrating tightly with the renderer and render graph.

---

## 2. Core Responsibilities

A debug renderer typically handles:

- collecting debug draw requests from across the engine
- managing lifetime of debug primitives (frame-local and timed)
- batching primitives efficiently
- allocating transient GPU resources
- inserting debug passes into the render graph

---

## 3. High-Level Architecture

```
Engine Systems (physics, gameplay, editor)
    ↓
DebugDraw API
    ↓
DebugDrawSystem (collector)
    ↓
Renderer::DebugRenderer
    ↓
RenderGraph (debug passes)
    ↓
RHI / Backend
```

---

## 4. Separation of Concerns

### Engine-facing API

The engine interacts with a simple interface:

```cpp
class IDebugDraw {
public:
    virtual void line(Vec3 a, Vec3 b, Color color, float duration = 0.0f) = 0;
    virtual void box(const AABB& box, Color color, float duration = 0.0f) = 0;
    virtual void sphere(Vec3 center, float radius, Color color, float duration = 0.0f) = 0;
};
```

This layer:
- records requests
- does not perform rendering
- is accessible from any system

---

### Collector Layer

Stores debug primitives for the current frame:

```
DebugDrawSystem
 ├── transient primitives
 ├── timed primitives
 ├── category filtering
 └── per-view filtering
```

Example data:

```cpp
struct DebugLine {
    Vec3 a, b;
    Color color;
    bool depthTest;
    float remainingTime;
};
```

---

### Renderer Integration

The `DebugRenderer` lives inside the renderer module and is responsible for:

- consuming collected debug data
- batching geometry
- uploading GPU buffers
- registering render graph passes

```
Renderer
 ├── SceneRenderer
 ├── DebugRenderer
 └── RenderGraph
```

---

## 5. Relation to Render Graph

The debug renderer contributes passes to the render graph.

Typical passes:

```
DebugWorldPass
DebugOverlayPass
```

### DebugWorldPass

- world-space primitives
- optionally depth-tested
- examples: physics shapes, bounds, frusta

### DebugOverlayPass

- screen-space or overlay rendering
- no depth testing
- examples: gizmos, UI helpers, labels

---

## 6. Frame Integration

Example frame order:

```
ShadowPass
GBufferPass
LightingPass
TransparentPass
DebugWorldPass
PostProcessPass
DebugOverlayPass
PresentPass
```

Placement depends on desired visual behavior.

---

## 7. Resource System Integration

The debug renderer relies heavily on transient resources:

```
DebugRenderer Resources
 ├── transient vertex buffers (lines)
 ├── transient instance buffers (shapes)
 ├── uniform buffers (per view)
 ├── pipeline states
 └── optional font/text resources
```

These are:
- allocated per frame
- recycled via pools
- managed by the resource system

---

## 8. Batching Strategy

Debug rendering should minimize draw calls via batching:

- line batching (single dynamic vertex buffer)
- instanced shapes (box, sphere, cone)
- shared pipelines

Example:

```cpp
class DebugLineBatcher {
public:
    void addLine(...);
    void upload(...);
    void draw(...);
};
```

---

## 9. Categories and Filtering

Debug rendering is often filtered by category:

```cpp
enum class DebugCategory {
    Physics,
    AI,
    Gameplay,
    Editor,
    Rendering
};
```

Supports:
- enabling/disabling categories
- per-view visibility

---

## 10. Lifetime Management

Two main modes:

### Immediate (frame-local)

```
debug.line(a, b, color);
```

### Timed (persistent)

```
debug.sphere(pos, 1.0f, color, 5.0f);
```

Handled by the collector system, not the render graph.

---

## 11. Gizmos vs Debug Primitives

### Debug Primitives

- diagnostic
- engine/runtime use
- simple rendering

### Gizmos

- interactive
- editor-only
- require picking and state

Suggested split:

```
DebugRenderer
 ├── WorldDebugRenderer
 └── GizmoRenderer
```

---

## 12. Minimal Class Structure

```cpp
class DebugDrawSystem {
public:
    void line(...);
    void newFrame();
    void update(float dt);

    const DebugFrameData& data() const;
};

class DebugRenderer {
public:
    void prepare(const DebugFrameData& data);
    void addPasses(RenderGraph& rg);
};
```

---

## 13. What Not To Do

Avoid:

- putting debug logic in the RHI
- calling Vulkan/D3D directly from gameplay systems
- mixing debug rendering into core render passes
- deep inheritance hierarchies

---

## 14. Key Takeaways

- Debug rendering is a **renderer feature**, not a core engine system
- Submission and rendering should be separated
- It should integrate with the **render graph** via passes
- It relies heavily on the **resource system** for transient data
- It should remain flexible, data-driven, and backend-agnostic

---

## 15. Mental Model

> The debug renderer is a bridge between engine diagnostics and the GPU, translating simple draw requests into efficient, batched rendering integrated into the frame pipeline.

