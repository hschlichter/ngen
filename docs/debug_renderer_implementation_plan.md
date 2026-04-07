# Debug Renderer Implementation Plan

Based on [debug_renderer_architecture_modern_c_renderer.md](debug_renderer_architecture_modern_c_renderer.md), adapted to the current ngen engine state.

---

## Current Engine State

**What exists:**
- RHI abstraction with Vulkan backend (`RhiDevice`, `RhiCommandBuffer`, `RhiBuffer`, `RhiPipeline`)
- Frame graph (`FrameGraph`, `FrameGraphBuilder`, `FrameGraphContext`) with pass setup/execute, resource lifetime tracking, automatic barrier insertion
- `ResourcePool` for transient texture allocation
- `Renderer` with ForwardPass and DebugUIPass (ImGui overlay)
- Scene types: `AABB`, `Transform`, `Frustum` in `src/scene/scenetypes.h`
- Per-frame uniform buffers with view/proj matrices (CpuToGpu, persistent map)
- Triple-buffered frame-in-flight sync

**What's missing (needed for debug renderer):**
- `RhiPrimitiveTopology` — pipeline topology is hardcoded to `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`
- `draw()` (non-indexed) command on `RhiCommandBuffer` — only `drawIndexed()` exists
- Line-list pipeline support
- Debug shaders (position + color only, no textures)
- The debug renderer itself

---

## Phase 1 — RHI Extensions

Add the minimal RHI surface needed for debug line rendering.

### 1a. Add primitive topology to RHI

**`src/rhi/rhitypes.h`** — add enum:
```cpp
enum class RhiPrimitiveTopology {
    TriangleList,
    LineList,
};
```

**`src/rhi/rhitypes.h`** — add field to `RhiGraphicsPipelineDesc`:
```cpp
RhiPrimitiveTopology topology = RhiPrimitiveTopology::TriangleList;
```

**`src/rhi/vulkan/rhidevicevulkan.cpp`** — use `desc.topology` instead of hardcoded `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`.

### 1b. Add non-indexed draw

**`src/rhi/rhicommandbuffer.h`** — add:
```cpp
virtual auto draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) -> void = 0;
```

**`src/rhi/vulkan/rhicommandbuffervulkan.cpp`** — implement with `vkCmdDraw`.

---

## Phase 2 — Debug Shaders

### 2a. `shaders/debug.vert`

Minimal vertex shader: position (vec3) + color (vec4), UBO with view/proj. No model matrix push constant (vertices are pre-transformed to world space by the CPU batcher).

```glsl
#version 450
layout(set = 0, binding = 0) uniform UBO { mat4 view; mat4 proj; };
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 fragColor;
void main() {
    gl_Position = proj * view * vec4(inPos, 1.0);
    fragColor = inColor;
}
```

### 2b. `shaders/debug.frag`

```glsl
#version 450
layout(location = 0) in vec4 fragColor;
layout(location = 0) out vec4 outColor;
void main() { outColor = fragColor; }
```

Compile to `debug.vert.spv`, `debug.frag.spv`.

---

## Phase 3 — DebugDraw API (Engine-Facing)

**File:** `src/debugdraw.h`

A simple, header-only collection point that any engine system can call. No rendering logic.

```cpp
struct DebugVertex {
    glm::vec3 position;
    glm::vec4 color;
};

struct DebugDrawData {
    std::vector<DebugVertex> lines;          // pairs of vertices
    std::vector<DebugVertex> linesNoDepth;   // overlay lines (no depth test)
};

class DebugDraw {
public:
    void line(glm::vec3 a, glm::vec3 b, glm::vec4 color);
    void box(const AABB& box, glm::vec4 color);

    void newFrame();                        // clears transient data
    const DebugDrawData& data() const;

private:
    DebugDrawData frameData;
};
```

- `line()` pushes two `DebugVertex` entries into `frameData.lines`.
- `box()` generates 12 line calls from the AABB corners.
- `newFrame()` clears `frameData.lines` (called at frame start, before any system submits).
- No timed primitives in Phase 3; add in a later phase if needed.

---

## Phase 4 — DebugRenderer (GPU Side)

**File:** `src/renderer/debugrenderer.h`, `src/renderer/debugrenderer.cpp`

Lives in the renderer module. Consumes `DebugDrawData`, uploads to GPU, adds frame graph passes.

### 4a. Init

Called from `Renderer::init()`:
- Load `debug.vert.spv`, `debug.frag.spv`
- Create descriptor set layout (single UBO binding, same as existing view/proj UBO)
- Create two pipelines:
  - **debugLinePipeline** — `LineList` topology, depth test enabled, depth write disabled
  - **debugLineNoDepthPipeline** — `LineList` topology, depth test disabled
- Create descriptor pool + sets (one per swapchain image, reusing existing UBOs)
- Allocate per-frame CpuToGpu vertex buffers (one per swapchain image, sized to a reasonable max like 64K vertices, grown if needed)

### 4b. Prepare (per frame)

```cpp
void DebugRenderer::prepare(const DebugDrawData& data, uint32_t frameIndex);
```

- memcpy `data.lines` into the mapped vertex buffer for `frameIndex`
- Store vertex counts for the draw calls

### 4c. Frame Graph Passes

```cpp
void DebugRenderer::addPasses(FrameGraph& fg, FgTextureHandle color, FgTextureHandle depth, uint32_t frameIndex);
```

Adds a **DebugWorldPass** that:
- Writes to color (no clear), reads depth
- `beginRendering` with color + depth attachments (clear = false)
- Binds `debugLinePipeline`, binds vertex buffer, binds descriptor set, calls `draw(lineVertexCount, 1, 0, 0)`

This pass slots between ForwardPass and DebugUIPass in the frame graph. Since the frame graph handles ordering via resource dependencies, declaring `write(color)` and `read(depth)` after ForwardPass writes both ensures correct ordering.

**Note:** A separate DebugOverlayPass (no depth) can be added later. For Phase 4, depth-tested world lines are the priority.

### 4d. Depth test configuration

The RHI pipeline desc currently lacks depth test/write control. Two options:
1. Add `bool depthTestEnable`, `bool depthWriteEnable` to `RhiGraphicsPipelineDesc` (cleanest)
2. Use a separate pipeline with depth test disabled hardcoded in the Vulkan backend

Option 1 is preferred — add these two booleans with defaults (`true`/`true`) so existing code is unaffected.

---

## Phase 5 — Integration

### 5a. Renderer changes

**`src/renderer/renderer.h`** — add member:
```cpp
DebugRenderer debugRenderer;
```

**`src/renderer/renderer.cpp`**:
- `init()` — call `debugRenderer.init(device, swapchain, uniformBuffers)`
- `render()` — call `debugRenderer.prepare(debugDrawData, frameIndex)` before frame graph build, then `debugRenderer.addPasses(fg, colorHandle, depthHandle, frameIndex)` between ForwardPass and DebugUIPass
- `destroy()` — call `debugRenderer.destroy()`

### 5b. Exposing DebugDraw

The `DebugDraw` instance lives in `main.cpp` (or wherever the application loop is). Systems call into it during update. The renderer receives `const DebugDrawData&` — it never owns or manages the debug draw state.

### 5c. Frame order

```
ForwardPass         — scene geometry
DebugWorldPass      — depth-tested debug lines
DebugUIPass         — ImGui overlay
```

---

## Phase 6 — Future Extensions (Not in Initial Implementation)

- **Timed primitives** — `DebugDraw` tracks `remainingTime`, `newFrame()` decrements and keeps alive
- **Categories/filtering** — `DebugCategory` enum, per-line category, bitmask filter
- **Instanced shapes** — sphere, cone meshes drawn with instancing for large counts
- **DebugOverlayPass** — screen-space lines without depth test
- **Text rendering** — requires font atlas texture, separate pipeline
- **Gizmo renderer** — interactive editor gizmos with picking support

---

## Summary of Files to Create/Modify

| Action | File | What |
|--------|------|------|
| Modify | `src/rhi/rhitypes.h` | Add `RhiPrimitiveTopology`, depth test booleans to pipeline desc |
| Modify | `src/rhi/rhicommandbuffer.h` | Add `draw()` |
| Modify | `src/rhi/vulkan/rhidevicevulkan.cpp` | Use topology from desc, honor depth test flags |
| Modify | `src/rhi/vulkan/rhicommandbuffervulkan.h/.cpp` | Implement `draw()` |
| Create | `shaders/debug.vert` | Position + color vertex shader |
| Create | `shaders/debug.frag` | Pass-through fragment shader |
| Create | `src/debugdraw.h` | Engine-facing debug draw API |
| Create | `src/debugdraw.cpp` | `line()`, `box()`, `newFrame()` |
| Create | `src/renderer/debugrenderer.h` | GPU-side debug renderer |
| Create | `src/renderer/debugrenderer.cpp` | Init, prepare, addPasses, destroy |
| Modify | `src/renderer/renderer.h` | Add `DebugRenderer` member |
| Modify | `src/renderer/renderer.cpp` | Wire up init/prepare/addPasses/destroy |
| Modify | `src/main.cpp` | Create `DebugDraw`, call `newFrame()`, pass data to renderer |
| Modify | `Makefile` | Compile new .cpp files, shader compilation rules |
