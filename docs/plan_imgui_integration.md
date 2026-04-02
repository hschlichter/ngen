# Plan: Dear ImGui Integration

## Goal

Add Dear ImGui to the engine as the UI library for engine tooling (scene inspector, property editors, debug overlays, etc.).

## Approach: Platform-agnostic UI layer with backend-specific implementations

Dear ImGui ships with battle-tested backend implementations (`imgui_impl_vulkan`, `imgui_impl_sdl3`, etc.). We use these directly rather than reimplementing ImGui rendering through our RHI abstraction — this avoids unnecessary work and keeps us on the well-maintained upstream path.

The UI layer follows the same pattern as the rest of the engine: a platform-agnostic interface in `src/ui/` with backend-specific implementations in `src/ui/vulkan/`. All Vulkan-specific ImGui calls live in the Vulkan implementation, never in the shared UI code.

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

**`src/rhi/vulkan/uivulkan.h`** / **`src/rhi/vulkan/uivulkan.cpp`** — Vulkan implementation. All `imgui_impl_vulkan` and `imgui_impl_sdl3` calls live here, alongside the rest of the Vulkan backend code.

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

Callers (main.cpp, renderer) only see `UI*` — they never touch Vulkan-specific ImGui code. A future D3D12 backend would add `src/rhi/d3d12/uid3d12.cpp` implementing the same interface.

### 3. ImGui needs its own render pass or subpass

Two options:

**Option A — Separate render pass (simpler, recommended to start)**
- Create a second render pass that loads (not clears) the color attachment and has no depth attachment.
- After the scene render pass ends, begin the ImGui render pass on the same command buffer, then end it.
- Straightforward, easy to reason about.

**Option B — Render in the existing render pass**
- Draw ImGui after all scene geometry, within the same render pass.
- Slightly more efficient (no extra render pass transition), but couples ImGui into the scene render pass setup.

Recommend starting with **Option A** for clean separation.

### 4. Expose Vulkan internals for ImGui init

`ImGui_ImplVulkan_Init` needs raw Vulkan handles: `VkInstance`, `VkPhysicalDevice`, `VkDevice`, `VkQueue`, `VkDescriptorPool`, `VkRenderPass`, etc.

Options:
- **a)** Add accessor methods to `RhiDeviceVulkan` that return the raw handles. These are Vulkan-specific so they live on the concrete class, not the abstract interface.
- **b)** Create a dedicated descriptor pool for ImGui (recommended — keeps it isolated from scene descriptor allocation).

### 5. Wire into main loop

Update `main.cpp` event loop and frame logic:

```
// Event loop
while (SDL_PollEvent(&event)) {
    ui.processEvent(&event);
    if (!ImGui::GetIO().WantCaptureMouse) { /* camera mouse handling */ }
    if (!ImGui::GetIO().WantCaptureKeyboard) { /* camera key handling */ }
}

// Frame
ui.beginFrame();
ImGui::ShowDemoWindow();  // replace with actual tool UI later
renderer.draw(camera, window);  // scene renders, then UI::render is called
```

main.cpp coordinates both render calls. The renderer exposes the active command buffer so that `ui.render()` can record into it after scene drawing is done but before submission.

```
// Frame
ui.beginFrame();
ImGui::ShowDemoWindow();  // replace with actual tool UI later
renderer.beginFrame(camera, window);  // acquire, begin command buffer, draw scene
ui.render(renderer.commandBuffer());  // record ImGui draw commands
renderer.endFrame();                  // end command buffer, submit, present
```

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
| `src/main.cpp` | Edit — wire up UI init, events, frame calls |
| `src/renderer/renderer.cpp` | Edit — add ImGui render pass (Option A) or call ImGui render at end of existing pass |

## Decisions made

- **main.cpp coordinates rendering** — `renderer.beginFrame()` draws the scene, then `ui.render()` records ImGui commands, then `renderer.endFrame()` submits and presents. This means splitting `Renderer::draw()` into `beginFrame()`/`endFrame()`.
- **Separate render pass for ImGui** (Option A) — its own render pass that loads (not clears) the color attachment with no depth. Clean separation and maps naturally to a future framegraph where the UI pass is an explicit node.
- **No docking branch** — use ImGui `master` for now, keep it simple.
