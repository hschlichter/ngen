# Sky: the dome light drawn and lighting the scene

**Status. Landed.** Built 2026-11-06. Decisions locked 2026-11-05 (Henrik): EXR read through a vendored tinyexr; the first plan covers the visible sky and its diffuse
light, with specular and tone mapping deferred. 2026-11-06 (Henrik): the sky on the GPU is RGB9E5 at full size, 4096×2048 in 32 MiB (every
Vulkan 1.3 device samples it; RGBA16F would take 64 MiB, a 2048×1024 sky 8 MiB with visibly softer clouds).

## Current state

The streaming test world authors its sky (`assets/streamtest-world/world/layers/environment.usda`, a sublayer of `root.usda`): a
`DomeLight_1 "Sky"` with `inputs:texture:file = @../textures/sky.exr@`, `texture:format = latlong`, `poleAxis = Z`, `intensity = 0.9`, beside the
`DistantLight "Sun"`. `sky.exr` is a 4096×2048 half-float lat-long image of linear radiance: a Preetham sky with clouds, a ground hemisphere, and a
sun disc at the DistantLight's direction carrying 1% of its irradiance, so the dome can light the scene without counting the sun twice (the
generator's `docs/DESIGN.md`, section 7). It averages 0.43 over the sky hemisphere before the intensity; around the sun it reaches about 4× the
zenith, the disc about 650.

ngen-view draws none of it; the background is the lighting pass's clear colour, (0.12, 0.12, 0.15). Where it is lost:

- **Packing**: `pack.cpp`'s texture rule takes `.png`, `.jpg`, `.jpeg`; `ngen-packer-texture` decodes with stb into RGBA8 sRGB
  (`PackedTextureFormat::Rgba8Srgb`, the only format). Nothing reads EXR: stb doesn't, and the minimal OpenUSD build has no OpenEXR.
- **USD packer**: keeps every `inputs:*` (the texture's path becomes an asset id, `usdpacker.cpp:178`), but drops `poleAxis`.
- **World**: the composer knows `DomeLight` and `DomeLight_1` (`LightShape::Dome`); `Light` holds colour, intensity, exposure, angle and shadow,
  no texture, format or pole axis.
- **Placement**: only `LightShape::Distant` becomes a `RenderLight` (`placement.cpp:1243`).
- **Renderer**: `lighting.frag` shades Lambert, `albedo · lightColor · (ambient + diffuse · shadow)`, its ambient a fixed hemisphere
  (`AmbientSettings`, `render.ambient.*`) as a fraction of the sun's radiance. Textures are RGBA8 sRGB in one 1,024-slot array
  ([ADR 28](adr/0028_bindless_sampler_array.md)). No tone mapping: the lit image is clamped when blitted to the sRGB backbuffer.

## Scope

**In**
- Reading the dome as authored: an EXR texture packer path, the dome's texture, format and pole axis in the World, the dome placed in the
  `RenderWorld` as its environment, its texture streamed and uploaded as other textures are.
- The sky drawn where nothing is, in the lit image.
- The dome's diffuse light: its irradiance (9 spherical-harmonic coefficients per colour, computed when the texture is packed) replacing the
  hemisphere ambient while the scene has a dome.
- Runtime access and visibility for the environment.

**Out**
- Tone mapping and exposure: the sun's surroundings clip to white, as they do in Storm.
- Specular reflections from the dome: shading has no specular term yet.
- Dome formats other than `latlong` (angular, mirrored ball, cube maps) and more than one dome: warned and left out.
- The sky in the G-buffer and debug views: they keep the clear colour.

## Steps

1. **tinyexr** as a submodule (`external/tinyexr`, with the miniz it ships), compiled into `ngen-packer-texture` only.
2. **The texture packer reads EXR.** `pack.cpp`'s texture rule takes `.exr`. `ngen-packer-texture` decodes it with tinyexr to linear RGB float and
   writes `PackedTextureFormat::Rgb9e5` (new, `version 2`): one level (no mip chain; the sky is magnified on screen), 4 bytes a texel. Packed with
   it, the irradiance of the image read as a lat-long dome: 9 SH coefficients per channel (27 floats), from the radiance weighted by each texel's
   solid angle, in the image's own frame (pole up). `packedtexture.h` gains the format and an optional irradiance block after the header.
3. **The USD packer keeps `poleAxis`.** Added to the kept attribute names.
4. **The World reads the dome.** `Light` gains `texture` (asset id), `textureFormat` and `poleAxis`; the composer reads them for `LightShape::Dome`.
   A format other than `latlong`, or a second dome, is warned (`DomeLightIgnored`).
5. **Placement makes the environment.** The first drawn dome becomes `RenderWorld::environment`:
   ```cpp
   struct RenderEnvironment {
       TextureHandle texture;              // RGB9E5 lat-long; invalid while it loads
       glm::vec3 radiance = glm::vec3(1.0f); // color × intensity × 2^exposure
       glm::mat4 worldTransform = glm::mat4(1.0f); // the dome's, with the pole axis applied
       std::array<glm::vec3, 9> irradiance;  // SH, from the pack, in the dome's frame
       PrimHandle prim;
   };
   ```
   `ViewScene` wants the texture asset like a material's (`wantAsset`, `AssetKind::Texture`). `TextureDesc` gains a format, so the
   `TextureLibrary` carries it to the renderer.
6. **The renderer draws it.** The environment texture is not one of the material array's slots: it gets its own binding in the lighting pass's
   set, with a linear, clamp-to-edge sampler, and a fallback while it loads. `lighting.frag`, in the lit view: where depth is 0 the pixel is the
   dome's radiance in the view ray's direction (through the inverse view-projection and the dome's frame); elsewhere the ambient term is the SH
   irradiance at the normal × the dome's radiance ÷ π (Lambert), in place of the hemisphere. Without a dome everything is as today.
7. **Visibility** (below), then the READMEs: `src/renderer/README.md` (the environment, the lighting), `src/world/README.md` (the dome),
   `src/asset/README.md` (EXR, RGB9E5, the irradiance block), `src/view/README.md` (placement).

## Visibility

- **Records**: an `environment` record (the dome's prim, texture asset, its size and format and whether it is resident, colour, intensity,
  exposure, pole axis, the irradiance's 9 coefficients and its value straight up and straight down) beside `scene`; the `assets` record lists
  `sky.exr` as any packed asset.
- **Methods and verbs**: a configuration value `render.sky.on` (on by default), so the hemisphere ambient and the clear background can be
  compared against the dome; set with `config` like the others. No new method.
- **Windows**: the Render Debug window's lighting section shows the dome next to the sun (texture, size, radiance, irradiance up and down),
  and the texture inspector can show the sky (a preview converted from RGB9E5). The Configuration window shows `render.sky.on`.
- **Trace**: `DomeLightIgnored` (a format that isn't lat-long, or a second dome), `EnvironmentChanged` when the scene's dome is placed or
  removed.

## Verification

- The 8 reference images (three_cubes and Sponza: no dome) byte-identical, with validation on.
- The packer: `sky.exr` packs to a 4096×2048 RGB9E5 texture of 32 MiB plus its irradiance; the irradiance's constant term, as radiance, within
  1% of the image's solid-angle-weighted mean (the dome's DESIGN.md gives 0.43 over the sky hemisphere).
- At `/cameras/village`: the `environment` record shows the dome resident, intensity 0.9, pole axis Z; a screenshot shows the sky above the
  horizon, and with a camera turned to the sun's direction the brightest pixels are where that direction projects.
- With `render.sky.on false` the village image matches today's.
- The `memory` record shows the sky texture; no warnings or errors in the trace.

## Results

Built 2026-11-06, every step as planned.
- tinyexr v3.3.0 as `external/tinyexr`, through stb's zlib (`TINYEXR_USE_STB_ZLIB`): stb is already vendored, and miniz would be the build's
  only C source. The sky is ZIP-compressed, which stb's zlib reads.
- The EXR path was first in `ngen-packer-texture`, then split into its own program the same day (Henrik): `ngen-packer-exr`, rule `exr`
  (version 1). Its dependency stays in its own process, and since the cache hashes a packer's binary, a change to EXR packing repacks only EXR
  images (ADR 38). The split packer writes the sky's pack byte-identical to the merged one's.
- The texture pack is version 2: a 28-byte header with an irradiance count, the 27-float block, then the levels (`PackedTextureFormat::Rgb9e5`,
  `encodeRgb9e5`/`decodeRgb9e5` beside it). The texture rule's version went to 2 with the header; the `exr` rule takes `.exr`; the USD rule keeps `poleAxis` (version 10).
- `sky.exr` packs in half a second to 32 MiB plus the block. The irradiance's constant term gives a mean radiance over the sphere of
  (0.2161, 0.2738, 0.3554), against (0.2158, 0.2736, 0.3552) averaged from the packed texels by solid angle: within 0.15%.
- `RhiFormat::E5B9G9R9_UFLOAT`; `TextureDesc` carries its format and irradiance; the GPU scene uploads and slots it as any texture (one
  level, within the 64 MiB upload budget).
- The World's `Light` holds a dome's texture, format and pole axis; placement keeps domes by prim (`domesByPrim`) and makes the environment in
  `collectEnvironment`; `EnvironmentChanged` and `DomeLightIgnored` trace.
- The lighting pass binds the sky at binding 6 with its own sampler; the shader draws it in the lit view's empty pixels and replaces the
  hemisphere ambient with the irradiance over π. A scene without a dome takes the old expression unchanged.
- Visibility: the `environment` record; `render.sky.on`; `skyDrawn` in `SceneGpuStats`; the Render Debug window and the `render` record show
  the sky and its mean radiance; the inspector previews an HDR texture as half float, and `dump-texture` writes it as sRGB.
- Checks: the 8 reference images byte-identical. At the village the `environment` record shows `/Environment/Sky`, image held (4096×2048),
  drawn, intensity 0.9, pole Z, light facing up (0.25, 0.34, 0.56) and down (0.11, 0.13, 0.09); turned to the sun's direction, the brightest
  pixels' centroid is (1278.8, 718.7) at the screen's centre (1279.5, 719.5); with `render.sky.on false` the background is the clear colour
  again and `drawn` false; the `memory` record lists the sky as a 33,554,432-byte `E5B9G9R9_UFLOAT` texture; the inspector and `dump-texture`
  run on it under validation; the debug build runs with the Render Debug window open.
- Not checked as planned: "the village image with `render.sky.on false` matches today's". The binary before this plan can't read the
  repacked textures (pack version 2) the server now serves; the no-sky shader path is the one the 8 reference images check unchanged.
- Found on the way, not changed: headless runs get the surface's first format, `R8G8B8A8_UNORM` (`rhiswapchainvulkan.cpp:53` takes
  `formats[0]`), so every headless image, the reference images among them, is written without the sRGB encoding a window's
  `B8G8R8A8_SRGB` swapchain gives: the sky and everything else look darker headless than in a window.
- Shadowed faces at the village are darker than with the hemisphere ambient: the sky's authored irradiance (about 0.25 to 0.56 facing up) is
  below the configured ambient's 0.2 of the sun's radiance.

## Open questions

None. The decisions are recorded in [ADR 79](adr/0079_dome_light_sky_and_irradiance.md).

## Deferred / follow-ups

- **Tone mapping and exposure.** Trigger: the clipped sun area matters, or a scene authors brighter light than the sun's 3.2.
- **Specular from the dome** (prefiltered levels). Trigger: a specular term in shading.
- **The sky in the debug views.** Trigger: a view that needs it.
