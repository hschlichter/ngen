# Modern Multi-Backend 3D Renderer Architecture (C++)

## 1. Overview

A modern renderer is best structured as a **layered system**, not a deep inheritance hierarchy.

```
Application / Game
    ↓
Scene / ECS / Gameplay systems
    ↓
Renderer front-end
    ↓
Render graph / frame pipeline
    ↓
RHI (Rendering Hardware Interface)
    ↓
Backend (Vulkan / D3D12 / Metal)
    ↓
GPU
```

---

## 2. Core Principles

- Prefer **composition over inheritance**
- Separate **engine logic from GPU logic**
- Use a **backend-agnostic API (RHI)**
- Flatten scene data into **draw packets** before rendering
- Use **handles + managers** instead of raw ownership

---

## 3. High-Level Structure

### Engine Side

```
Engine
 ├── Scene
 ├── CameraSystem
 ├── MeshSystem
 ├── MaterialSystem
 └── Renderer
      ├── RenderWorld
      ├── FramePipeline
      ├── RenderGraph
      ├── ResourceManager
      └── RHI::IDevice
```

---

## 4. RHI Layer

Backend-independent GPU abstraction:

```
RHI
 ├── IInstance
 ├── IAdapter
 ├── IDevice
 ├── ISwapchain
 ├── IQueue
 ├── ICommandList
 ├── IBuffer
 ├── ITexture
 ├── ISampler
 ├── IShaderModule
 ├── IGraphicsPipeline
 ├── IComputePipeline
 ├── IDescriptorSet
```

---

## 5. Backend Implementations

```
Vulkan
 ├── VkDevice : IDevice
 ├── VkBuffer : IBuffer
 ├── VkTexture : ITexture

D3D12
 ├── D3D12Device : IDevice
 ├── D3D12Buffer : IBuffer
 ├── D3D12Texture : ITexture

Metal
 ├── MetalDevice : IDevice
 ├── MetalBuffer : IBuffer
 ├── MetalTexture : ITexture
```

---

## 6. Three-Level Mental Model

### Level A: Render Features
- MeshRenderer
- Lighting
- Shadows
- Post-processing

### Level B: Frame Orchestration
- RenderGraph
- FramePipeline
- Pass scheduling

### Level C: GPU Abstraction (RHI)
- Buffers
- Pipelines
- Command lists
- Draw calls

---

## 7. Example Interfaces

```cpp
class IDevice {
public:
    virtual std::unique_ptr<IBuffer> createBuffer(...) = 0;
    virtual std::unique_ptr<ITexture> createTexture(...) = 0;
    virtual std::unique_ptr<ICommandList> createCommandList(...) = 0;
};

class ICommandList {
public:
    virtual void begin() = 0;
    virtual void drawIndexed(...) = 0;
};
```

---

## 8. Resource System

### CPU Side
- MeshAsset
- TextureAsset
- MaterialAsset

### GPU Side
- Buffer
- Texture
- Pipeline

### Material Model

```
Material
 ├── Template
 ├── Instance
 └── GPU Proxy
```

---

## 9. Render Graph

Modern renderers use a **render graph** instead of fixed pipelines.

```
RenderGraph
 ├── ShadowPass
 ├── GeometryPass
 ├── LightingPass
 ├── PostProcessPass
 └── PresentPass
```

---

## 10. Scene → Draw Data

Scene objects are converted into flat structures:

```
Entity
 └── MeshRenderer

↓

MeshDrawPacket
 ├── Buffers
 ├── Material
 ├── Transform
 └── Pipeline state
```

---

## 11. Handle-Based Design

```cpp
using TextureHandle = uint32_t;

class TextureManager {
public:
    TextureHandle create(...);
    ITexture* get(TextureHandle);
};
```

Benefits:
- Better lifetime control
- Easier backend switching
- Less pointer complexity

---

## 12. Frame System

```
FrameSystem
 ├── FrameContext
 ├── UploadContext
 ├── Sync primitives
 └── DeletionQueue
```

---

## 13. Full Hierarchy Summary

```
Renderer
 ├── SceneRendering
 ├── FramePipeline
 ├── RenderGraph
 ├── ResourceManager
 └── RHI Device
      └── Backend (Vulkan / D3D12 / Metal)
```

---

## 14. Key Takeaway

A modern renderer is structured as:

> Engine systems → Render graph → RHI → Backend

with most complexity handled through **composition, data flow, and resource systems**, not deep inheritance.

