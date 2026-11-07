# Subset bounds: each draw culled by its own subset's box

**Status. Landed.** Decisions locked 2026-10-22 with the user: per-subset bounds and the meshlet capture decoder; cascades draw per
subset (the user's choice over a second box per instance). Follows [plan_meshlet_culling_visibility.md](plan_meshlet_culling_visibility.md).

## Current state

A prim's mesh draws as one instance per subset (`placement.cpp`), but every one of them carries the whole mesh's world bounds, so instance
culling can't remove one subset and keep another. Sponza's `/root/_st_floor/exterior` shows it: its three subsets (14, 88 and 261 triangles)
share a 36 × 20 × 18 m box that holds the camera, so all three are always drawn, though the 14-triangle subset is one meshlet of 1.3 m
at a far corner. Cascades draw the prim's first instance (`primFirst`) with the whole mesh's meshlets, except for a prim with a cutout
subset (`primHasCutout`), which they draw per subset; that instance would need the whole mesh's box for cascades and its subset's for the
camera.

The `gpuscene.meshlets` capture shows a meshlet's `counts` and `cone` as the low 8 bits of each word (`GpuFieldType::Bits`).

## Scope

In:
- Subset bounds, computed when the view loads a mesh (as the mesh's bounds are now), the instance's world bounds from them.
- Cascades draw every subset instance on its own, as the camera does: `primFirst` and `primHasCutout` leave drawing and the instance record.
- The capture decoder: meshlet counts and cone decoded.

Out:
- Bounds in the mesh pack: they would need a format bump and a repack of every scene for numbers the view derives in one pass over the
  indices; the mesh's own bounds are already derived the same way.
- Apex-form cone test and orientation-grouped meshlets: not worth it for Sponza's exterior (it costs 7 meshlets the rasterizer discards).

## Decisions

### 1. Where subset bounds come from

**Locked: computed at load** (`subsetBounds` in `src/scene/mesh.h`, into `SubMesh::bounds`), beside the mesh bounds the view already
derives, which they replace (`meshBoundsOf` goes). Alternative: written by the packer, which needs a pack format and `usd` rule bump.

### 2. Cascades per subset

**Locked** (the user's choice). With one draw per region and meshlet work items, the whole-mesh draw saves no draws: the cascade draws the
same meshlets either way, in the same order (a prim's subsets are consecutive slots and consecutive meshlet ranges). What changes: cascades
test one box per subset instead of one per prim, and cascade instance counts are per subset, as the camera's are. The shadow maps stay the
same. Alternative: a second box per instance (`GpuInstance` 128 → 160 bytes), only for the camera's gain.

## Steps

1. `src/scene/mesh.h`: `SubMesh::bounds`, `subsetBounds(mesh, submesh)`; `meshBounds` removed. `viewscene.cpp` fills the bounds when it
   makes the `MeshDesc`; `meshBoundsOf` removed; `placement.cpp` takes each instance's world bounds from its subset.
2. `instancecull.comp`: every view draws the instance's subset (`perSubmesh` gone), cascade counts over every instance; `meshlet.glsl` the
   unused `drawsSubmesh` and flags gone. `RenderMeshInstance`, `GpuInstance` and `placement.cpp` drop `primFirst` and `primHasCutout`;
   `DrawLists::report`, the render debug snapshot's `primFirstInstances`, the GPU Scene window's flags and the Culling window's text follow.
3. `gpuschema.cpp`: meshlet `vertices`, `triangles` and `cone` (axis and cutoff) as fields of their own.
4. `src/renderer/README.md` (instance records, culling regions, counts).

## Visibility

- **Records**: `culling`'s cascade `drawn` and `culled` count subset instances; `render`'s `scene.primFirstInstances` goes.
- **Windows**: "Show culled" and the AABB overlay draw each subset's box; the GPU Scene window's instance bounds are the subset's; captures of
  `gpuscene.meshlets` show counts and cones.
- **Verbs, RPC, Engine Map**: none; nothing new to reach.

## Verification

- Sponza from `/root/PhysCamera001`: the exterior's three instances have three different boxes in `dump-gpuscene`'s `instances.json`; the
  camera culls more instances than before (161); triangles drawn no more than before.
- Shadow maps unchanged: screenshots before and after byte-identical at the same camera (the change moves only what culling removes whole).
- No validation messages on Sponza and three_cubes.

## Results

Sponza from `/root/PhysCamera001`, debug build, frame 150, against the build before this change:

- **The exterior's boxes**: the 14-triangle subset is now 0.3 × 1.6 × 2.1 m at the far corner, the roof subset a 1.7 m slab, the walls the
  building's box (`dump-gpuscene`). The walls subset still holds the camera, so it is still drawn: its meshlets are building-sized, and only
  the deferred apex-form cone test with orientation-grouped meshlets would cull them from inside.
- **Culling**: camera instances culled whole 161 → 192 (of 407); cascade 0 culls 180 subset instances (35 prims before), cascade 1 15
  (0 before). Meshlets the task shaders test: camera 23,743 → 22,861, cascade 0 29,481 → 27,438, cascade 1 41,012 → 40,750. Triangles drawn
  barely move (camera 948,949 → 948,113): meshlet culling already removed most of what instance culling now removes.
- **Same image**: screenshots at `/root/PhysCamera001` and at a second camera byte-identical to before, shadows included.
- **Capture**: `gpuscene.meshlets` rows show `vertices`, `triangles` and `cone` (axis and cutoff, or `none`).
- **No validation messages** on Sponza and three_cubes; the trace's only warnings are the asset server's existing translucent-glass notes.

## Deferred / follow-ups

- Apex-form cone test with orientation-grouped meshlets. Trigger: low-poly shells costing visible time in the test world.
