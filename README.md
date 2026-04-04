# ngen

A modern 3D graphics engine written in C++23 with a Vulkan rendering backend. Loads and renders glTF/GLB models with textures, lighting, and an interactive camera.

## Features

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
 └─ Renderer (renderer/)  — Frame orchestration, GPU mesh management
     └─ RHI (rhi/)        — Abstract device, swapchain, command buffer interfaces
         └─ Vulkan (rhi/vulkan/)  — Vulkan 1.2+ backend implementation
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

GLM, cgltf, and stb are included as git submodules in `external/`. SDL3 and the Vulkan SDK must be installed on the system.

After cloning, initialize the submodules:

```bash
git submodule update --init --recursive
```

## Building

Requires clang++ with C++23 support and the Vulkan SDK.

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
| Right mouse button | Hold to look around |
| ESC | Quit |
