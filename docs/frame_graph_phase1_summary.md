# Frame Graph Phase 1 — Implementation Summary

## What Changed

Phase 1 delivered dynamic rendering (RHI prerequisite), frame graph core types, compilation, and renderer integration. The existing forward pass now runs through the frame graph.

---

## Dynamic Rendering RHI Extension

Replaced the legacy VkRenderPass/VkFramebuffer path with Vulkan 1.3 dynamic rendering.

### New RHI types (rhitypes.h)

- `RhiImageLayout` — Undefined, ColorAttachment, DepthStencilAttachment, ShaderReadOnly, TransferSrc, TransferDst, PresentSrc
- `RhiRenderingAttachmentInfo` — texture, layout, clear flag, clear values
- `RhiRenderingInfo` — extent, color attachments, optional depth attachment
- `RhiBarrierDesc` — texture, old layout, new layout
- New `RhiFormat` entries: Undefined, R8G8B8A8_UNORM, B8G8R8A8_SRGB, B8G8R8A8_UNORM

### Removed RHI types

- `RhiRenderPass`, `RhiFramebuffer`, `RhiRenderPassBeginDesc`
- `RhiRenderPassVulkan`, `RhiFramebufferVulkan`

### Command buffer (rhicommandbuffer.h)

- Added: `beginRendering()`, `endRendering()`, `pipelineBarrier()`
- Removed: `beginRenderPass()`, `endRenderPass()`

### Pipeline creation

`RhiGraphicsPipelineDesc` now takes `colorFormats` + `depthFormat` instead of `RhiRenderPass*`. The Vulkan backend chains `VkPipelineRenderingCreateInfo` into the pipeline create info.

### Swapchain

- Removed render pass and framebuffer ownership
- Exposes swapchain images as `RhiTexture*` via `image(index)` and `depthImage()`
- Reports format via `colorFormat()` and `depthFormat()`

### Vulkan device

- Enables `VkPhysicalDeviceDynamicRenderingFeatures`
- Enables `VkPhysicalDeviceSynchronization2Features`
- Barriers use `vkCmdPipelineBarrier2` with `VkImageMemoryBarrier2`

---

## Frame Graph

### New files (src/renderer/)

| File | Purpose |
|------|---------|
| `framegraphresource.h` | Access flags, typed handles, resource descriptors |
| `passnode.h` | PassNode struct (reads, writes, side effects, execute fn) |
| `framegraphbuilder.h` | Setup-phase API (read, write, setSideEffects) |
| `framegraphcontext.h` | Execute-phase API (texture resolution, command buffer access) |
| `framegraph.h` | FrameGraph class with addPass template |
| `framegraph.cpp` | Build/compile/execute implementation |

### API

```cpp
// Import external resources
auto color = frameGraph.importTexture(texture, desc);

// Add passes
frameGraph.addPass<MyData>("PassName",
    [&](FrameGraphBuilder& builder, MyData& data) {
        data.color = builder.write(color, FgAccessFlags::ColorAttachment);
        builder.setSideEffects(true);
    },
    [](FrameGraphContext& ctx, const MyData& data) {
        auto* cmd = ctx.cmd();
        auto* tex = ctx.texture(data.color);
        // record GPU commands
    });

// Compile and execute
frameGraph.compile();
frameGraph.execute(cmd);
```

### Compilation

1. **Adjacency** — resource writes create edges to reader passes
2. **Topological sort** — Kahn's algorithm
3. **Pass culling** — backward walk from side-effect passes; unreachable passes are removed
4. **Barrier insertion** — automatic layout transitions between passes based on declared access flags

### Renderer integration

`Renderer::render()` now:
1. Resets the frame graph
2. Imports swapchain color + depth images
3. Adds a ForwardPass (the existing draw loop)
4. Compiles and executes the graph
5. Manually inserts pre/post barriers for swapchain layout transitions

---

## Files Modified

| File | Change |
|------|--------|
| `src/rhi/rhitypes.h` | New types, updated pipeline desc, removed render pass types |
| `src/rhi/rhicommandbuffer.h` | Dynamic rendering + barrier methods |
| `src/rhi/rhiswapchain.h` | Simplified interface |
| `src/rhi/vulkan/rhiresourcesvulkan.h` | Removed render pass/framebuffer structs |
| `src/rhi/vulkan/rhicommandbuffervulkan.h/.cpp` | Dynamic rendering + barrier implementation |
| `src/rhi/vulkan/rhiswapchainvulkan.h/.cpp` | Removed render pass/framebuffer, expose images |
| `src/rhi/vulkan/rhidevicevulkan.cpp` | Feature enablement, pipeline creation update, new formats |
| `src/renderer/renderer.h/.cpp` | Frame graph member, refactored render() |
