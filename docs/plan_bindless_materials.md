# Bindless materials

**Status. Landed.**

Stage 3 of [plan_gpu_driven.md](plan_gpu_driven.md).

## Current state

- **Descriptor sets per instance.** `Renderer::rebuildGeometryDescriptorSets` allocates `frameSlots × instances` sets. Each holds the view UBO
  (binding 0), the instance's material texture with `materialSampler` (binding 1), and, since stage 1, the instance buffer (binding 2). The geometry
  pass and the depth prepass bind one set per draw: Sponza has 65 descriptor binds for 65 geometry draws at the courtyard camera.
- **Materials are one texture each.** `gbuffer.frag` samples `texSampler` and multiplies by the vertex colour. `MaterialDesc::baseColorFactor` is
  not read by the renderer:
  - Untextured materials encode the factor into a 1×1 texel (`usdscene.cpp`, `extractMaterial`).
  - Vertices are white.
  - A textured material's factor is ignored today. This is a pre-existing gap and stays out of scope, so pixels don't change.
- **Missing textures** fall back to `fallbackTexture`, a pink checkerboard.
- **Texture storage.** `textureCache` (keyed by material index) holds the GPU textures. Textures re-upload with every geometry change.
  `applySamplerSettings` rebuilds all sets when the material sampler changes.
- **The device** (RADV, Radeon 890M) reports `shaderSampledImageArrayDynamicIndexing`, the full descriptor-indexing set (`runtimeDescriptorArray`,
  `descriptorBindingPartiallyBound`, `shaderSampledImageArrayNonUniformIndexing`), and `maxPerStageDescriptorSampledImages` 8,388,606. The RHI
  enables none of the indexing features. `RhiDescriptorBinding` has no array count and `RhiDescriptorWrite` no array element.

Stage 4's multi-draw can't switch descriptor sets between draws. The texture a draw samples has to come from data the shader indexes.

## Scope

**In**

- Descriptor arrays in the RHI: `RhiDescriptorBinding::count` and `RhiDescriptorWrite::arrayElement`, both defaulting to today's behaviour.
- A texture array in the geometry descriptor set, a GPU material table, and a material index per instance. `gbuffer.frag` samples
  `textures[materials[instanceMaterial].baseColorTexture]`.
- One geometry descriptor set per frame slot instead of one per slot per instance. The geometry pass and the prepass bind it once per pipeline.
- `ngen-example-bindless`: the RHI verification for descriptor arrays and indexing, run before the renderer switches.

**Out**

- Using `baseColorFactor` for textured materials, and more texture slots (normal, roughness). Pixels must stay byte-identical, and the material
  model is its own plan.
- Delta uploads for material edits. Material changes set `geometryChanged` today (`uploadRenderWorld` compares `material`), so the table rebuilds
  with the geometry. Trigger: a material edit path that skips the geometry rebuild.
- Streaming or in-place texture updates. Textures re-upload together, as today.
- The shadow pass. It doesn't sample materials, and alpha-tested shadows would be the trigger.

## Decisions

All five recommendations were confirmed and are locked. Two details changed during implementation; both are noted where they apply.

1. **Descriptor model: fixed-size array or full descriptor indexing?**
   - (a) **Fixed-size array** `sampler2D textures[maxTextures]` with every slot written: real textures, and the fallback texture in the unused slots.
     A new set is allocated when textures change (the old one goes through the deletion queue), so nothing is updated while bound. Indexing is
     dynamically uniform, because every draw reads one material. That needs only `shaderSampledImageArrayDynamicIndexing`, a Vulkan 1.0 core
     feature, and no extension. *Correction during implementation:* the feature still has to be enabled at device creation. `RhiDeviceVulkan::init`
     now enables it and fails with a message if the device lacks it.
   - (b) **Full descriptor indexing**: a runtime-sized array with a variable descriptor count, partially bound, update-after-bind, and
     `nonuniformEXT`. It needs four features enabled, the update-after-bind pool and layout flags, and a variable-count allocate path in the RHI.

   I lean (a). It covers stage 4 too: in a multi-draw, each draw is its own invocation group, so a per-draw material index is still dynamically
   uniform. (b) buys in-place updates and no fixed ceiling, and neither is needed until textures stream. With `maxTextures` 1024, Sponza uses 29
   slots. Trigger for (b): texture streaming, or a scene over the ceiling. That scene would log a warning and use the fallback past the limit.
2. **Combined image samplers or separate images plus one sampler?**
   - (a) An array of combined image samplers, all using `materialSampler`. This is today's descriptor type, so there is no RHI type change. A
     sampler setting change reallocates the set, as `applySamplerSettings` already rebuilds sets today.
   - (b) An array of sampled images plus one `sampler` binding. It needs `SampledImage` and `Sampler` descriptor types in the RHI and in shaders.
     It is closer to D3D12 and Metal, and makes sampler changes a one-descriptor write.

   I lean (a) now. (b) comes with the descriptor-model revisit that the README gates on a second backend.
3. **How a draw finds its material.**
   - (a) The instance record grows from `mat4 model` to `{ mat4 model; uint material; uint pad[3]; }` (std430, stride 80). The vertex shader passes
     the material index to the fragment shader as a `flat` varying.
   - (b) A second per-instance buffer holding only material indices, uploaded in bulk on geometry change.

   I lean (a). Stage 5's culling reads bounds and mesh per instance, so the instance record is where per-instance data goes. One table is simpler
   than parallel arrays. Cost: the stage 1 delta upload writes 80-byte records, and instance buffer size goes from 64 to 80 bytes per instance.
4. **Material table contents.** `GpuMaterial { uint baseColorTexture; uint pad[3]; }`, the texture slot only. It holds what the shader reads and
   nothing else. Factors join when the material model plan makes them change pixels. The table is uploaded in bulk with the geometry, through
   `GpuScene`.
5. **Set layout: one set per frame slot, or per-frame and scene sets split?**
   - (a) One set per slot: UBO, textures, instances, materials.
   - (b) Set 0 per slot for the UBO, set 1 shared for the scene tables.

   I lean (a). It is 3 sets instead of 3 × instances, and one bind per pipeline either way. Splitting pays off when several passes share the scene
   set with different per-frame data, which happens with stage 5's culling pass. Trigger for (b): stage 5.

## Steps

1. **RHI** (`rhitypes.h`, `rhidevicevulkan.cpp`).
   - `RhiDescriptorBinding` gains `uint32_t count = 1`. The layout's `descriptorCount` and the pool sizes (`count × maxSets`) use it.
   - `RhiDescriptorWrite` gains `uint32_t arrayElement = 0`, which maps to `dstArrayElement`.
   - `RhiDeviceLimits` gains `maxPerStageSampledImages`.
   - `src/rhi/README.md` gets the new fields, and the examples table gets a new row.
2. **`ngen-example-bindless`** (`src/rhi/examples/bindless.cpp`).
   - Eight 1×1 textures of distinct colours in one `sampler2D[8]` binding.
   - Eight quads drawn with `firstInstance = i`. The fragment shader indexes `textures[instanceIndex]` through a flat varying.
   - `--check` reads each quad's centre and compares it with texture `i`'s colour.
3. **Instance record** (`gpuscene.h/.cpp`, the three vertex shaders). `GpuInstanceRecord { glm::mat4 model; uint32_t material; uint32_t pad[3]; }`.
   The staging write copies records, and capacity bytes use `sizeof(GpuInstanceRecord)`. `shadow.vert` and `depthonly.vert` declare the same struct
   and read `.model`.
4. **Material table** (`gpuscene.h/.cpp`).
   - `GpuScene::rebuildMaterials(std::span<const GpuInstance>, const std::unordered_map<uint32_t, CachedTexture>&, RhiTexture* fallback, GpuUploader&, uint64_t frame)`
     assigns texture slots: 0 is the fallback, 1..N the material textures in instance order.
   - It builds `GpuMaterial` per material index and the per-instance material index, and uploads the table in bulk.
   - It exposes `textureSlots()` (slot to `RhiTexture*`) for the descriptor writes.
   - It is called from `uploadRenderWorld` after the texture loop.
5. **Descriptor sets** (`renderer.cpp`, `geometrypass.cpp`).
   - Layout: binding 0 UBO, binding 1 `CombinedImageSampler` with `count = maxTextures` (fragment stage), binding 2 instances, binding 3 materials.
   - `rebuildGeometryDescriptorSets` allocates `frameSlots` sets and writes every array element: the slot's texture, or the fallback.
   - The passes take one `RhiDescriptorSet*` for the frame slot and bind it right after `bindPipeline`. The per-draw `bindDescriptorSet` goes away.
6. **Shaders.**
   - *Changed during implementation:* the material lookup happens in the vertex shader rather than the fragment shader. `gbuffer.vert` reads
     `materials.data[instances.data[gl_InstanceIndex].material].baseColorTexture` and passes the slot as `layout(location = 3) flat out uint
     fragTexture`. That is one table read per vertex instead of per fragment, and the material table binding is vertex-stage only.
   - `gbuffer.frag` declares `layout(set = 0, binding = 1) uniform sampler2D textures[1024]` and samples `textures[fragTexture]`.
     `textureQueryLod` uses the same element.
7. **Observation.** `MaterialTableBuilt { materials, texture_slots, max_textures }` and `GeometryDescriptorsRebuilt { sets, reason }`, where
   `reason` is `geometry`, `sampler` or `instance_buffer`. `RenderStats` gains `material_count`. Per-pass `descriptorBinds` is already in the dump.

## Verification

Baseline: the stage 2 screenshots (byte-identical to the pre-stage-1 ones) and the stage 2 dump.

- `ngen-example-bindless --check --validation` exits 0. **Verified**: each of the 8 quads samples its own texture, with 1 descriptor bind. The other
  13 examples pass too.
- Screenshots byte-identical, prepass off and on. **Verified**: all six pairs identical.
- `--fail-on-validation` exits 0. **Verified** on all runs.
- Descriptor binds on Sponza at the courtyard camera. **Verified**: `GeometryPass` 65 to 1 for 65 draws (every draw is single-sided there, so one
  bucket). `draws`, `primitives` and `bufferBinds` are unchanged. `ShadowPass` is unchanged at 3.
- Material table. **Verified**: `MaterialTableBuilt` reports 28 materials and 29 texture slots, matching the dump's 28 materials with a texture.
  `RenderStats.material_count` is 28, and `instance_buffer_bytes` is 40,960 (512 entries of 80 bytes).
- Sampler changes rebuild the sets. **Verified**: `sampler aniso=1` and `sampler aniso=8` give two `GeometryDescriptorsRebuilt` with reason
  `sampler` (5 sets each, one per swapchain image).
- The `translate` script still moves the cube. **Verified**: the upload is 80 bytes with `carried_access` `StorageRead`, and the screenshot matches
  the stage 1 one.
- GPU time on Sponza within noise. **Verified**: `GeometryPass` 1.27 to 1.28 ms, `ShadowPass` 3.65 to 3.63 ms.

## Deferred / follow-ups

- Full descriptor indexing (runtime array, partially bound, update-after-bind). Trigger in decision 1.
- Separate sampled images and samplers. Trigger: the descriptor-model revisit with a second backend.
- Split per-frame and scene descriptor sets. Trigger: stage 5.
- Material model (factors on textured materials, more maps) as its own plan.
- Delta uploads for material edits. Trigger in Out.
