# Texture packer

**Status. Landed.**

Part of step 2 of [plan_tool_architecture.md](plan_tool_architecture.md), the first real packer after shaders. It builds on
[plan_asset_copy.md](plan_asset_copy.md): textures already come streamed from the asset server, but as PNG bytes that the view decodes and
mip-maps itself. A texture packer moves both out of the view: it decodes the image and builds the mip chain once, when the asset is packed, and the
view uploads what arrives.

## Current state

- **Textures are PNGs packed by copy.** `pack.cpp`'s copy rule covers `.png .jpg .jpeg .hdr`. `loadTextureFromResolvedPath` (`src/scene/usdscene.cpp`)
  opens the texture through the resolver and decodes it with `stbi_load_from_memory` to RGBA8 in `MaterialDesc::texPixels`.
- **Only base colour textures are used**, one per material, as `R8G8B8A8_SRGB`. Materials without one get a generated 1×1 texture.
- **Mips are built in the renderer at upload**: `buildMipChain` (`src/renderer/mipchain.*`), a 2×2 box filter averaging colour in linear space,
  then `packMipChain` and `GpuUploader::uploadTexture` (`Renderer::uploadRenderWorld`).
- **The RHI has no compressed formats.** `RhiFormat` stops at 8-bit, float and depth formats; BC formats need Vulkan's `textureCompressionBC`, an
  optional feature.
- **Where Sponza's load goes** (debug, 60 frames, 34.9 s, `--dump-profile`): `UploadTexture` 4.7 s for 28 textures, of which `BuildMips` 4.0 s.
  Scene loading has no profile zones; from the total, the layer parse (about 3 s in the resolver spike) and the frames, PNG decoding is an estimated
  20–25 s.

## Scope

**In**

- **`ngen-packer-texture`**: decodes an image with stb_image, builds the full mip chain with the renderer's filter, and writes a packed texture.
- **A packed texture format** (`src/asset/pack/packedtexture.h`): a small header and the mip levels, read by the engine and written by the packer.
- **The texture rule** in `pack.cpp` for `.png`, `.jpg` and `.jpeg`, taking them from the copy rule. `.hdr` stays on the copy rule; nothing uses it.
- **The scene reads packed textures**: `loadTextureFromResolvedPath` parses the packed texture instead of decoding an image.
- **The renderer uploads the mips it is given**, and builds them only for textures that arrive without, such as the generated 1×1 ones.
- **Profile zones** around the scene's texture loading, so the gain is measured, not estimated.

**Out**

- **Block compression (BC7 and friends)**: needs the optional `textureCompressionBC` feature, a vendored encoder, and new RHI formats. See Decision 1
  and Deferred.
- **Linear (non-colour) textures**, such as normal and roughness maps: nothing uses them yet. See Open questions.
- **Requesting a scene's textures in one batch** ([plan_asset_copy.md](plan_asset_copy.md), Deferred).
- **Textures streamed by mip level** (lowest mips first). The packed format keeps levels separate, so it can come later.

## Decisions

Proposed; pushback welcome.

1. **The packed format is RGBA8 sRGB with the full mip chain, uncompressed.** I lean this for the first step:
   - no new RHI format and no optional Vulkan feature
   - the packer uses the renderer's mip filter, so the images stay byte-identical, which makes the verification exact
   - the cost is size: a 4096² texture with mips is 89 MB against 10–20 MB as PNG, so Sponza's cache and stream grow from about 0.4 GB to
     2.2 GB. The view already holds the same 2.2 GB today, decoded, so its memory doesn't grow.

   The alternative is BC7 now: four times smaller than RGBA8 and the format a shipping build wants, but it needs `textureCompressionBC`
   (optional; per the Vulkan baseline, asked about before use), an encoder vendored as a submodule (`bc7enc_rdo` or similar), packing times of
   minutes for Sponza on first pack, and screenshots that no longer match byte for byte. It is the natural next step once this one lands (Deferred).
2. **The mip filter moves to the asset system.** `buildMipChain` and `packMipChain` move from `src/renderer/mipchain.*` to
   `src/asset/pack/mipchain.*` (the `packer` library), used by the texture packer, and by the renderer for the textures it builds mips for. The
   packer must not link the renderer, and the asset system sits below it.
3. **The packed texture file:**

   ```cpp
   // A packed texture: a header, then every mip level tightly packed, level 0 first. Little-endian.
   struct PackedTextureHeader {
       char magic[4];        // "NGTX"
       uint32_t version;     // 1
       uint32_t format;      // PackedTextureFormat: 1 = RGBA8 sRGB
       uint32_t width;
       uint32_t height;
       uint32_t mipLevels;   // full chain: floor(log2(max(width, height))) + 1
   };

   // Parses a packed texture; nullopt when the header is invalid or the sizes don't add up.
   auto readPackedTexture(std::span<const std::byte> bytes) -> std::optional<PackedTextureView>;
   ```

   Levels are found by size, not by an offset table: each is `max(1, width >> level) × max(1, height >> level) × 4` bytes, the layout
   `GpuUploader::uploadTexture` already takes. The header is the only new format the engine reads.
4. **The view keeps the bytes as they arrive.** `MaterialDesc` gains the packed mip block and its level count; `texPixels` stays for generated
   textures. The renderer uploads a material's mip block directly when it has one, and runs `buildMipChain` only when it doesn't.
5. **The packer is `ngen-packer-texture`** (`src/apps/packertexture.cpp`) on `packer.h`, linking stb_image from `external/stb`. Its depfile lists
   only the source. The rule's version is the packed format's version, so a format change repacks every texture.

## Steps

1. **Move the mip filter** to `src/asset/pack/mipchain.h/.cpp` in the `packer` library; the renderer links `packer` for it and includes the new
   header. No behaviour change.
2. **The packed format** (`src/asset/pack/packedtexture.h/.cpp`): the header, `writePackedTexture` for the packer, `readPackedTexture` for the
   engine.
3. **`ngen-packer-texture`**: decode with `stbi_load` to RGBA8, `buildMipChain(…, srgb = true)`, `packMipChain`, write the header and the block to
   `--out`; a program target in `build.cpp`, which `ngen-asset-server` depends on.
4. **The rule** in `pack.cpp`: `texture` for `.png .jpg .jpeg`, `ngen-packer-texture`, version 1; the copy rule keeps `.usda .usdc .usd .hdr`.
5. **The scene** (`src/scene/usdscene.cpp`, `src/scene/material.h`): `loadTextureFromResolvedPath` reads the packed texture into the
   `MaterialDesc`; a `LoadTexture` profile zone around it, and one around the whole material pass.
6. **The renderer** (`Renderer::uploadRenderWorld`): upload the packed block when the material has one; build mips only when it doesn't. The
   `TextureUploaded` event keeps its fields.
7. **Docs:** `src/asset/README.md` (the texture packer and its format), `src/renderer/README.md` (mips come packed), `src/scene/README.md`
   (textures arrive packed).

## Verification

- **The same pictures:** the six headless screenshots are byte-identical to the baseline.
- **The same textures:** Sponza's 28 `TextureUploaded` events have the same fields as before (4096², 13 mips, 89,478,484 bytes each, and the small
  ones).
- **Nothing decoded in the view:** with a warm cache, the view's profile has no `BuildMips` zones for Sponza's textures, and `stbi_load_from_memory`
  is not called (a `LoadTexture` zone each, well under a millisecond per MB).
- **Faster:** Sponza's warm load (60 frames, debug) is measured before and after and recorded in Results; the target is under 15 s from about 35 s.
- **The packer matches the renderer:** for a few of Sponza's textures, each level of the packed file is byte-identical to `buildMipChain` run on the
  stb-decoded PNG.
- **Repacking:** editing a PNG (a copy of one inside `assets/`) repacks only that texture on the next request; the server's trace shows the
  `packing with ngen-packer-texture` line and the time.
- **Odd sizes:** a 1×1, a 3×5 and a 1000×600 PNG pack to the level counts and sizes `mipLevelCount` and `buildMipChain` give, and upload without
  validation errors.

## Deferred / follow-ups

- **BC7 (and BC5 for normal maps).** Trigger: this lands; it needs `textureCompressionBC` approved, an encoder vendored, and RHI formats.
- **Mip-level streaming**: lowest levels first, so a texture appears blurry and sharpens. Trigger: load time is dominated by texture bytes.
- **Packing on more cores**: the packer is single-threaded per texture; the server already packs textures in parallel. Trigger: cold packing time
  matters.

## Open questions

- **Linear textures.** A packer rule sees only the file, not how a material uses it, so it can't tell a normal map from a base colour. When
  materials use non-colour textures, the request has to carry the intent: a separate id per use (for example `brick_n.png#linear`), or a rule
  parameter per path pattern. It doesn't matter until such a texture is used.

## Results

- **The same pictures:** the six headless screenshots are byte-identical to the baseline.
- **The same textures:** Sponza's 28 `TextureUploaded` events have the same fields as before the packer, cold and warm (25 packed at 4096², 13 mips,
  89,478,484 bytes; 3 generated 1×1).
- **The packer matches the renderer:** for `arch_stone_wall_01_BaseColor`, `floor_tiles_01_BaseColor` and `dirt_decal_01`, every level of the
  packed file is byte-identical to `buildMipChain` run on the stb-decoded PNG.
- **Nothing decoded in the view:** the scene no longer includes stb_image; `BuildMips` runs only for the 3 generated 1×1 textures (0.00 s).
- **Faster** (Sponza, 60 frames, debug, cache warm): 14.0 s, from 34.9 s. `UpdateAssetBindings` (the scene's material pass, newly profiled)
  takes 5.4 s, of which `LoadTexture` 2.8 s for 25 textures; `UploadTexture` 1.8 s, from 4.7 s.
- **Cold** (empty cache): 46.2 s, with `LoadTexture` 31.9 s. The scene requests its textures one at a time, so each waits for its own packing
  (about 1.2 s per 4096² texture in debug). The server would pack them in parallel if they were requested together.
- **Odd sizes:** 1×1, 3×5 and 1000×600 PNGs pack and upload with 1, 3 and 10 levels (4, 72 and 3,199,588 bytes), validation clean.
- **Repacking:** after editing the 1000×600 PNG, the next load repacked only it (50 ms); the scene and the other two were up to date.
- All three configurations and the examples build.

Deviations from the plan:

- **`LoadTexture` is about 1.3 ms per MB, not "well under a millisecond".** That criterion was wrong: the zone includes fetching the bytes from
  the asset server (about 2.1 GB, roughly 0.75 GB/s through the stream), not only reading the header. What matters held: nothing is decoded and no
  mips are built.
- **The packed levels live in `MaterialDesc::texPixels`** with a new `texMipLevels` (0 for the generated textures), instead of a separate block.
- **ngen-view links `packer` directly**: the build doesn't pass static libraries on transitively.

Follow-up made more pressing by the cold number: requesting a scene's textures in one batch ([plan_asset_copy.md](plan_asset_copy.md),
Deferred), so cold packing runs in parallel.
