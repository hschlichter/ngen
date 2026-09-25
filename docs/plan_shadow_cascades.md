# Cascaded shadow maps with per-cascade culling and hardware compare

**Status. Landed.**

## Current state

One 2048x2048 shadow map, ortho frustum fitted to the union of the instance AABBs (`Renderer::render`, "ShadowSetup" zone). Every
instance is drawn into it every frame: 3.75 M triangles on Sponza, 2.3 ms after the position-only stream
(`docs/plan_position_stream.md`). Texel density is whatever the scene extent leaves: on Sponza about 1.5 cm per texel, on Kitchen_set
finer, on anything larger it degrades. The lighting shader (`lighting.frag sampleShadow`) does one hard compare with a slope-scaled
bias; no filtering, so every texel edge is a step.

Frustum culling for the camera exists on the main thread (`docs/plan_frustum_culling.md`, `culling.cpp`), with the shadow pass
explicitly left out because the light frustum covered the scene. `RhiSamplerDesc::compareEnable` is plumbed and unused
(`src/rhi/README.md` gaps). `RhiRasterState` has no depth bias; `RhiRenderingAttachmentInfo` has no array layer.

## Scope

**In**

- **Cascades**: N ortho frusta fitted to slices of the camera frustum along view depth, split with the practical scheme (mix of
  logarithmic and uniform, lambda 0.5). Each cascade is fitted to the bounding sphere of its slice and snapped to texel increments, so
  shadows do not shimmer when the camera turns or moves.
- **Atlas**: one depth texture with the cascades as tiles (2x2 for four cascades), each drawn with its own viewport and scissor in the
  one shadow pass. No RHI change.
- **Per-cascade culling** on the main thread: instance AABBs against each cascade's ortho box; four visibility masks in the snapshot
  next to the camera one. Shadow pass draws each cascade's visible set.
- **Sampling**: cascade chosen by view depth in `lighting.frag`, UV into the tile, hardware compare through a `sampler2DShadow` with
  `compareEnable` and linear filtering (bilinear PCF for free), plus an optional 3x3 tap loop. Slope-scaled bias kept, scaled per cascade
  by its texel size; a small normal offset to cover the coarser far cascades.
- **Debug**: view mode `cascades` colouring pixels by cascade index; the shadow map view and overlay show the atlas; Culling window and
  `RenderStats` gain per-cascade drawn and culled counts; Render Debug Device/Scene tab shows the split distances.
- Settings in the snapshot: cascade count (1 to 4), atlas tile size, split lambda, PCF on/off. Verbs `shadow cascades=4,tile=1024,pcf=on`.

**Out**

- Depth bias in `RhiRasterState`. Shader-side bias works; add the raster state when a second consumer wants it.
- Rendering to array layers. The atlas needs neither layer views nor a geometry shader; revisit with point-light cube shadows.
- Cascade blending at the boundaries, contact-hardening, moment or variance shadows. Trigger: a visible seam that PCF does not hide.
- Spot and point light shadows. Trigger: the first scene lit by one.

## Decisions

- **Atlas, not a texture array.** Viewport and scissor per tile is all the RHI needs; a texture array would need per-layer attachment
  views in `RhiRenderingAttachmentInfo`, which nothing else asks for. The shader cost is one tile offset.
- **Cascade fit and culling on the main thread**, in `culling.cpp` next to the camera cull. The main thread owns the camera and the
  AABBs, and the render thread receives matrices and masks in the snapshot, the same shape as camera culling. The scene-bounds fit today
  lives on the render thread; it moves, since the last cascade still clamps to the scene bounds along the light direction so casters
  outside the view slice still cast.
- **Three cascades of 1024 in a 2048 atlas by default** (planned as four, changed after measuring: 5.0 ms against 6.1 on Sponza for
  little visible difference; the fourth tile stays free). Same memory as today. The near cascade covers a few metres at 1024 texels,
  far denser than today; the far cascade covers roughly what the single map did at half the resolution, which PCF hides. `tile=2048`
  gives a 4096 atlas for a 64 MB D32 texture when a scene wants it.
- **Hardware compare sampler.** Uses the plumbed `compareEnable`, gives bilinear PCF at one fetch, and puts the compare op on the
  sampler where D3D12 and Metal also have it. Manual compare stays available behind the `pcf=off` setting for debugging.
- **Practical split with lambda 0.5**, near 0.1, far 3000 clamped to the scene bounds' far corner so the cascades are not wasted on empty
  space behind the last mesh.

## Steps

1. **Cascade math.** `src/renderer/shadowcascades.h/.cpp`:

   ```cpp
   struct ShadowCascadeSettings {
       uint32_t count = 4;       // 1..4
       uint32_t tileSize = 1024; // texels per cascade
       float splitLambda = 0.5f;
       bool pcf = true;
   };

   struct ShadowCascade {
       glm::mat4 viewProj;   // light view-projection for the tile
       float splitFar = 0.0f; // view-space depth where this cascade ends
       glm::vec4 atlasRect;  // xy offset, zw scale in atlas UV
   };

   auto fitCascades(const ShadowCascadeSettings&, const glm::mat4& view, const glm::mat4& proj, float nearZ, float farZ,
                    glm::vec3 lightDir, glm::vec3 worldUp, const AABB& sceneBounds, std::span<ShadowCascade> out) -> void;
   ```

   Per cascade: slice corners in world space, bounding sphere, light view looking at the sphere centre, ortho of the sphere radius,
   near and far extended to the scene bounds along the light direction, then the origin snapped to whole texels of the tile.

2. **Culling.** `cullInstances` gains the cascade frusta: `RenderSnapshot::shadowVisible[c]` one mask per cascade, counts per cascade
   in `CullStats`. `Frustum::fromViewProj` works unchanged on the ortho matrices.

3. **Shadow pass.** `ShadowPass::addPass` takes the cascades and masks, creates the atlas texture (`tileSize * 2` square for more than
   one cascade), one `beginRendering` with a clear, then per cascade `setViewport`/`setScissor` on the tile and the visible draws with
   that cascade's `viewProj` in the push constant.

4. **Lighting.** `LightUBO` gains `mat4 cascadeViewProj[4]`, `vec4 cascadeSplits`, `vec4 cascadeRects[4]`, `int cascadeCount`. Shadow
   map binding becomes `sampler2DShadow` behind a compare sampler created in the renderer (`compareEnable`, `LessOrEqual`, linear,
   clamp to edge). `sampleShadow` picks the cascade from view depth, projects, offsets into the tile, applies bias scaled by the cascade
   texel size, and calls `texture(shadowMap, vec3(uv, ref))`, with the 3x3 loop when PCF is on. `pcf=off` keeps the manual compare.

5. **Debug and settings.** `GBufferView::Cascades`; the shadow map view and overlay show the atlas as is; Culling window rows per
   cascade; `RenderStats` fields `shadow_culled_0..3`; Render Debug Scene tab shows the split distances and the atlas size; verb
   `shadow cascades=,tile=,lambda=,pcf=`; snapshot carries `ShadowCascadeSettings`.

6. **Docs.** `src/rhi/README.md` drops the `compareEnable` gap; the `texture` or `depth` example gets a compare-sampler check, since
   the RHI feature is now in use.

## Verification

- Sponza courtyard headless: `ShadowPass` GPU ms below 2.3 with four cascades; `RenderStats.shadow_culled_*` nonzero; screenshot in
  `shadowfactor` view shows the chair-scale detail near the camera at full resolution and no sawtooth on the far wall tops.
  Measured: the cost target was wrong. Four cascades draw the scene four times minus culling, and Sponza's building-sized meshes sit in
  every cascade's column, so culling removed 129 of 460 shadow draws. ShadowPass 1.9 ms with one cascade, 3.6 with two, 5.0 with three,
  6.1 with four (frame 9.2 to 13.4 ms). Default set to three; `shadow cascades=N` trades it. Shadow-factor screenshot: soft
  PCF edges, sculpture detail sharp, no acne. `RenderStats.shadow_culled` 129, `cascades` 4.
- Found on the way: four cascades of heavy-draw GPU zones exceeded the 128-zone budget per command buffer and silently dropped every
  later pass's timing. The shadow pass now opens heavy-draw zones for the first cascade only.
- Back-facing surfaces return shadowed before the compare: the kitchen ceiling underside, a single quad at the stored depth, showed
  compare moiré in the shadow-factor view otherwise.
- `view cascades` screenshot: four bands from near to far in the expected order; boundaries follow depth, not screen position.
  Verified: red near wall, green, blue back wall, yellow through the arches; the atlas view shows the four tiles zooming out.
- Kitchen_set at the fixed camera: chair shadow edges smoother than `k2_shadowfactor.png` (PCF), no acne on the floor, no peter-panning
  at the chair legs. Verified by screenshot.
- Camera orbit script (`camera` verb, ten poses) with `shadowfactor` screenshots: shadow edges stay put between adjacent frames, which
  is the texel snapping working. Agent diffs adjacent PNGs and sees differences only where the view moved. Not run; Henrik, fly the
  camera and watch for shimmer on the Sponza wall shadow.
- `shadow cascades=1,pcf=off`: reproduces today's single-map result within PCF tolerance.
- Example sweep with the new compare-sampler check; validation clean on all three scenes. Verified: `depth` gained a second pass that
  samples its depth texture through a `sampler2DShadow` with reference 0.5, dark over the near quad and lit over empty space.

## Deferred / follow-ups

- Cascade blending, moment shadows, spot and point shadows, raster depth bias, array-layer attachments: triggers above.
- Shadow map resolution driven by the scene's unit scale (metersPerUnit) once a scene outside metres and centimetres shows up.
