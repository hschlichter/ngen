# Geometry pass fragment cost: back-face culling, normal target format, depth prepass

**Status. Landed.**

## Current state

The geometry pass (`src/renderer/passes/geometrypass.cpp`) writes two colour targets, albedo `R8G8B8A8_UNORM` and normal
`R32G32B32A32_SFLOAT` (16 bytes per pixel, 59 MB per full write at 2560x1440), plus depth. Rasterisation is `RhiCullMode::None` in
both the geometry and the shadow pass (`shadowpass.cpp`), so back faces of every closed mesh are shaded. Draws run in prim order with
`Less` depth test and no prepass, so hidden fragments still sample the material texture and write both targets. Sponza from inside the
courtyard: 25.9 ms geometry pass, of which about 20 ms is fragment work.

Three knobs on the same pass, each shippable and measurable alone, in one plan because they touch the same two files and share the
measurement. Sibling plans from the same analysis: `docs/plan_frustum_culling.md`, `docs/plan_texture_mipmaps.md`.

## Scope

**In**

1. Back-face culling in the geometry and shadow pipelines, honouring USD `orientation` (rightHanded is counter-clockwise front faces, the
   default; leftHanded meshes get their triangle winding flipped in the extractor so one pipeline state serves both).
2. Normal target to `R16G16B16A16_SFLOAT`. Quarter the bytes, no shader change beyond the format; the lighting pass already decodes
   `rgb * 2 - 1`.
3. Depth prepass: a position-only pipeline draws all visible instances to the depth target first; the geometry pass then runs with
   `RhiCompareOp::Equal` and depth writes off, so only the front-most fragment per pixel is shaded. Frame-graph pass `DepthPrepass`
   before `GeometryPass`, sharing the depth handle.
4. A `doubleSided` flag per material from `UsdGeomGprim` (`doubleSided` attribute) so two-sided assets draw without culling: instances
   partitioned into two loops, one per pipeline.

**Out**

- Octahedral `R16G16` normals. Another 2x on the normal target for a shader encode/decode pair; measure after step 2 first. Trigger:
  the normal target still visible in the per-pass numbers.
- Front-to-back sorting. The prepass makes the geometry pass order irrelevant; sorting would only speed up the prepass itself.
  Trigger: the prepass costs more than a third of the old geometry pass.
- Position-only vertex stream for prepass and shadow pass (`notes.md` item 3). The prepass reads the full 44-byte vertex, like the
  shadow pass. Trigger: prepass plus shadow pass vertex fetch shows in the numbers after culling lands.

## Decisions

- **Cull back faces by default, opt out per material.** Sponza main is closed stone; Kitchen_set props are closed. USD says a mesh is
  single-sided unless `doubleSided` is authored, so the renderer follows USD. Two pipelines (cull back, cull none) rather than a dynamic
  state, since dynamic cull mode is not in the RHI and would be its own step.
- **Flip winding in the extractor for leftHanded meshes** instead of a per-mesh front-face state. One place, at load, no render-side
  branching. `orientation` is read once per mesh next to `subdivisionScheme`.
- **Equal-depth geometry pass, not LessOrEqual.** `Equal` with writes off is the standard prepass contract and rejects every hidden
  fragment; `LessOrEqual` would shade the front-most fragment twice on coplanar draws. Both pipelines use the same vertex shader
  transform, so the depths match bit for bit.
- **Prepass draws every visible instance, not only the heavy ones.** Simpler, and the prepass is depth-only at 6 ms for the whole
  scene today (the shadow pass proves the number), less after culling. Rejected: a heuristic prepass of large meshes only.
- **Prepass off by default** (decided after measuring). From the courtyard the scene has little overdraw once back faces are culled,
  so the prepass's second vertex pass costs more than the shading it removes: 5.7 ms geometry alone against 3.3 + 4.3 ms with the
  prepass, culling on. It stays as a toggle (Debug menu, `prepass on|off`) for views where overdraw dominates. Trigger to flip the
  default: a scene where the toggle wins.
- **Depth-only shader shares the gbuffer UBO and declares `gl_Position` invariant.** The first version reused `shadow.vert` with a
  CPU-side view-projection; the depths differed in the last bits and `Equal` dropped pixels. `depthonly.vert` now evaluates the same
  expression from the same uniforms as `gbuffer.vert`, both invariant.

## Steps

1. **Extractor.** `usdscene.cpp`: read `geomMesh.GetOrientationAttr()`; when `leftHanded`, swap the second and third corner of every
   fan triangle. Read `UsdGeomGprim::GetDoubleSidedAttr()` into `MaterialDesc::doubleSided` (a mesh flag stored on the submesh's
   material binding, since instances are per submesh). `RenderMeshInstance` and `GpuInstance` gain `bool doubleSided`.

2. **Pipelines.** `GeometryPass::init` builds `pipelineCullBack` and `pipelineCullNone`; `ShadowPass::init` likewise. Draw loops run
   single-sided instances first, then double-sided, one `bindPipeline` each. Measure: Sponza has no double-sided meshes, so the second
   loop is empty there.

3. **Normal format.** `geometrypass.cpp` `colorFormats[1]` and `normalDesc.format` to `R16G16B16A16_SFLOAT`; the frame-graph preview
   and the buffer overlay pick up the format from the descriptor. Check `supportsTextureFormat` at init, fall back to the float format
   with a log line.

4. **Depth prepass.** `src/renderer/passes/depthprepass.h/.cpp`, same shape as `ShadowPass`: `init(device, depthFormat)`,
   `addPass(fg, depthHandle, extent, instances, visible, meshCache)`; vertex shader `shaders/depthonly.vert` (position and model
   push only), no fragment shader. `GeometryPass` pipelines get `depth = {.testEnable = true, .writeEnable = false, .compareOp = Equal}`
   and the pass reads the depth handle with `FgAccessFlags::DepthAttachment` after the prepass has written it. Frame graph: the prepass
   is the first writer of `depth`, the geometry pass the second; `PassExecuted` order shows it.

5. **Toggle.** `RenderSnapshot::depthPrepass` bool, default on, Debug menu "Depth Prepass", session verb `prepass on|off`, so the gain is
   measurable in one run.

## Verification

- Sponza headless at the courtyard camera, one run per step with the others off, `--dump-render-debug` per run: `GeometryPass` GPU ms
  after step 1, after step 3, after step 4; each lower than the previous. Target: geometry plus prepass under half of 25.9 ms.
  Verified, all with mipmaps already landed (10.5 ms baseline): back-face culling plus 16-bit normals 8.8 ms with culling off; with
  frustum culling on 5.7 ms; prepass on 3.3 + 4.3 ms. Whole frame 34.0 to 13.0 ms GPU with the defaults (cull on, prepass off).
- Screenshots at the courtyard camera before and after each step: pixel-identical for steps 2 and 4 (the same fragments win),
  identical for step 1 except where back faces were visible through open geometry (none expected in Sponza; agent diffs the PNGs).
  Verified for the prepass: 7826 of 3.7 M pixels differ, 78 by more than 8 levels, coplanar surfaces where `Equal` lets the later
  draw win, spread by FXAA. No holes.
- Kitchen_set at the fixed camera: no holes appear with back-face culling (the props are closed); three_cubes unchanged. Verified by
  screenshot.
- `FrameGraphCompiled` pass count is one higher with the prepass on, and `PassExecuted` shows `DepthPrepass` before `GeometryPass`.
- Validation clean; example sweep unchanged.

## Deferred / follow-ups

- Octahedral normals, front-to-back sort, position-only stream: triggers above.
- Per-draw pipeline state from USD beyond `doubleSided` (for example `purpose`, `visibility` already handled elsewhere).
