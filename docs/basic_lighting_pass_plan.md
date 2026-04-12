# Basic Lighting Pass Implementation Plan

## Goal
Introduce a dedicated **lighting pass** to the renderer by splitting the current forward path into:
1. a **Geometry (G-buffer) pass** that writes material/normal data, and
2. a **Lighting pass** that shades a fullscreen target from those G-buffer textures.

This keeps the existing frame-graph approach and preserves current debug/UI overlays.

## Current baseline (what exists today)
- Scene rendering is done in one `ForwardPass` that writes directly to swapchain color and depth.
- The material fragment shader currently computes ambient + diffuse inline.
- Frame graph already supports creating transient textures and wiring pass `read`/`write` dependencies.

## Scope and constraints
- Keep this first version intentionally simple: one directional light + ambient term.
- Avoid changing mesh/material upload systems unless required.
- Preserve debug line and debug UI behavior (they should still draw after final lit color).

---

## File-by-file implementation plan

### 1) `src/renderer/renderer.h`
**Objective:** represent explicit geometry and lighting pass state.

- Replace forward-specific members with geometry naming:
  - `pipeline` -> `geometryPipeline`
  - `vertShader` / `fragShader` -> `geometryVertShader` / `geometryFragShader`
- Add lighting-pass members:
  - `RhiPipeline* lightingPipeline = nullptr;`
  - `RhiDescriptorSetLayout* lightingDescriptorSetLayout = nullptr;`
  - `RhiDescriptorPool* lightingDescriptorPool = nullptr;`
  - `std::vector<RhiDescriptorSet*> lightingDescriptorSets;`
  - `RhiShaderModule* lightingVertShader = nullptr;`
  - `RhiShaderModule* lightingFragShader = nullptr;`
- Add a small lighting-parameter struct (direction/intensity/ambient), either here or in `renderer.cpp`.

---

### 2) `src/renderer/renderer.cpp` (`Renderer::init` / `Renderer::destroy`)
**Objective:** create and tear down geometry + lighting pipelines and descriptors.

#### `Renderer::init`
- Keep the existing geometry descriptor layout for mesh draw (UBO + material texture).
- Build a **geometry pipeline** with MRT outputs:
  - RT0: albedo/base color
  - RT1: normal
- Build a **lighting pipeline**:
  - fullscreen draw (triangle list)
  - descriptor layout for sampled G-buffer textures
  - depth test/write disabled
- Allocate/update lighting descriptor sets (typically per swapchain image) to bind:
  - G-buffer albedo texture
  - G-buffer normal texture
  - optional depth texture for later extensions

#### `Renderer::destroy`
- Destroy all geometry/lighting resources mirroring existing cleanup order:
  - lighting descriptor sets/pool/layout
  - lighting pipeline + shader modules
  - geometry pipeline + shader modules

---

### 3) `src/renderer/renderer.cpp` (`Renderer::render`)
**Objective:** switch render graph flow from single forward pass to two passes.

#### A. Resource setup
- Import swapchain color/depth as before.
- Create transient frame-graph textures for:
  - `gbufferAlbedo`
  - `gbufferNormal`

#### B. Geometry pass (`GeometryPass`)
- Frame graph setup:
  - `write(gbufferAlbedo, ColorAttachment)`
  - `write(gbufferNormal, ColorAttachment)`
  - `write(depthHandle, DepthAttachment)`
- Execution:
  - begin rendering with two color attachments + depth
  - bind geometry pipeline
  - draw scene instances (same draw loop and push constants as today)

#### C. Lighting pass (`LightingPass`)
- Frame graph setup:
  - `read(gbufferAlbedo, ShaderRead)`
  - `read(gbufferNormal, ShaderRead)`
  - optional `read(depthHandle, ShaderRead)`
  - `write(colorHandle, ColorAttachment)`
- Execution:
  - begin rendering to swapchain color
  - bind lighting pipeline + descriptor set
  - draw fullscreen triangle

#### D. Overlay passes (unchanged ordering)
- Keep debug renderer pass and debug UI pass after lighting so overlays remain visible over final lit output.

---

### 4) `shaders/` (new files)
**Objective:** split geometry material output from lighting evaluation.

Add:
- `shaders/gbuffer.vert`
- `shaders/gbuffer.frag`
- `shaders/lighting.vert`
- `shaders/lighting.frag`

Responsibilities:
- `gbuffer.vert/frag`
  - output albedo and encoded normal to MRT
  - do not perform lighting
- `lighting.vert`
  - fullscreen triangle generation
- `lighting.frag`
  - sample G-buffer
  - compute ambient + directional Lambert diffuse

---

### 5) `Makefile`
**Objective:** ensure new shaders compile into SPIR-V.

- No rule changes expected; existing wildcard shader source discovery should include new `.vert`/`.frag` files automatically.
- Verify runtime shader paths passed to `createShaderModule(...)` match generated `*.spv` names.

---

### 6) Optional format/usability follow-ups

#### `src/rhi/rhitypes.h` (only if needed)
- If normal precision is insufficient, add/enable higher precision formats in RHI and backend (future enhancement).

#### Debug UI toggle (optional)
- Add visualization modes:
  - Lit
  - Albedo
  - Normal
- Useful for validating G-buffer correctness quickly.

---

## Milestones

### Milestone A — Bring-up
- Geometry pass writes G-buffer.
- Lighting pass outputs albedo-only (no shading yet).

### Milestone B — Basic lighting
- Lighting pass adds directional diffuse + ambient.
- Remove inline lighting from old material shader path.

### Milestone C — Validation and polish
- Add G-buffer debug view toggle.
- Confirm overlay passes (debug lines/UI) render correctly.

---

## Validation checklist
- Frame graph shows expected pass dependencies (`GeometryPass` -> `LightingPass` -> overlays).
- G-buffer resources transition correctly between color-attachment and shader-read states.
- Final swapchain image transitions to `PresentSrc`.
- No resource leaks in init/destroy lifecycle.
- Scene appearance is stable under camera movement (normals/light direction behave consistently).

## Risks and mitigations
- **MRT format mismatch risk:** start with formats already supported in current RHI enum; upgrade later if precision artifacts appear.
- **Descriptor complexity risk:** keep lighting descriptors minimal (2 textures + optional constants).
- **Debug overlay regression risk:** preserve pass order and existing debug pass integration.

## Out-of-scope for this first iteration
- Shadows
- Multiple dynamic lights
- BRDF/PBR model
- Screen-space effects (SSAO, SSR, bloom)

