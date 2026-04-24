# Plan: Dear ImGui Integration

## Goal

Add Dear ImGui to the engine as the UI library for engine tooling (scene inspector, property editors, debug overlays, etc.).

## Approach: Platform-agnostic UI layer with backend-specific implementations

Dear ImGui ships with battle-tested backend implementations (`imgui_impl_vulkan`, `imgui_impl_sdl3`, etc.). We use these directly rather than reimplementing
ImGui rendering through our RHI abstraction — this avoids unnecessary work and keeps us on the well-maintained upstream path.

The UI layer follows the same pattern as the rest of the engine: a platform-agnostic interface in `src/ui/` with backend-specific implementations in
`src/ui/vulkan/`. All Vulkan-specific ImGui calls live in the Vulkan implementation, never in the shared UI code.

---

## Steps

### 1. Add Dear ImGui as a dependency

- Clone or add as git submodule under `external/imgui/`.
- Only need core files + backends:
  - `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, `imgui_demo.cpp`
  - `backends/imgui_impl_vulkan.cpp`
  - `backends/imgui_impl_sdl3.cpp`
- Update `Makefile`: add imgui source files to the build, add `external/imgui` and `external/imgui/backends` to include paths.

### 2. Create the UI interface and Vulkan implementation

The UI layer is split into a platform-agnostic interface and a backend-specific implementation, mirroring how the RHI is structured.

**`src/ui/ui.h`** — Abstract interface. No Vulkan or backend headers.

```
class UI {
    virtual void init(SDL_Window*, RhiDevice*) = 0;
    virtual bool processEvent(SDL_Event*) = 0;  // true if UI captures input
    virtual void beginFrame() = 0;
    virtual void render(RhiCommandBuffer*) = 0;
    virtual void shutdown() = 0;
};
```

**`src/rhi/vulkan/uivulkan.h`** / **`src/rhi/vulkan/uivulkan.cpp`** — Vulkan implementation. All `imgui_impl_vulkan` and `imgui_impl_sdl3` calls live here,
alongside the rest of the Vulkan backend code.

```
UIVulkan::init(SDL_Window*, RhiDevice*)
  - Cast RhiDevice* to RhiDeviceVulkan* to access Vulkan handles
  - ImGui::CreateContext()
  - ImGui_ImplSDL3_InitForVulkan(window)
  - ImGui_ImplVulkan_Init() with VkInstance, VkDevice, VkQueue, etc.
  - Upload font atlas texture (one-shot command buffer)

UIVulkan::processEvent(SDL_Event*)
  - ImGui_ImplSDL3_ProcessEvent(event)
  - Returns ImGui::GetIO().WantCaptureMouse || WantCaptureKeyboard

UIVulkan::beginFrame()
  - ImGui_ImplVulkan_NewFrame()
  - ImGui_ImplSDL3_NewFrame()
  - ImGui::NewFrame()

UIVulkan::render(RhiCommandBuffer*)
  - ImGui::Render()
  - Extract VkCommandBuffer from RhiCommandBufferVulkan
  - ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vkCmdBuf)

UIVulkan::shutdown()
  - ImGui_ImplVulkan_Shutdown()
  - ImGui_ImplSDL3_Shutdown()
  - ImGui::DestroyContext()
```

Callers (main.cpp, renderer) only see `UI*` — they never touch Vulkan-specific ImGui code. A future D3D12 backend would add `src/rhi/d3d12/uid3d12.cpp`
implementing the same interface.

### 3. ImGui as a frame graph pass

The renderer now uses a frame graph (`FrameGraph` in `src/renderer/`) with topological sorting, automatic barrier insertion, and transient resource management.
ImGui rendering becomes a dedicated pass in the graph.

In `Renderer::render()`, after the existing `ForwardPass`, add a new `DebugUIPass`:

```
struct DebugDebugUIPassData {
    FgTextureHandle color;
};

frameGraph.addPass<DebugDebugUIPassData>(
    "DebugUIPass",
    [&](FrameGraphBuilder& builder, DebugDebugUIPassData& data) {
        data.color = builder.write(colorHandle, FgAccessFlags::ColorAttachment);
        builder.setSideEffects(true);
    },
    [this](FrameGraphContext& ctx, const DebugDebugUIPassData& data) {
        ui->render(ctx.cmd());
    });
```

- The DebugUIPass writes to the same color attachment the ForwardPass writes to. The frame graph handles the layout transition automatically.
- No depth attachment needed — ImGui draws 2D overlays.
- `setSideEffects(true)` ensures the pass is never culled.
- The frame graph's topological sort guarantees DebugUIPass executes after ForwardPass (since both write the same color resource, and DebugUIPass reads the
  result of ForwardPass's write).

### 4. Expose Vulkan internals for ImGui init

`ImGui_ImplVulkan_Init` needs raw Vulkan handles: `VkInstance`, `VkPhysicalDevice`, `VkDevice`, `VkQueue`, `VkDescriptorPool`, `VkRenderPass`, etc.

Options:
- **a)** Add accessor methods to `RhiDeviceVulkan` that return the raw handles. These are Vulkan-specific so they live on the concrete class, not the abstract
  interface.
- **b)** Create a dedicated descriptor pool for ImGui (recommended — keeps it isolated from scene descriptor allocation).

### 5. Wire into main loop

Update `main.cpp` event loop and frame logic:

```
// Event loop
while (SDL_PollEvent(&ev)) {
    ui.processEvent(&ev);
    if (!ImGui::GetIO().WantCaptureMouse) { /* camera mouse handling */ }
    if (!ImGui::GetIO().WantCaptureKeyboard) { /* camera key handling */ }
}

// Frame
ui.beginFrame();
ImGui::ShowDemoWindow();  // replace with actual tool UI later
renderer.render(cam, window);  // frame graph includes DebugUIPass which calls ui->render()
```

The renderer owns the `UI*` pointer and calls `ui->render()` from within the DebugUIPass execute lambda. No need to split `Renderer::draw()` into
`beginFrame()`/`endFrame()` — the frame graph handles pass ordering and command buffer recording internally.

### 6. Build and verify

- Build with ImGui demo window enabled.
- Verify: demo window renders on top of the 3D scene, mouse/keyboard input routes correctly (ImGui captures when hovering UI, camera gets input otherwise).

---

## File summary

| File | Action |
|------|--------|
| `external/imgui/` | New — git submodule |
| `Makefile` | Edit — add imgui sources and include paths |
| `src/ui/ui.h` | New — platform-agnostic UI interface |
| `src/rhi/vulkan/uivulkan.h` | New — Vulkan ImGui implementation header |
| `src/rhi/vulkan/uivulkan.cpp` | New — Vulkan ImGui implementation (all Vulkan-specific ImGui calls) |
| `src/rhi/vulkan/rhidevicevulkan.h` | Edit — add accessors for raw Vulkan handles |
| `src/main.cpp` | Edit — wire up UI init, events, beginFrame call |
| `src/renderer/renderer.cpp` | Edit — add DebugUIPass to the frame graph after ForwardPass |

## Decisions made

- **ImGui as a frame graph pass** — the DebugUIPass is a dedicated node in the frame graph that writes the swapchain color attachment after the ForwardPass. The
  frame graph handles ordering, barriers, and command buffer recording. No need to split `Renderer::render()`.
- **Renderer owns the UI pointer** — `main.cpp` calls `ui.beginFrame()` and issues ImGui draw calls, but the actual `ui->render()` happens inside the
  DebugUIPass execute lambda within `Renderer::render()`.
- **No docking branch** — use ImGui `master` for now, keep it simple.
