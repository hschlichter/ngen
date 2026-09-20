# Position-only vertex stream for depth-only passes

**Status. Landed.**

## Current state

Every mesh is one interleaved `Vertex` buffer (position, normal, colour, texcoord, 44 bytes). The shadow pass and the depth prepass
declare a single position attribute but bind that buffer, so the vertex fetch pulls 44 bytes to use 12. After the Sponza work
(`docs/plan_geometry_pass_cost.md`) the shadow pass is the largest GPU cost at 5.8 ms on 3.75 M triangles and is vertex-bound; the
prepass, when on, has the same shape. Listed as follow-up item 3 in `notes.md` since the first Sponza profile.

## Scope

**In**

- `CachedMesh::positionBuffer`: a second `Vertex`-usage buffer of `std::array<float, 3>` per mesh, built at upload from the same
  vertices, deleted with the mesh.
- Shadow pass and depth prepass pipelines take stride 12, attribute offset 0, and bind the position buffer. Geometry pass unchanged.
- `RenderDebugMesh::vertexBytes` includes the position stream, so the Scene tab and dump report what the GPU holds.

**Out**

- De-interleaving the geometry pass stream (position + attributes) or 16-bit attributes. Trigger: the geometry pass becomes vertex
  bound after the fragment work is gone. Today it is 5.7 ms with most of it shading.
- Index buffer compaction to 16 bits where meshes allow. Same trigger.

## Decisions

- **Duplicate positions rather than restructure `Vertex`.** 12 extra bytes per vertex of GPU memory (about 27 % more vertex memory,
  44 MB on Sponza) against two pipelines and one upload loop changed. Splitting `Vertex` into two streams for every pass would touch
  every vertex layout and the RHI single-binding `vertexStride`, which the plan for the geometry stream can do when it is needed.

## Verification

- Sponza courtyard headless: `ShadowPass` GPU ms below 5.8; screenshot byte-identical to the pre-change frame (`final_sponza.png`).
  Verified: ShadowPass 5.8 to 2.3 ms, frame 13.2 to 9.9 ms. The screenshot differed in 3888 pixels, traced to the shared
  fullscreen sampler losing anisotropy in `docs/plan_mip_debug.md` (the material sampler was split off); with anisotropy restored on
  it the frame is byte-identical, so the stream change itself is exact. The fullscreen sampler stays without anisotropy, which is
  the right state for 1:1 reads.
- `prepass on`: `DepthPrepass` GPU ms below the earlier 3.3 (culling on). Verified: 1.45 ms; prepass plus geometry 5.7 ms against
  6.0 ms without, so the prepass is now break-even here. Default stays off.
- three_cubes clean under validation; example sweep unchanged (no RHI change). Verified.

## Deferred / follow-ups

- Geometry pass stream split and 16-bit indices, triggers above.
