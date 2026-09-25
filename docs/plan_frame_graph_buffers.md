# Buffer resources in the frame graph

**Status. In progress.** Implemented and verified headless; the Frame Graph node view check is pending a look in the editor.

Stage 1 of [plan_gpu_driven.md](plan_gpu_driven.md).

## Current state

The frame graph tracks textures only. `FgBufferHandle` is declared in `src/renderer/framegraphresource.h` but has no users, and `FrameGraph::execute`
only emits `RhiTextureBarrierDesc`. The RHI side is ready: `RhiBufferState`, `RhiBufferBarrierDesc` and `pipelineBarrier(textures, buffers)` exist and
are exercised by `src/rhi/examples/compute.cpp` (`docs/plan_rhi_compute.md`). The renderer itself issues no buffer barriers: mesh buffers are uploaded once
through `GpuUploader`, and per-frame data is either a CPU-mapped UBO or push constants.

Every imported resource starts each frame in `FgAccessFlags::None`, which maps to `Undefined`. That is harmless for the resources imported today: the
backbuffer comes from the swapchain, and the depth buffer is cleared every frame. It is wrong for a persistent resource whose contents and pending reads
carry over from the previous frame.

Per-instance data is a push constant. `gbuffer.vert` and `depthonly.vert` read `push.model`, and `shadow.vert` reads `ShadowPush { lightViewProj, model }`.
Every stage after this one reads instance data from a GPU buffer (`docs/plan_gpu_driven.md`).

## Scope

**In**

- Buffer resources in `FrameGraph`: `importBuffer`, `FrameGraphBuilder::read/write(FgBufferHandle, FgAccessFlags)`, `FrameGraphContext::buffer`, and
  buffer barriers derived per pass alongside the texture barriers.
- **Cross-frame state for imported buffers.** The caller passes the resource's access state from the end of the previous frame, and reads back the
  final state after `execute`.
- Buffers in the debug snapshot, so the Frame Graph window and node view list them (size and usage instead of extent and format; no preview).
- **One persistent GPU instance buffer** (`Storage | TransferDst`, `GpuOnly`) holding each instance's model matrix.
  - Delta upload: each frame the renderer writes the dirty span into a per-slot staging buffer, and an `InstanceUpload` pass copies it into the
    instance buffer.
  - The shadow, prepass and geometry passes read `instances[gl_InstanceIndex]` instead of the push constant.

**Out**

- Transient (graph-created, pooled) buffers. They arrive with their first consumer, stage 5's culling output (`docs/plan_gpu_driven.md`).
- Cross-frame state for imported textures. Today's texture imports don't need it (see Current state). Trigger: the first persistent texture read
  across frames, such as TAA history (`docs/frame_graph_next.md`).
- New access flags (`VertexRead`, `IndexRead`, `UniformRead`, `IndirectRead`). The consumer uses only the existing `TransferSrc`, `TransferDst` and
  `StorageRead`. Add each flag with its first user; `IndirectRead` is stage 4.
- Tracking mesh vertex/index buffers or UBOs in the graph. The geometry pool (stage 2) revisits the mesh buffers.
- Material, bounds or mesh indices in the instance record. They arrive with the stages that read them (3 and 5).
- Moving cascade matrices out of the shadow push constant.

## Decisions

- **One resource list with a kind tag, separate handle types.** `FgResource` gains `FgResourceKind kind` and an `FgBufferDesc`. Buffers share the
  `resources` vector with textures, so the adjacency, topological sort, pass culling and lifetime code in `FrameGraph::compile` apply to buffers
  unchanged. `FgTextureHandle` and `FgBufferHandle` stay distinct types, so a buffer can't be passed where a texture is expected. The alternative, a
  separate `buffers` vector, would duplicate the dependency code or need a combined index space anyway.
- **One persistent instance buffer, not per-slot copies.** Instance data is scene state that outlives a frame, and stages 3 to 5 read it on the GPU
  as a single table. Frame N+1's upload is ordered after frame N's reads on the one queue. The barrier from `StorageRead` to `TransferDst` makes that
  ordering a real dependency, provided the graph knows the buffer was in `StorageRead`. That is the next decision.
- **Explicit carried state, owned by the caller.** `importBuffer(name, buffer, desc, initialAccess)`, then `FrameGraph::finalAccess(FgBufferHandle)`
  after `execute`. The renderer stores the value beside the buffer. The alternative is a graph-internal map keyed by `RhiBuffer*`. It is invisible to
  the caller, and it goes stale when a buffer is destroyed and a new one lands at the same address. Explicit state costs one field per persistent
  buffer. A history-resource API can wrap it once there are several (`docs/frame_graph_next.md`).
- **Delta upload as one dirty span per frame.** `uploadRenderWorld` already compares transforms per instance. It records the lowest and highest
  changed index, and the next frame copies that span.
  - Moving one prim gives a one-entry span. A full-scene change gives a full copy.
  - Spans from several frames in a row merge until a frame uploads them, since snapshots can arrive faster than frames.
  - Multi-range copies wait until a profile shows the span wasting bandwidth.
- **Per-slot staging, capacity-sized.** `instanceStaging[slot]` is `TransferSrc`, `CpuToGpu` and persistently mapped. The slot's fence guards it
  like the slot's UBO. The dirty span is written at the same offset as in the instance buffer, so the copy is a single `RhiBufferCopy` with
  `srcOffset == dstOffset`.
- **Resize recreates.** When the instance count exceeds capacity, `uploadRenderWorld` recreates the instance buffer and the staging buffers at the
  new capacity. The old ones go through the `deletionQueue`, the carried state resets to `None`, and the dirty span covers every instance.
- **Instance index through `firstInstance`.** `drawIndexed(count, 1, firstIndex, 0, m)`, and the shader reads `instances[gl_InstanceIndex]`
  (Vulkan includes the base instance). The umbrella plan explains why this carries through to indirect draws.
- **Instance layout is `mat4 model` only (std430, stride 64).**

## Steps

1. **Resource model** (`src/renderer/framegraphresource.h`).
   ```cpp
   enum class FgResourceKind : uint8_t {
       Texture,
       Buffer,
   };

   struct FgBufferDesc {
       uint64_t size = 0;
       RhiBufferUsageFlags usage = {};
   };

   struct FgResource {
       const char* name = "";
       FgResourceKind kind = FgResourceKind::Texture;
       FgTextureDesc desc;
       FgBufferDesc bufferDesc;
       FgAccessFlags currentAccess = FgAccessFlags::None;
       RhiTexture* physical = nullptr;
       RhiBuffer* physicalBuffer = nullptr;
       bool external = false;
       uint32_t firstUseOrder = UINT32_MAX;
       uint32_t lastUseOrder = 0;
   };
   ```
2. **Graph API** (`framegraph.h/.cpp`, `framegraphbuilder.h`, `framegraphcontext.h`).
   - `FrameGraph::importBuffer(const char* name, RhiBuffer* buffer, const FgBufferDesc& desc, FgAccessFlags initialAccess = FgAccessFlags::None)
     -> FgBufferHandle`.
   - `FrameGraph::finalAccess(FgBufferHandle) const -> FgAccessFlags`: the access after the last pass that touched the buffer, or `initialAccess`
     if no pass ran.
   - `FrameGraphBuilder::read(FgBufferHandle, FgAccessFlags)` and `write(FgBufferHandle, FgAccessFlags)`, pushing onto the same `reads`/`writes` lists.
   - `FrameGraphContext::buffer(FgBufferHandle) -> RhiBuffer*`.
   - In `execute`, `resourceAccess` is seeded from each resource's `currentAccess`: the initial access for imports, `None` otherwise. `checkTransition`
     branches on `kind`. Buffers map through a new `accessToBufferState(FgAccessFlags) -> RhiBufferState` (`StorageRead`, `StorageWrite`,
     `TransferSrc`, `TransferDst`) and collect into a `std::vector<RhiBufferBarrierDesc>`. The call becomes
     `cmd->pipelineBarrier(textureBarriers, bufferBarriers)`. At the end, the final access is written back to `currentAccess` for `finalAccess`.
   - Transient allocation and release, and the debug capture hook, skip `Buffer` resources.
3. **Debug snapshot** (`framegraphdebug.h`, `FrameGraph::buildDebugSnapshot`, `src/ui/framegraphwindow.cpp`, `src/ui/framegraphnodeview.cpp`,
   `framegraphpreviews.cpp`). `FgResourceDebug` gains `bool buffer` and `uint64_t sizeBytes`. The label reads `name (imported, buffer 7.2 KB)`. The
   previews skip buffers, and the windows show size and usage where they show extent and format.
4. **Instance buffer** (`renderer.h/.cpp`). Members:
   - `RhiBuffer* instanceBuffer`
   - `FgAccessFlags instanceBufferAccess`
   - `std::vector<RhiBuffer*> instanceStaging` and its mapped pointers, one per frame slot
   - `uint32_t instanceCapacity`
   - `uint32_t dirtyFirst` and `dirtyEnd` (empty when equal)

   `uploadRenderWorld` extends the dirty span where it compares transforms, and resizes as described in Decisions. The resize happens before
   `rebuildGeometryDescriptorSets` and the shadow set writes, so both see the current buffer.
5. **InstanceUpload pass** (`addInstanceUploadPass` in `renderer.cpp`, beside `addBlitPass`). The renderer imports the instance buffer with
   `instanceBufferAccess` and the slot's staging buffer with `None`. If the dirty span is non-empty, the renderer writes it into staging and adds the
   pass. The pass reads staging with `TransferSrc`, writes the instance buffer with `TransferDst`, and calls `copyBuffer`; the span is then cleared.
   The pass has no side effects, and stays alive because the draw passes read its output. After `execute`,
   `instanceBufferAccess = frameGraph.finalAccess(instances)`.
6. **Consumers.**
   - Geometry: descriptor binding 2 `StorageBuffer`, vertex stage, in `GeometryPass::init`. The pool gets matching bindings in
     `rebuildGeometryDescriptorSets`, and every set writes the one `instanceBuffer`. The pipeline push range becomes size 0. `addPass` takes
     `FgBufferHandle instances` and declares `read(instances, StorageRead)`. The draw passes `firstInstance = m`.
   - Depth prepass: shares the geometry set layout, so it only needs the handle, the read and `firstInstance`.
   - Shadow: new set layout `{binding 0, StorageBuffer, vertex}` and one set, rewritten when the buffer is recreated (deferred free of the old set).
     The push constant becomes `mat4 lightViewProj` only. The draw passes `firstInstance = m`.
   - Shaders: `gbuffer.vert`, `depthonly.vert` and `shadow.vert` declare
     `layout(std430, set = 0, binding = N) readonly buffer Instances { mat4 model[]; } instances;`. `gbuffer.vert` and `depthonly.vert` stay
     textually identical in the position expression, so `invariant gl_Position` still holds for the Equal test.
7. **Observation.** `RenderStats` gains `instance_buffer_bytes` and `instance_upload_bytes` (this frame's span, 0 when clean). An `InstanceUpload`
   event on each upload frame reports `first`, `count`, `bytes`, `carried_access`, and the barrier counts of the upload and shadow passes.
8. **Per-pass barrier counts include the graph's barriers** (added during implementation). The per-pass stats window used to open after the graph
   emitted a pass's transitions, so `barriers` in the Render Debug window and in `--dump-render-debug` only counted barriers a pass recorded itself,
   which is almost always 0. The window now opens before the graph's barriers. Frame totals in `RenderStats` were always command-buffer totals and
   don't change.
9. **`translate /prim dx,dy,dz` session verb** (added during implementation, `src/main.cpp`, `src/session/sessionscript.h`). It submits a preview
   `SetTransform`, the same edit a gizmo drag submits. The verification below needs it, and agents can use it to drive transform edits headless.

## Verification

Baseline: screenshots from the `HEAD` binary before the change (`adc928a`), same flags. Scenes: three_cubes, and Sponza at the courtyard camera and
framed on the whole scene (`--camera-frame=scene`), 200 frames each.

- Screenshot byte-identical before and after, with prepass off and with `prepass on`. **Verified**: all six pairs are identical files (cubes, cubes
  with prepass, Sponza courtyard, Sponza courtyard with prepass, Sponza framed, Sponza framed with prepass).
- `--fail-on-validation` exits 0 on both scenes. **Verified** on all six runs and on the move run below.
- First frame uploads every instance. **Verified**: `InstanceUpload` on frame 1 with `count` equal to the instance count, `carried_access` `None`.
  On Sponza, 407 instances grow the buffer from 256 to 512 entries (`InstanceBufferCreated` twice).
- Steady state uploads nothing. **Verified**: `instance_upload_bytes == 0` in every `RenderStats` after frame 1, and one `InstanceUpload` pass per
  run. Clean-frame `barriers` stays at 16 on Sponza, the same as before.
- Moving one prim uploads one entry and syncs with the previous frame's reads. **Verified** with `10 translate /World/Cube_1 0,0,1` on three_cubes:
  - `InstanceUpload` has `first` 1, `count` 1, `bytes` 64 and `carried_access` `StorageRead`.
  - The upload pass has 2 barriers (staging, and instance `StorageRead` to `TransferDst`).
  - `ShadowPass` has 2 barriers on that frame against 1 on clean frames (the added one is `TransferDst` to `StorageRead`).
  - The screenshot after the move shows the green cube moved toward the camera.
- Frame Graph node view: `InstanceUpload` has edges to `ShadowPass`, `DepthPrepass` (when on) and `GeometryPass`, and the resource list shows
  `instances` and `instanceStaging` as imported buffers. **Pending**: UI only, needs a look in the editor.
- GPU time on Sponza within noise. **Verified**: `ShadowPass` 3.73 to 3.65 ms and `GeometryPass` 1.27 to 1.21 ms at the courtyard camera.

## Deferred / follow-ups

- Transient pooled buffers: stage 5 (`docs/plan_gpu_driven.md`).
- Cross-frame state for imported textures, and a history-resource API over the explicit state: trigger above.
- Multi-range delta uploads: when a profile shows the single span copying mostly clean entries.
- Access flags `VertexRead`, `IndexRead`, `UniformRead`: with their first graph-tracked user.
