# Mip level view and sampler controls

**Status. Landed.**

## Current state

Material textures carry full mip chains and an anisotropic sampler since `docs/plan_texture_mipmaps.md`. Nothing shows which level a
pixel samples or what the sampler settings cost; the only check is a screenshot looking less noisy. The buffer views
(`GBufferView`, `lighting.frag sampleBuffer`) are the place for a per-pixel diagnostic, and the Render Debug View tab already hosts the
view radio group and overlay toggles.

## Scope

**In**

- **Mip level view** (`view miplevel`, `GBufferView::MipLevel`). `gbuffer.frag` writes the LOD the sampler picked, `textureQueryLod`,
  into the albedo alpha (unused until now) as `lod / 16`; `lighting.frag` maps it through an eight-colour ramp, level 0 red, 1 orange,
  2 yellow, 3 green, 4 cyan, 5 blue, 6 magenta, 7 and up white, blended between levels.
- **Sampler controls**: max anisotropy, LOD bias, minimum LOD (forces coarser levels), nearest or linear mip filter. Live in the Render
  Debug View tab, `SamplerSettings` in the snapshot, verb `sampler aniso=8,bias=0.5,minlod=2,mip=nearest`. The renderer keeps a separate
  material sampler; a settings change creates a new one, defers the old to the deletion queue, and rebuilds the geometry descriptor sets
  through the same path a geometry change uses.
- `RhiSamplerDesc::mipLodBias`, additive.

**Out**

- Coloured debug mip chain replacing the textures, texture inspector with per-level thumbnails. Trigger: a suspected upload or filter
  bug the LOD view cannot explain.

## Decisions

- **LOD through the albedo alpha, not a third target.** Alpha was written as 1.0 and read nowhere. 8 bits over 16 levels is 16 steps per
  level, enough for a ramp. Rejected: a dedicated debug target, which costs bandwidth in every frame for a view mode.
- **Sampler change rebuilds descriptor sets.** Sets can be bound by frames in flight, so updating them in place is not allowed without
  update-after-bind; the free-and-rebuild path already exists for geometry changes and runs once per change.

## Verification

- Sponza courtyard, `view miplevel` screenshot: near floor red or orange, far walls green to blue, the ramp following distance. With
  `sampler minlod=4` the whole frame is cyan or coarser; with `sampler aniso=0` grazing floors shift one level coarser. Verified.
- three_cubes: `view miplevel` is red everywhere, 1x1 base-colour texels have only level 0. Verified.
- Sponza with `sampler minlod=4`: cyan everywhere except the window grilles, whose small textures have no level 4. With `sampler aniso=0`
  about one level coarser overall and blue to magenta on grazing surfaces. Verified by screenshot.
- Validation clean, example sweep unchanged.

## Deferred / follow-ups

- Coloured mip chain and texture inspector, above.
- Per-material sampler state from USD, listed in `docs/plan_texture_mipmaps.md`.
