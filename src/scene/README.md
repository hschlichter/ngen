# Scene types

The plain types the renderer and ngen-view share for what is drawn. No USD, no runtime scene: the runtime scene is the World
(`src/world/README.md`), which ngen-view flattens into these (`src/view/README.md`). The USD scene that used to live here is in
`src/editor/` (`src/editor/README.md`).

| Files | What |
|---|---|
| `mesh.h` | `MeshDesc` (vertices, 32-bit indices, submeshes with their material, `doubleSided`) and `MeshLibrary`, keyed by `MeshHandle` |
| `material.h` | `MaterialDesc` (base colour factor, base colour texture pixels with their mip levels, the cutout opacity: threshold, constant, channel, raw flag, its own texture or the base colour's) and `MaterialLibrary`, keyed by `MaterialHandle` |
| `scenetypes.h` | `Transform` (position, rotation, scale, as the gizmos edit it), `AABB`, `Frustum`, `Ray`, `LightDesc` |
| `scenehandles.h` | the 1-based handles: `MeshHandle`, `MaterialHandle`, `PrimHandle` (a draw's prim index), `LayerHandle` |
