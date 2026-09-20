# Texture mipmaps at upload

**Status. Landed.**

## Current state

Every material texture is uploaded as one mip level (`Renderer::uploadRenderWorld`, `renderer.cpp` about line 441, `RhiTextureDesc
mipLevels` left at 1). Sponza has 28 textures at 4096x4096, 1.6 GB, and samples them minified everywhere: from inside the courtyard the
geometry pass costs 25.9 ms, and an earlier outside-view measurement (`notes.md`) put the no-mip penalty at 2.1 ms of a 7.9 ms pass; the
inside view has far more textured pixels, so the share is larger. Textures also shimmer when the camera moves.

The RHI already carries mip chains: `RhiTextureDesc::mipLevels`, `RhiBufferTextureCopy::mipLevel`, the Vulkan image view covers all
levels, `RhiSamplerDesc` defaults to linear mipmap filtering with `maxLod` unclamped, and the `mipcube` example uploads a three-level
chain through `copyBufferToTexture` per level. Only the engine's uploader stops at level 0.

Sibling plans: `docs/plan_frustum_culling.md`, `docs/plan_geometry_pass_cost.md`.

## Scope

**In**

- `GpuUploader::uploadTexture` generates the full chain down to 1x1 for RGBA8 textures: CPU 2x2 box filter per level, each level staged
  and copied with its `mipLevel`; `mipLevels` set on the texture desc.
- sRGB-correct downsampling: decode to linear, average, encode, since the formats in use are `R8G8B8A8_SRGB`.
- Anisotropic filtering on the material sampler, `maxAnisotropy = 8`, clamped by the backend to the device limit.
- Render Debug texture table shows the real mip count and bytes including the chain (`RenderDebugSnapshot::textures`).
- Obs: `TextureUploaded` gains `mips`.

**Out**

- GPU mip generation with `blitTexture` per level. The RHI blit has no mip-level parameters yet; adding them is RHI work for the same
  result. Trigger: upload time on the CPU shows up in the `Upload` zone for a scene that streams textures.
- Compressed formats (BC7). Trigger: an asset pipeline that produces them (`notes.md` item 11).
- Non-power-of-two edge handling beyond rounding down (odd sizes drop the last row or column at each level). Fine for photo textures.

## Decisions

- **CPU box filter, not GPU blit.** Zero RHI change; the per-level copy path exists and is tested by `mipcube`. 4096x4096 RGBA8 is 64 MB
  at level 0 and 21 MB for the rest of the chain; filtering it on the CPU is around 20 ms per texture on one core, 28 textures at load
  time. Acceptable for a load step that already takes seconds decoding PNGs. Runs inside the existing `UploadTexture` profile zone.
- **Linear-space averaging.** A box filter in sRGB space darkens mid-tones; decoding through a 256-entry table and encoding with the
  standard curve costs nothing measurable.
- **Anisotropy on by default.** The sampler is shared by every material; grazing floors in Sponza are exactly the case anisotropy exists
  for. Cost on an iGPU is a few percent of texture bandwidth, which mips just cut by far more.

## Steps

1. **Mip chain builder.** `src/renderer/mipchain.h/.cpp`:

   ```cpp
   struct MipLevel {
       uint32_t width = 0;
       uint32_t height = 0;
       std::vector<uint8_t> pixels; // RGBA8
   };

   // Full chain from level 0 down to 1x1. sRGB decode/encode when srgb is set.
   auto buildMipChain(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) -> std::vector<MipLevel>;
   ```

2. **Uploader.** `GpuUploader::uploadTexture(const RhiTextureDesc&, std::span<const std::byte>)` keeps its signature; when
   `desc.mipLevels > 1` the caller passes all levels concatenated, level 0 first, and the uploader computes offsets from the desc. One
   staging buffer per texture, one `copyBufferToTexture` per level with `bufferOffset` and `mipLevel`, barriers on the whole image as now.

3. **Renderer.** `uploadRenderWorld` builds the chain, sets `mipLevels = chain.size()`, concatenates into one staging span.
   `CachedTexture` gains `mipLevels` and `bytes`; the render debug snapshot reads them instead of computing `w * h * 4`.

4. **Sampler.** `textureSampler = device->createSampler({.maxAnisotropy = 8.0f})`.

5. **Obs.** `TextureUploaded` (or the existing upload event) gains `mips` and `bytes`.

## Verification

- Sponza headless at the courtyard camera: `GeometryPass` GPU ms below the pre-change 25.9 with culling off (measure with
  `cull off` from `docs/plan_frustum_culling.md` if that lands first, so the two gains are attributed separately). Render Debug dump shows
  every texture with `mips: 13` and `bytes` about 89 MB for 4096x4096. Verified: GeometryPass 26.0 to 10.5 ms with `cull off`, 28
  textures at 13 mips and 89 478 484 bytes. Chain build: 3.5 s for all 28 in the debug build, 0.7 s in release, after the lookup tables
  and row threads in `mipchain.cpp` (the first pow-per-texel version took 23 s).
- Screenshot from the courtyard camera: floor and far walls no longer sparkle; agent compares two PNGs of consecutive frames with the
  camera moving one step (`camera` verb at frames 10 and 11) and sees far surfaces stable.
- `mipcube` example unchanged (RHI untouched); example sweep passes. Verified.
- three_cubes: `RenderStats.textures` unchanged; its materials are 1x1 base-colour texels, `TextureUploaded.mips` is 1 for those.

## Deferred / follow-ups

- GPU mip generation through blits, with `RhiBlitRegion { mipLevel, extent }` on both sides. Trigger above.
- BC7 or ASTC through an offline step. Trigger above.
- Per-material sampler state from USD (`wrapS`, `wrapT` on `UsdUVTexture`). Trigger: an asset that visibly needs clamp.
