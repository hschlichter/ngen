# 79. The dome light as the sky, its irradiance computed when packed

Date: 2026-11-05

## Status

Accepted

## Context

The streaming test world authors its sky as UsdLux does: a `DomeLight_1` with a 4096×2048 half-float lat-long OpenEXR image, built so it can
light the scene without counting the sun twice (its sun disc carries 1% of the `DistantLight`'s energy). ngen-view drew none of it: nothing
read EXR, the World kept no dome's image, placement dropped every light but distant ones, and the lighting pass lit faces out of the sun with a
fixed hemisphere ambient as a fraction of the sun's radiance. Shading is Lambert with no specular term, and nothing tone-maps.

## Decision

Locked by Henrik on 2026-11-05 and 2026-11-06:

- OpenEXR is read through a vendored tinyexr, in a packer of its own (`ngen-packer-exr`, the only program that links it, so EXR packing
  changes repack only EXR images); the engine reads the dome's image as authored.
- The first step is the visible sky and its diffuse light. The sky is drawn where nothing is in the lit view; its irradiance replaces the
  hemisphere ambient while a scene has a dome.
- The irradiance is integrated once, when the image is packed: 9 spherical-harmonic coefficients per channel, convolved with the cosine lobe,
  in the image's frame (pole +Z), packed beside the image. The view rotates directions into that frame; the shader evaluates them at the normal.
- On the GPU the sky is `E5B9G9R9_UFLOAT` at full size, one level: 4096×2048 in 32 MiB, sampled by every Vulkan 1.3 device.
- The sky is its own binding in the lighting pass, with a sampler that wraps around the longitude and clamps at the poles.
- One sky: the first dome light in prim order; a second, a format other than lat-long, or an image that isn't HDR is traced and left out.

Rejected: authoring the sky as Radiance `.hdr`, which stb already reads (the engine couldn't read the EXR domes other USD scenes use);
RGBA16F (64 MiB) and a 2048×1024 sky (8 MiB, visibly softer clouds); integrating the irradiance in the view at load (the view would decode and
integrate a 32 MiB image on its scene thread; packing makes assets engine-ready once). Deferred: tone mapping, specular reflections from the
dome, the sky in the debug views.

## Consequences

- Easier: a scene authored with a dome looks and lights as authored, and a scene without one renders exactly as before.
- Easier: the light a dome casts costs the shader nine terms, and its numbers are in a record (`environment`) before anything is drawn.
- Harder: without tone mapping the sun's surroundings clip to white, and shadowed faces are as dark as the authored sky makes them.
- Harder: the texture pack's format changed (version 2): every cached texture repacks once.

## Verification

- Sources: [plan_sky.md](../plan_sky.md), `src/asset/README.md` (the EXR packer), `src/view/README.md` ("The sky"), `src/renderer/README.md`
  ("The sky").
- Code: `src/apps/packerexr.cpp` (`packExr`, `latLongIrradiance`); `src/asset/pack/packedtexture.h` (the format and its conventions);
  `src/view/placement.cpp` (`collectEnvironment`); `src/renderer/renderworld.h` (`RenderEnvironment`); `shaders/lighting.frag`
  (`skyRadianceToward`, `skyIrradianceToward`).
- Differs from plan: the plan put EXR in `ngen-packer-texture` (step 2); it is its own packer, `ngen-packer-exr`, with its own rule.
