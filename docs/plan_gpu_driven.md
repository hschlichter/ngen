# GPU-driven rendering (umbrella)

**Status. Draft.**

Umbrella plan: it orders the stages and holds the decisions that cut across them. Each stage gets its own `docs/plan_*.md` with its own steps and
verification. This doc links to them and records their status.

## Current state

Rendering is CPU-driven end to end:

- **Visibility.** `cullInstances` and `cullShadowCascades` (`src/renderer/culling.h`, `docs/plan_frustum_culling.md`, `docs/plan_shadow_cascades.md`)
  run on the main thread. Per-instance byte masks travel to the render thread in `RenderSnapshot::visible` and `shadowVisible`.
- **Draws.** The shadow, prepass and geometry passes loop over `GpuInstance`s on the CPU. They issue one `drawIndexed` per visible instance and
  submesh, with a push constant for the model matrix. Sponza courtyard: about 400 geometry draws, 460 shadow draws over three cascades.
- **Geometry.** Every mesh owns its own vertex, position and index buffers (`CachedMesh`), so every draw rebinds them.
- **Materials.** Descriptor sets are allocated per frame slot per instance (`Renderer::rebuildGeometryDescriptorSets`), each holding the UBO and the
  instance's one texture. Every draw binds one.
- **RHI.** Vulkan with `synchronization2` and `dynamicRendering` enabled. The RHI has no descriptor indexing, `multiDrawIndirect`,
  `drawIndirectCount` or indirect commands. Buffers are not tracked by the frame graph (`docs/plan_frame_graph_buffers.md`).

Measured on Sponza (Radeon 890M, 2560x1440): CPU main thread about 1.2 ms, render-thread record about 0.5 ms, GPU 9 to 13 ms. The GPU cost is
triangle throughput, and per-instance culling removes little because the meshes are the size of buildings (129 of 460 shadow draws culled).

## Principle

The GPU owns the scene. Instance, geometry and material data live in persistent GPU buffers that the CPU updates with deltas. Visibility and draw lists
are produced on the GPU. A pass issues a handful of indirect draws per pipeline state, not one draw per instance. The CPU describes what changed, not
what to draw.

## What this does and does not buy on Sponza

Stages 1 to 5 are the foundation, not the Sponza speed-up.

- CPU culling and recording already cost under 2 ms. Moving them to the GPU saves CPU time, not GPU time.
- Per-instance GPU culling removes the same instances CPU culling removes today.
- The measured bottleneck, too many triangles in building-sized meshes, is fixed by culling finer than a whole instance: meshlets or clusters. Those
  plug into stage 5's culling pass.
- Meshlets depend on asset packing (`notes.md`), which is why they are Out here.

The payoff of stages 1 to 5 is scale. Draw and CPU cost stop growing with instance count (Jungle Ruins, `notes.md`), and meshlets arrive later as an
extension of the culling pass rather than a rewrite.

## Scope

**In**

1. Frame-graph buffers and a persistent GPU instance buffer.
2. Global geometry pool.
3. Bindless materials.
4. Indirect draws, still fed by CPU culling.
5. GPU culling, per instance, against the camera and each shadow cascade.

**Out**

- Meshlets, clusters, mesh shaders. They depend on the asset-packing and cook discussion in `notes.md`.
- Occlusion culling (two-phase HZB). Trigger in Deferred.
- LOD, streaming, visibility buffer, ray tracing.
- Async compute. There is still nothing to overlap (`notes.md` measurements).
- A second backend. Stages 3 and 4 widen the RHI in a Vulkan-shaped way; see the decisions.

## Stages

Each stage ships on its own: rendered output byte-identical to the stage before, fewer or cheaper commands, and observations that show it.

| # | Plan | Delivers | RHI change | Depends on |
|---|------|----------|------------|------------|
| 1 | [plan_frame_graph_buffers.md](plan_frame_graph_buffers.md) | Buffers in the graph; imported buffers keep state across frames; one persistent instance buffer with delta upload; shaders read `instances[gl_InstanceIndex]` | none | none |
| 2 | `plan_geometry_pool.md` (to write) | One vertex, one position and one index buffer for all meshes; mesh table with `firstIndex`, `vertexOffset`, `indexCount`, bounds | none | none |
| 3 | `plan_bindless_materials.md` (to write) | Material buffer (factors, texture indices), one global texture array; per-instance descriptor sets gone | descriptor indexing | 1 |
| 4 | `plan_indirect_draws.md` (to write) | CPU-culled draw lists written as indirect commands; one `drawIndexedIndirectCount` per pipeline bucket per view | indirect draw, `IndirectRead` state, features | 1, 2, 3 |
| 5 | `plan_gpu_culling.md` (to write) | Compute pass per view writes compacted indirect commands and counts; CPU culling deleted; transient graph buffers | none beyond 4 | 4 |

- **Stage 1.** Resource model, cross-frame state, and the instance buffer.
- **Stage 2.** A suballocator over three large buffers. Meshes are added and removed at runtime, so it needs a free list, or compaction on
  removal. Draws bind the pool once per pass. The per-draw `bindVertexBuffer`/`bindIndexBuffer` calls go away, and `drawIndexed` uses the mesh's
  `firstIndex` and `vertexOffset`.
- **Stage 3.** The biggest RHI step.
  - Enable Vulkan 1.2 descriptor indexing: `runtimeDescriptorArray`, `descriptorBindingPartiallyBound`, `descriptorBindingSampledImageUpdateAfterBind`,
    `shaderSampledImageArrayNonUniformIndexing`.
  - Add a variable-count binding to `RhiDescriptorBinding`.
  - Add a `GpuMaterial` buffer indexed from the instance.
  - `gbuffer.frag` samples `textures[nonuniformEXT(material.baseColorTexture)]`.
  - Verified through a new RHI example (`ngen-example-bindless`) before the renderer switches.
- **Stage 4.**
  - RHI: `drawIndexedIndirect`, `drawIndexedIndirectCount`, `RhiBufferState::IndirectRead`, `RhiBufferUsage::Indirect`, and the `multiDrawIndirect`
    and `drawIndirectCount` features.
  - The render thread turns the CPU masks into `VkDrawIndexedIndirectCommand`s per bucket and uploads them. The pass issues one indirect call per bucket.
  - The RHI example `ngen-example-indirect` comes first.
- **Stage 5.**
  - `InstanceCull` compute pass per view: the camera, plus each cascade.
  - Inputs: instance bounds and the mesh table. Outputs: transient command and count buffers per bucket.
  - The frozen-frustum and AABB overlays, and the `culled`/`shadow_culled` stats, read the counts back one frame late.
  - The CPU culling code and the snapshot's visibility masks are deleted.

## Decisions

- **Order 1, 2, 3, 4, 5.** Stages 2 and 3 are independent of each other; both must land before 4.
  - I lean geometry pool before bindless. It is smaller, needs no RHI change, and removes half of the per-draw binds on its own, which makes stage 3's
    remaining descriptor bind easy to see. Pushback welcome.
  - Stage 1 comes first because 3, 4 and 5 all read the instance buffer through the graph.
- **Instance index through `firstInstance`, not `gl_DrawID`.** An indirect command carries `firstInstance`. Setting it to the instance index makes
  `gl_InstanceIndex` the lookup key in every stage, and that needs no `shaderDrawParameters`. Stage 1 introduces this, so the shaders don't change
  again in stage 4.
- **Draw record = instance × submesh, as today.** `GpuInstance` is already expanded per material submesh, and the shadow pass draws whole meshes on
  `primFirst`. Stage 5's culling emits commands from the same records, so the shadow-pass rule becomes a per-view filter in the compute shader.
- **Pipeline buckets stay.** The buckets are `doubleSided` × (prepass or not) in the geometry pass and `doubleSided` in the shadow pass. Each bucket
  gets its own region of the command buffer and its own count. More material models later add buckets, not per-draw state.
- **Require the features; no CPU fallback path.** Descriptor indexing, `multiDrawIndirect` and `drawIndirectCount` are core in Vulkan 1.2 and present
  on RADV and on every desktop GPU from the last several years. Keeping the CPU-driven path alive would double every pass. `RhiDevice::init` fails with
  a clear message if a feature is missing.
- **GPU-driven is the only path.** No CPU-driven fallback, no toggle between the two. Stage 4 feeds its indirect commands from the existing CPU
  masks only as a step toward stage 5. Stage 5 deletes `cullInstances`, `cullShadowCascades` and `RenderSnapshot::visible`/`shadowVisible`, rather
  than keeping them as a reference.
- **Debugging and introspection get their own pass after stage 5.** Stages 4 and 5 remove the CPU-side draw that the draw log, "Time draws",
  `LargeDraw` zones and per-draw stats rely on (`FrameGraphContext::beginDraw`). Those stages keep the tools compiling and correct at per-pass
  granularity, and accept losing per-draw detail in the meantime. A follow-up plan (`plan_gpu_driven_debugging.md`, to write after stage 5)
  rebuilds per-draw and per-view debugging on GPU-produced data: command and count readback, and GPU-side counters. It also adds the new introspection
  that GPU-driven rendering needs, such as the culling results per view and the command buffer contents, in the Render Debug window and in
  `--dump-render-debug`, for agents too (`docs/plan_agent_introspection.md`).
- **The RHI stays Vulkan-shaped for now.** Bindless and indirect are added in the RHI's current descriptor model (`src/rhi/README.md` known gaps).
  The descriptor-model revisit stays gated on a second backend, as the README already says. D3D12 and Metal both have native equivalents, so nothing
  here blocks one.

## Open questions

- **Delta uploads beyond transforms.** Stage 1 uploads dirty transform ranges. Material edits (stage 3) and mesh add or remove (stage 2) need the
  same mechanism. Should one shared "GPU scene upload" path serve all three, or one path per table? Decide when stage 2 is planned.

## Verification (end state)

Each stage plan has its own binary criteria. The umbrella is done when all of these hold on the Sponza courtyard headless run:

- Screenshot byte-identical to the pre-stage-1 baseline, with prepass off and on, three cascades.
- `--dump-render-debug`: `GeometryPass` and `DepthPrepass` record at most one draw command per bucket, and `ShadowPass` at most one per bucket per
  cascade. `RhiCommandStats::draws` counts indirect calls.
- `--dump-profile`: no culling zone on the main thread. An `InstanceCull` GPU zone per view.
- `RenderStats.culled` and `shadow_culled` equal the counts the CPU culling reported before stage 5 on the same camera, recorded as reference numbers
  before the CPU code is deleted.
- No CPU culling code remains (`cullInstances`, `cullShadowCascades`).
- `--fail-on-validation` exits 0. All RHI examples, including the new `bindless` and `indirect` ones, pass `--check --validation`.
- GPU frame time no worse than the pre-stage-1 baseline, measured the same way as in `docs/plan_shadow_cascades.md`.

## Deferred / follow-ups

- **Debugging and introspection pass** (`plan_gpu_driven_debugging.md`). Trigger: stage 5 lands. See the decision above.

- **Meshlet or cluster culling.** Trigger: asset packing lands with a cook step that can generate meshlets (`notes.md`). Extends stage 5's culling
  pass with a second level.
- **Two-phase occlusion culling (HZB).** Trigger: after stage 5, the geometry pass is still dominated by hidden instances on a scene with real
  occlusion (Jungle Ruins interior, Sponza from inside the gallery).
- **Async compute for culling.** Trigger: GPU timestamps show idle gaps that the culling dispatches could fill.
- **GPU-side LOD selection.** Trigger: asset packing produces LOD chains.
