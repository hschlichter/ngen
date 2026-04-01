# Render Graph & Resource System (Modern C++ Renderer)

## 1. Overview

A **render graph** is a data-driven system that describes *what work happens each frame* and *how GPU resources flow between passes*. It replaces hardcoded render pipelines with a dependency graph that the engine can analyze, optimize, and execute.

The render graph is tightly coupled with the **resource system** because:

- passes *produce and consume resources*
- resource lifetimes are *frame-local and often transient*
- synchronization (barriers) depends on *resource usage transitions*

Together, they form the core of modern renderer orchestration.

---

## 2. Core Concepts

### Nodes
- Represent **render passes** or **compute passes**

### Edges
- Represent **resource dependencies** between passes

### Resources
- Logical representations of textures/buffers
- Not necessarily backed by physical GPU memory until execution

### Execution
- Graph is compiled into an **ordered list of GPU commands**

---

## 3. Logical vs Physical Resources

### Logical (Graph-level)

```
TextureHandle gbufferAlbedo;
TextureHandle depth;
```

- Created declaratively
- Used to express dependencies
- No immediate GPU allocation

### Physical (RHI-level)

```
RHI::ITexture* texture;
```

- Allocated during graph compilation/execution
- May be reused via aliasing

---

## 4. Resource Lifetime & Aliasing

The graph enables **automatic lifetime tracking**:

```
Pass A → writes Texture X
Pass B → reads Texture X
Pass C → no longer uses Texture X
```

Texture X can be freed or aliased after Pass B.

### Aliasing

Different logical resources can share the same memory if lifetimes do not overlap.

Benefits:
- Reduced VRAM usage
- Better cache locality

---

## 5. Pass Definition Model

Modern render graphs often use a **builder pattern**.

```cpp
renderGraph.addPass("GBuffer", [&](PassBuilder& builder) {
    auto albedo = builder.createTexture({ ... });
    auto depth  = builder.createDepth({ ... });

    builder.write(albedo);
    builder.write(depth);

    builder.setExecute([=](RenderContext& ctx) {
        ctx.cmd->setPipeline(gbufferPipeline);
        ctx.cmd->draw(...);
    });
});
```

Key ideas:
- declare resources first
- declare read/write usage
- provide execution lambda

---

## 6. Resource Usage Tracking

Each pass declares how it uses resources:

```
builder.read(texture);
builder.write(texture);
builder.readWrite(buffer);
```

This allows the graph to:

- infer dependencies
- insert barriers automatically
- validate correctness

---

## 7. Graph Compilation

Before execution, the graph is compiled:

### Steps

1. **Dependency resolution**
2. **Topological sorting**
3. **Resource lifetime analysis**
4. **Physical resource allocation**
5. **Barrier generation**
6. **Pass culling (unused work removed)**

---

## 8. Barrier & Synchronization Model

The graph determines transitions automatically:

```
ColorAttachment → ShaderRead
TransferDst → ShaderRead
DepthWrite → DepthRead
```

Instead of manual barrier management, the graph inserts them based on usage.

---

## 9. Transient Resource System

Most graph resources are **transient**:

- valid only for one frame
- allocated from a pool
- recycled across frames

Example:

```
TransientTexturePool
 ├── allocate(desc)
 ├── free(handle)
 └── reuse via aliasing
```

---

## 10. Persistent Resources

Not all resources are transient:

### Persistent
- swapchain images
- shadow atlases (optional)
- history buffers (TAA, SSR)

These live outside the graph but are imported into it:

```cpp
builder.importTexture(swapchainImage);
```

---

## 11. Resource System Integration

The resource system typically has two layers:

### High-level (engine assets)
- TextureAsset
- MeshAsset
- MaterialAsset

### Graph-level
- Transient textures
- Frame-local buffers

### RHI-level
- Actual GPU allocations

---

## 12. Example Frame Graph

```
ShadowPass
    ↓
GBufferPass
    ↓
LightingPass
    ↓
PostProcessPass
    ↓
PresentPass
```

But internally represented as a dependency graph, not just a list.

---

## 13. Execution Model

At runtime:

```
for (auto& pass : compiledGraph) {
    beginPass(pass);
    pass.execute(context);
    endPass(pass);
}
```

The execution order is derived from dependencies.

---

## 14. Advanced Features

### Pass Culling
Unused passes are removed automatically.

### Async Compute
Graph can schedule compute passes on separate queues.

### Multi-Queue Support
- graphics queue
- compute queue
- transfer queue

### Parallel Recording
Command lists can be built in parallel.

---

## 15. Data-Oriented Design

Modern implementations avoid heavy inheritance:

- pass data stored in structs
- execution via function/lambda
- graph stored as arrays

Example:

```cpp
struct PassNode {
    ResourceHandle reads[8];
    ResourceHandle writes[8];
    ExecuteFn execute;
};
```

---

## 16. Minimal Class Structure

```cpp
class RenderGraph {
public:
    PassHandle addPass(...);
    void compile();
    void execute();

private:
    std::vector<PassNode> m_passes;
    ResourceRegistry m_resources;
};

class ResourceRegistry {
public:
    TextureHandle createTexture(...);
    BufferHandle createBuffer(...);

private:
    std::vector<ResourceDesc> m_resources;
};
```

---

## 17. Key Takeaways

- Render graph defines *frame work*, not rendering logic itself
- Resource system and graph are inseparable
- Most resources are transient and frame-local
- Dependencies and barriers should be automatic
- The system is fundamentally **data-driven**

---

## 18. Mental Model

Think of the render graph as:

> A compiler that transforms high-level rendering intent into optimized GPU command execution, while managing resource lifetimes and synchronization automatically.

