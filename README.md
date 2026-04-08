# ngen

A modern 3D engine written in C++23 with a Vulkan rendering backend and OpenUSD scene system.

![ngen](docs/engine_1.png)

## Features

- **Vulkan Renderer** — Dynamic rendering (VK_KHR_dynamic_rendering), synchronization2 barriers
- **Frame Graph** — Declarative render pass system with automatic dependency resolution, topological sorting, pass culling, and barrier insertion
- **Transient Resource Management** — Pool-based GPU resource allocation with lifetime tracking
- **RHI Abstraction** — Backend-agnostic GPU interface, currently implemented for Vulkan
- **OpenUSD Scene System** — Stage loading, layer stack, composition, sublayer management
- **Scene Graph UI** — Hierarchical tree view with selection, raycast picking, property editing
- **Debug Renderer** — Line-based debug drawing (AABBs, selection highlights)
- **Property Inspector** — Transform (local + world), visibility, bounds, material inspection
- **Layer Management** — Add/load sublayers, mute/unmute, save, edit target selection
- **Material Support** — UsdPreviewSurface textures, displayColor primvars, constant colors
- **FPS Camera** — WASD movement, right-click mouse look

## Architecture

```
App (main.cpp)
 └─ Scene (scene/)        — USD loading, mesh/texture/material extraction
 └─ Renderer (renderer/)  — Frame graph, resource pool, GPU mesh management
     ├─ FrameGraph        — Pass declaration, compilation, execution
     ├─ ResourcePool      — Transient texture pooling
     ├─ DebugRenderer     — Debug line pass (AABBs, highlights)
     └─ RHI (rhi/)        — Abstract device, swapchain, command buffer interfaces
         └─ Vulkan (rhi/vulkan/)  — Vulkan 1.3 backend
```

## Dependencies

| Library | Purpose |
|---------|---------|
| [SDL3](https://github.com/libsdl-org/SDL) | Window creation, input, Vulkan surface, file dialogs |
| [Vulkan SDK](https://vulkan.lunarg.com/) | GPU API + glslc shader compiler |
| [GLM](https://github.com/g-truc/glm) | Math (vectors, matrices, quaternions) |
| [stb](https://github.com/nothings/stb) | Image loading (stb_image) |
| [Dear ImGui](https://github.com/ocornut/imgui) | Editor UI |
| [OpenUSD](https://github.com/PixarAnimationStudios/OpenUSD) | USD scene format (stage, layers, composition) |

GLM, stb, Dear ImGui, and OpenUSD are included as git submodules in `external/`. SDL3 and the Vulkan SDK must be installed on the system.

After cloning, initialize the submodules:

```bash
git submodule update --init --recursive
```

## Building OpenUSD

OpenUSD must be built separately before building the engine. This only needs to be done once. Requires CMake and Python 3.

```bash
python3 external/openusd/build_scripts/build_usd.py \
  --no-python --no-imaging --no-tests --no-examples \
  --no-tutorials --no-tools --no-docs --no-materialx \
  --no-alembic --no-draco --no-openimageio --no-opencolorio \
  --no-openvdb --no-ptex --no-embree --no-prman \
  --onetbb --build-variant release \
  -j$(nproc) \
  external/openusd_build
```

## Building

Requires clang++ with C++23 support and the Vulkan SDK. OpenUSD must be built first (see above).

```bash
make
```

Shaders are compiled from GLSL to SPIR-V automatically via `glslc`.

## Usage

```bash
./_out/ngen [scene.usd]
```

Or launch without arguments and use File > Open.

### Controls

| Input | Action |
|-------|--------|
| WASD | Move camera |
| Q / E | Move down / up |
| Shift | Sprint (3x speed) |
| Right mouse button | Hold to look around |
| Left click | Pick object |
