# Plan: Support Lights from the USD Scene

Minimal, incremental plan to render the lights authored in a USD stage. The aim is the smallest change that turns
"only one directional sun is shaded" into "the scene's UsdLux lights actually light the scene" — multiple lights, plus
local (point) lights, evaluated analytically in the existing deferred lighting pass. Everything beyond that (area-light
shape, extra shadow maps, IBL) is deferred.

## Current state

The pipeline already does more than half the work:

- **Extraction** (`src/scene/usdscene.cpp`, `extractLight`): every UsdLux kind is recognised and stored in a `LightDesc`
  (`LightKind`, color, intensity, exposure, distant `angle`, shadow enable/color). This already covers Distant / Sphere
  / Rect / Disk / Cylinder / Dome. **No change needed to read the lights.**
- **RenderWorld extraction** (`src/scene/usdrenderextractor.cpp`): the bottleneck — it forwards **only** `LightKind::Distant`
  as a `RenderLight{type = Directional}` and drops everything else (see the `desc->kind == LightKind::Distant` guard).
- **Renderer** (`src/renderer/renderer.cpp`, `render()`): collapses `RenderWorld::lights` down to a **single** directional
  light — picks one directional, derives direction from its world transform's +Z column, computes
  `radiance = color * intensity * 2^exposure`, and builds one ortho shadow map for it.
- **Lighting** (`shaders/lighting.frag`, `LightingUBO` in `src/renderer/passes/lightingpass.h`): a single directional
  light — one `lightDirection` / `lightColor`, one shadow map, N·L diffuse + flat ambient, deferred (world pos
  reconstructed from the depth gbuffer).

So the shading model is "one sun". USD convention worth noting: lights emit along local **-Z**, so "direction toward the
light" for a directional is +Z (`worldTransform[2]`), and a local light's position is the transform translation
(`worldTransform[3]`).

## Scope

**In:** multiple lights in one deferred pass; directional + point light types; point attenuation; keep the existing
single sun shadow map.

**Out (explicitly, for later):** area-light shape/soft falloff (Sphere/Rect/Disk treated as point), spot cones, dome/IBL
lighting, shadows for local lights, more than one shadow map, tiled/clustered light culling. These are noted at the end.

## Steps

### 1. Forward all lights to the RenderWorld

`usdrenderextractor.cpp`: drop the Distant-only filter and map `LightKind` → `RenderLight` type:

- `Distant` → `Directional`
- `Sphere`, `Disk`, `Rect`, `Cylinder` → `Point` (shape ignored for now — position only)
- `Dome` → skip (would be ambient/IBL; out of scope)

`RenderLight` already carries `worldTransform`, so no new fields are strictly required — position and direction are
derived on the renderer side. (Optional: read `inputs:radius` into `LightDesc`/`RenderLight` later for nicer falloff;
not needed for a first pass.)

### 2. GPU light array

Replace the single-light fields in `LightingUBO` with a small fixed array. Introduce a `MAX_LIGHTS` constant (start at
16) shared by C++ and GLSL (define in both; keep the struct std140-friendly — vec4s only):

```
struct GpuLight {
    vec4 posOrDir;   // xyz = world position (point) or direction-toward-light (directional); w = type (0 = dir, 1 = point)
    vec4 radiance;   // rgb = color * intensity * 2^exposure; a = range (0 = no cutoff), or reserved
};
```

`LightingUBO` gains `GpuLight lights[MAX_LIGHTS]` + `int lightCount`, and keeps `depthParams`, `invViewProj`,
`shadowTint`, `lightViewProj` (the shadow map stays tied to the primary directional).

### 3. Build the array in the renderer

In `renderer.cpp`, alongside the existing "pick the shadow-casting directional" logic (unchanged — it still drives the
shadow map), loop `RenderWorld::lights`, fill up to `MAX_LIGHTS` `GpuLight`s:

- directional: `posOrDir = normalize(worldTransform[2].xyz)`, `type = 0`
- point: `posOrDir = worldTransform[3].xyz`, `type = 1`
- `radiance = color * intensity * 2^exposure` (reuse the existing exposure math)

Record which light index (if any) is the shadow-casting directional so the shader only shadows that one.

### 4. Multi-light shading

`lighting.frag`: loop `0..lightCount`, accumulate diffuse:

- directional: `L = lights[i].posOrDir.xyz;` no attenuation.
- point: `d = lights[i].posOrDir.xyz - worldPos; L = normalize(d); atten = 1.0 / max(dot(d, d), eps);` (optional range
  cutoff via `radiance.a`).
- `diffuse += max(dot(N, L), 0) * radiance * atten * shadowFactorForThisLight`.

Only the primary directional samples the shadow map; all others use `shadowFactor = 1`. Keep the single flat ambient
term (added once). The gbuffer-preview / debug view modes stay as-is.

**Tuning note:** USD point-light intensity + physical inverse-square can be wildly bright or dim depending on scene
scale. Expect to add a simple intensity scale (or the `radius`-based normalization UsdLux implies) during bring-up;
don't block the first pass on getting units perfect.

### 5. Verify

The NewSponza *Main* file ships **no lights** (cameras only — Intel packages lighting separately), so verify with a
small authored scene: add a `UsdLuxSphereLight` (and keep the default distant) to `assets/three_cubes.usda` or a scratch
`.usda`, load it, and confirm via the gbuffer "lit" view that the point light produces a local falloff and multiple
lights sum. Headless: the observation bus already logs lights; assert `lightCount` and per-light type as a sanity check.

## Deferred / follow-ups

- Area-light shape and soft falloff (use `inputs:radius`, `width`/`height`, treat Rect/Disk properly).
- Spot lights (UsdLux `shaping:cone:angle` on Sphere lights) → `RenderLight::Spot` + cone attenuation (type already exists).
- Dome / environment lighting (IBL) and a real ambient term.
- Shadows for local lights (cube/spot shadow maps) and more than one shadow-casting light.
- Light culling (tiled/clustered) once counts grow beyond a handful.
