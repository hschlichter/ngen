# Render Graph Architecture (Modern C++ Engine)

## 1. Overview

This document outlines a practical, modern render graph architecture aligned with:

- explicit APIs (Vulkan / D3D12 / Metal)
- transient resource systems
- engine-level scene + extraction systems

---

## 2. High-Level Structure

```text
Renderer
 ├── FramePipeline
 ├── RenderGraph
 │    ├── Passes
 │    ├── Resources
 │    └── Compiler
 ├── ResourceSystem
 └── RHI
```

---

## 3. Core Components

### 3.1 RenderGraph

Responsible for:

- collecting passes
- tracking resources
- compiling execution plan
- executing passes

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
```

---

### 3.2 PassNode

Represents a single unit of GPU work.

```cpp
struct PassNode {
    std::vector<ResourceHandle> reads;
    std::vector<ResourceHandle> writes;
    ExecuteFn execute;
};
```

---

### 3.3 ResourceRegistry

Tracks logical resources.

```cpp
class ResourceRegistry {
public:
    TextureHandle createTexture(const TextureDesc&);
    BufferHandle createBuffer(const BufferDesc&);

private:
    std::vector<ResourceDesc> m_resources;
};
```

---

## 4. Build Phase

User code defines passes:

```cpp
rg.addPass("Lighting", [&](PassBuilder& builder) {
    auto gbuffer = builder.read(gbufferTex);
    auto output  = builder.write(colorTex);

    builder.setExecute([=](RenderContext& ctx) {
        ctx.cmd->draw(...);
    });
});
```

---

## 5. Compile Phase

Transforms graph into executable plan.

Steps:

1. dependency analysis
2. topological sort
3. lifetime calculation
4. resource allocation
5. barrier insertion

---

## 6. Execution Phase

```cpp
for (auto& pass : compiledPasses) {
    beginPass(pass);
    pass.execute(ctx);
    endPass(pass);
}
```

---

## 7. Resource Model

### Logical resources

- declared in graph
- used for dependency tracking

### Physical resources

- allocated during compile
- may be aliased

---

## 8. Integration with Resource System

```text
RenderGraph
    ↓ requests
TransientResourceAllocator
    ↓ creates
RHI resources
```

Resources are:

- frame-local
- pooled
- reused via aliasing

---

## 9. Barrier System

Barriers are derived from usage transitions:

```text
Write → Read
Read → Write
Write → Write
```

The graph inserts them automatically.

---

## 10. RenderGraph + Scene System

```text
Scene System
    ↓ extraction
RenderGraph
    ↓ execution
RHI
```

The graph consumes:

- draw packets
- light data
- camera/view data

---

## 11. Typical Pass Layout

```text
ShadowPass
GBufferPass
LightingPass
TransparentPass
PostProcessPass
PresentPass
```

Each is a node in the graph.

---

## 12. Transient Resource Allocator

```cpp
class TransientAllocator {
public:
    ResourceHandle allocate(const ResourceDesc&);
    void free(ResourceHandle);
};
```

Key feature:

- reuse memory across non-overlapping lifetimes

---

## 13. Frame Context

```cpp
struct FrameContext {
    CommandList cmd;
    DescriptorAllocator descriptors;
    UploadBuffer upload;
};
```

Used during execution.

---

## 14. Advanced Features

### Pass Culling
- remove unused passes

### Async Compute
- schedule compute passes separately

### Multi-Queue
- graphics / compute / transfer queues

---

## 15. Minimal Viable Implementation

Start with:

- pass list
- resource handles
- dependency sorting
- execution loop

Add later:

- barriers
- aliasing
- async execution

---

## 16. Design Principles

- data-oriented over inheritance
- explicit resource usage
- separation of declaration and execution
- backend-agnostic design

---

## 17. Suggested File Structure

```text
RenderGraph/
 ├── RenderGraph.h
 ├── PassNode.h
 ├── ResourceRegistry.h
 ├── Compiler.h
 ├── Executor.h
 └── TransientAllocator.h
```

---

## 18. Mental Model

> The render graph is a compiler that transforms pass declarations and resource usage into an optimized GPU execution schedule.

---

## 19. Key Takeaways

- render graph is central to modern renderer design
- simplifies synchronization and memory management
- integrates tightly with resource system
- should remain simple at its core

