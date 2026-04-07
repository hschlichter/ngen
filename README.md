# ngen

A modern 3D graphics engine written in C++23 with a Vulkan rendering backend. Loads and renders glTF/GLB models with textures, lighting, and an interactive camera.

## Features

- **Frame Graph** — Declarative render pass system with automatic dependency resolution, topological sorting, pass culling, and barrier insertion
- **Transient Resource Management** — Pool-based GPU resource allocation with lifetime tracking and intra-frame aliasing for non-overlapping resources
- **Dynamic Rendering** — Vulkan 1.3 dynamic rendering (no VkRenderPass/VkFramebuffer), with synchronization2 barriers
- **Rendering Hardware Interface (RHI)** — Backend-agnostic GPU abstraction layer, currently implemented for Vulkan
- **glTF/GLB model loading** — Meshes with positions, normals, UVs, vertex colors, and embedded textures
- **Texture sampling** — Base color textures with linear filtering
- **Basic lighting** — Directional diffuse + ambient lighting in the fragment shader
- **FPS camera** — WASD movement, right-click mouse look
- **Per-mesh transforms** — Model matrices from glTF node hierarchy via push constants
- **Modern C++23** — Trailing return types, ranges, `std::expected` error handling

## Architecture

```
App (main.cpp)
 └─ Scene (scene/)        — glTF loading, mesh/texture extraction
 └─ Renderer (renderer/)  — Frame graph, resource pool, GPU mesh management
     ├─ FrameGraph        — Pass declaration, compilation, execution
     ├─ ResourcePool      — Transient texture pooling with descriptor-keyed reuse
     └─ RHI (rhi/)        — Abstract device, swapchain, command buffer interfaces
         └─ Vulkan (rhi/vulkan/)  — Vulkan 1.3 backend (dynamic rendering, sync2)
```

Additional backends (D3D12, Metal) can be added as sibling folders under `src/rhi/`.

## Dependencies

| Library | Purpose |
|---------|---------|
| [SDL3](https://github.com/libsdl-org/SDL) | Window creation, input, Vulkan surface |
| [Vulkan SDK](https://vulkan.lunarg.com/) | GPU API + glslc shader compiler |
| [GLM](https://github.com/g-truc/glm) | Math (vectors, matrices, quaternions) |
| [cgltf](https://github.com/jsmber/cgltf) | glTF file parsing |
| [stb](https://github.com/nothings/stb) | Image loading (stb_image) |
| [OpenUSD](https://github.com/PixarAnimationStudios/OpenUSD) | USD scene format (stage, layers, composition) |

GLM, cgltf, stb, and OpenUSD are included as git submodules in `external/`. SDL3 and the Vulkan SDK must be installed on the system.

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

This builds the core USD libraries (usd, sdf, tf, usdGeom, usdShade, usdLux) with oneTBB into `external/openusd_build/`. The engine links against these at build time and loads them at runtime via rpath.

## Building

Requires clang++ with C++23 support and the Vulkan SDK. OpenUSD must be built first (see above).

```bash
make
```

Shaders are compiled from GLSL to SPIR-V automatically via `glslc`.

## Usage

```bash
./_out/ngen models/DamagedHelmet.glb
```

### Controls

| Input | Action |
|-------|--------|
| WASD | Move camera |
| Q / E | Move down / up |
| Shift | Sprint (3x speed) |
| Right mouse button | Hold to look around |
| ESC | Quit |
