# ngen

## File naming
- No snake_case in filenames. Use lowercase concatenated names (e.g. `sceneloader.cpp`, `devicevulkan.h`).
- Platform-specific files put the platform as the last part of the name (e.g. `devicevulkan`, `swapchainvulkan`).

## Folder structure
- All source code lives under `src/`.
- `src/rhi/` — backend-agnostic RHI interfaces.
- `src/rhi/vulkan/` — Vulkan backend implementation. Additional backends go in sibling folders (e.g. `src/rhi/d3d12/`).
- `src/renderer/` — renderer front-end (render graph, resource management).
- `src/scene/` — scene loading, ECS, materials.
- Cross-cutting files (`main.cpp`, `types.h`, `camera.*`) live directly in `src/`.
