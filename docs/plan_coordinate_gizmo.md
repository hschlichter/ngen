# Plan: Coordinate System Gizmo

## Context

The engine has no orientation reference in the viewport. A coordinate system gizmo (XYZ axes indicator) in the top-right corner shows the camera's orientation
at a glance, standard in 3D editors.

## Approach: Dedicated GizmoPass

A new `GizmoPass` renders three colored axis lines (X=red, Y=green, Z=blue) in a small viewport region in the top-right corner. It reuses the existing
`debug.vert/frag` shaders and `DebugVertex` format. No new shaders needed.

Key rendering differences from the debug renderer:
- **Rotation-only view matrix** (strip translation so position doesn't matter)
- **Orthographic projection** (axes stay constant size)
- **No depth test** (always visible, axes don't fight scene geometry)
- **Restricted viewport/scissor** (80x80px in top-right corner)

## Steps

### 1. Extend RHI viewport/scissor with offset

The current `setViewport(RhiExtent2D)` hardcodes origin to (0,0). The gizmo needs an offset.

- `src/rhi/rhicommandbuffer.h` — Add overloads: `setViewport(int32_t x, int32_t y, RhiExtent2D)`, `setScissor(int32_t x, int32_t y, RhiExtent2D)`. Make existing
  single-arg versions non-virtual wrappers that call these with (0,0).
- `src/rhi/vulkan/rhicommandbuffervulkan.h/.cpp` — Implement the new overrides.

### 2. Create GizmoPass

**`src/renderer/passes/gizmopass.h`** — Class with `init`, `destroy`, `addPass` following `DebugRenderer` pattern.

**`src/renderer/passes/gizmopass.cpp`**:

`init()`:
- Load `debug.vert.spv` / `debug.frag.spv`
- Pipeline: LineList, no depth test/write, no backface culling, no depth format
- Static vertex buffer (6 verts = 3 axes x 2 endpoints):
  - X: (0,0,0) to (1,0,0) red
  - Y: (0,0,0) to (0,1,0) green
  - Z: (0,0,0) to (0,0,1) blue
- Per-frame UBOs + descriptor sets (same layout as debug renderer: binding 0 = UBO)

`addPass(fg, colorHandle, fullExtent, viewMatrix, imageIndex)`:
- Compute rotation-only view: `glm::mat4(glm::mat3(viewMatrix))`
- Orthographic proj: `glm::ortho(-1.5, 1.5, -1.5, 1.5, -10, 10)` with Vulkan Y-flip
- Write UBO, register frame graph pass with offset viewport/scissor (80x80px, top-right with 10px margin)

### 3. Integrate into Renderer

- `src/renderer/renderer.h` — Add `#include "gizmopass.h"`, add `GizmoPass gizmoPass` member
- `src/renderer/renderer.cpp`:
  - `init()`: `gizmoPass.init(device, imgCount, ext, colorFmt)`
  - `render()`: `gizmoPass.addPass(...)` after debugRenderer, before editorUIPass
  - `destroy()`: `gizmoPass.destroy(device)`

## Files

| File | Action |
|------|--------|
| `src/rhi/rhicommandbuffer.h` | Add offset viewport/scissor overloads |
| `src/rhi/vulkan/rhicommandbuffervulkan.h` | Declare overrides |
| `src/rhi/vulkan/rhicommandbuffervulkan.cpp` | Implement overrides |
| `src/renderer/passes/gizmopass.h` | Create |
| `src/renderer/passes/gizmopass.cpp` | Create |
| `src/renderer/renderer.h` | Add GizmoPass member |
| `src/renderer/renderer.cpp` | Init, addPass, destroy calls |

## Verification

1. `make` — clean build
2. Run `_out/ngen models/city/city_5x5/city.usda`
3. Confirm XYZ gizmo visible in top-right corner
4. Rotate camera — gizmo axes should rotate correspondingly
5. Move camera — gizmo should stay fixed in corner, same size
6. Resize window — gizmo stays in top-right corner
