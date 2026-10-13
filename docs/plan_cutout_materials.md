# Cutout materials

**Status. Landed.** 2026-10-13.

Materials whose opacity is cut against a threshold (UsdPreviewSurface `opacity` with `opacityThreshold > 0`) draw only where the opacity
reaches it. Today the renderer reads no opacity: a cutout surface draws as a solid quad, colour, depth and instance id included.

## Current state

- **Sponza's decals draw opaque.** `decals_1st_floor`, `decals_2nd_floor` and `decals_3rd_floor` (one submesh each) are bound to
  `/root/mtl/dirt_decal_usd`, whose `opacity` is connected to `Map__54`, a `UsdUVTexture` reading `textures\dirt_decal_01_mask_usd.png` with
  `sourceColorSpace = "raw"` (`outputs:rgb`), and `opacityThreshold = 0.1`. Drawn opaque, the decal quads cover walls and columns: the dark
  smears on Sponza's walls are dirt quads, and a pick on a column returns `decals_2nd_floor` (found at the camera `3.293,-1.020,9.798,-176.85,-3.95`:
  moving the decals away with `translate` makes the same pixels pick `column_2ndfloor_12` and `column_2ndfloor_01`). The USD view drew them
  the same way; the scene pack baselines include it.
- **Sponza's other 27 materials** author `opacity = 1`, `opacityThreshold = 0`: opaque.
- **The material pack** (`src/asset/pack/scenepacktypes.h`, `MaterialPack`) holds the base colour (texture or constant) only;
  `packMaterial` (`src/asset/pack/usd/usdpacker.cpp`) reads `diffuseColor`.
- **Textures are packed sRGB** (`ngen-packer-texture`: RGBA8 sRGB with mips). A mask authored `raw` is decoded as colour when sampled; the
  roadmap's "Linear (non-colour) textures" item is that gap.
- **The renderer** (`src/renderer/README.md`): `GpuMaterial` is `{baseColorTexture, pad[3]}`; one texture per material in the texture cache
  (keyed by material index); draw lists have two buckets per view, single- and double-sided (`DrawLists::bucketCount`), one pipeline each;
  the shadow pass draws whole meshes (on each prim's first submesh) from the position-only stream; the depth prepass is position-only too;
  `gbuffer.frag` writes albedo, normal and the instance id that picking reads.

## Scope

**In**
- Cutout opacity from UsdPreviewSurface: `opacity` as a constant or connected to a `UsdUVTexture` channel (`outputs:r`, `g`, `b`, `a`, or
  `rgb`, read as its first channel), with `opacityThreshold > 0`.
- Packing it: the material pack carries the opacity source and threshold; the opacity texture is an asset of its own.
- Drawing it in every pass that writes depth or ids: the geometry pass, the depth prepass, the shadow pass, the debug views; picking then
  goes through the cut-away parts.
- The Render Debug window and `render` record show which materials and draws are cutout.

**Out**
- Translucency (`opacity < 1` with `opacityThreshold = 0`: blended surfaces, ordering). A material like that stays opaque, as today, with a
  packer warning naming it.
- Alpha-to-coverage, MSAA, and opacity mip filtering that preserves coverage (a cutout thins out at small mips). See Deferred.
- Linear textures in general (normal and roughness maps): only what the mask needs, per the decision below.

## Decisions

Locked 2026-10-13, all four as proposed.

1. **Cutout draws in buckets of their own.** A bucket becomes (cull mode, cutout): four per view instead of two, each with its pipeline. The
   cutout pipelines sample the opacity and `discard` below the threshold; the opaque ones stay as they are, keeping early depth testing for
   everything that isn't cutout. Rejected: one pipeline with a per-material branch that discards, which turns early depth off for every draw
   of the pipeline (`discard` anywhere in the shader). I lean buckets: Sponza's decals are three draws, the walls are hundreds.
2. **The mask's colour space: undo the sRGB decode in the shader** for opacity read from a texture authored `raw`: the material says the
   texture is raw, and the shader re-encodes the sampled value (linear to sRGB) before comparing it with the threshold, which gives back the
   stored value to within a step of 8-bit precision. Rejected for now: packing masks as linear textures, which is the roadmap's "Linear
   textures" item (the request carrying intent, a second texture format) and is worth doing when normal or roughness maps arrive. I lean
   re-encoding; pushback welcome if you'd rather do linear textures now.
3. **Cutouts cast cut shadows.** The shadow pass draws cutout submeshes one by one, with texture coordinates, through a cutout shadow
   pipeline that discards; a prim with any cutout submesh draws all its shadows per submesh, so its whole-mesh draw doesn't cast the cutout
   part as solid. The depth prepass does the same for its cutout bucket. Rejected: leaving cutouts out of shadows (decals cast none, which is
   right for decals but wrong for foliage and fences).
4. **The opacity texture is its own texture slot.** `GpuMaterial` gains the opacity texture slot, its channel and the threshold (the three
   pad words); the texture cache holds the base colour and opacity textures of a material. A material whose opacity is the base colour's
   alpha (`outputs:a` of the same file) uses one texture for both.

## Steps

1. **Pack** (`scenepacktypes.h`, `scenepack.cpp`, `usd/usdpacker.cpp`): `MaterialPack` gains

   ```cpp
   struct MaterialPack {
       bool hasBaseColor = false;
       std::array<float, 4> baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
       std::string baseColorTexture;
       // Cutout: drawn only where opacity reaches the threshold. opacityThreshold 0 is opaque.
       float opacity = 1.0f;          // the constant, when no texture
       std::string opacityTexture;    // a texture asset id, empty for none
       uint32_t opacityChannel = 0;   // 0 r, 1 g, 2 b, 3 a
       bool opacityRaw = false;       // the texture is authored raw: undo the sRGB decode
       float opacityThreshold = 0.0f;
   };
   ```

   `packMaterial` follows the `opacity` connection through node graphs as it does `diffuseColor`, reads the channel from the output name,
   `sourceColorSpace`, and `opacityThreshold`. A material with `opacity < 1` (or connected) and threshold 0 gets a warning (translucency isn't
   supported). The format version and the `usd` rule's version bump. `ngen-packdump` prints the new fields.
2. **The view** (`src/view/viewscene.*`): fetches opacity textures with the base colour ones; `MaterialDesc` (`src/scene/material.h`) gains the
   opacity texture's pixels, channel, raw flag and threshold; `drawsOf` counts the opacity texture as used by the material's draws.
3. **GPU tables** (`gpuscene.*`, `renderer.cpp`): `GpuMaterial` gains `opacityTexture`, `opacityChannel`, `opacityThreshold` (float bits),
   and a raw flag; the texture cache uploads opacity textures; `GpuInstance` gains a cutout flag (its material is cutout) and a
   prim-has-cutout flag for the shadow rule.
4. **Draw lists and culling** (`drawlists.*`, `instancecull.comp`): `bucketCount` becomes 4, the bucket from the cull mode and the cutout flag;
   the shadow views draw a prim with a cutout submesh per submesh instead of on its first submesh.
5. **Pipelines** (registered through the pipeline registry): the geometry pass, the depth prepass and the shadow pass each gain cutout
   variants; `gbuffer.frag` and a cutout fragment shader for the depth-only passes (`cutout.frag`), with the vertex shaders passing texture
   coordinates where the cutout variants need them (the shadow and prepass cutout pipelines read the full vertex stream, not the position-only
   one); the debug views discard as the geometry pass does.
6. **Debug and docs**: the Render Debug window and `render` record show the cutout materials and the per-bucket draws; `src/renderer/README.md`
   (buckets, the cutout rule, shadows), `src/asset/README.md` (the material pack's fields).

## Verification

- **Picking through decals**: at `3.293,-1.020,9.798,-176.85,-3.95` in Sponza, `pick 499 1216` and `pick 678 1024` select
  `/root/_nd_floor/column_2ndfloor_12` and `/root/_nd_floor/column_2ndfloor_01` with the decals in place.
- **Sponza looks right**: a screenshot at that camera shows the columns and walls, with dirt only inside the decals' masks; the instance-ID
  debug view shows the decals as fragments, not quads. The default-camera screenshot changes only where decals draw (a pixel diff against the
  current baseline, inspected); new baselines are captured.
- **Nothing else changes**: three_cubes is byte-identical to its baseline; Sponza's `render` record has the same meshes and textures plus
  the one mask texture (29), and three cutout draws in the cutout buckets.
- **Shadows**: the decals' shadows follow their masks (the shadow-factor view at a decal shows no solid quad).
- **The pack**: `ngen-packdump --id …#/root/mtl/dirt_decal_usd` prints the mask texture, channel 0, raw, threshold 0.1; a material with
  `opacity < 1` and threshold 0 warns.
- Validation clean; debug and release build.

### Results (2026-10-13)

- Picks at `3.293,-1.020,9.798,-176.85,-3.95`: `pick 499 1216` selects `column_2ndfloor_12`. `pick 678 1024` still selects `decals_2nd_floor`:
  that pixel is a dirt streak the mask keeps (57% of the mask is at or above 0.1), so the decal is rightly what is drawn there; `pick 635 1075`, a
  clean spot on the same column, selects `column_2ndfloor_01`.
- three_cubes lit, overdraw and wireframe are byte-identical to their baselines. Sponza at the default camera: 341 pixels differ by small amounts
  (cut shadows), overdraw 16883 and wireframe 264 pixels, at the decals. New baselines in `_out/cutout-baseline`.
- Sponza: 29 textures (the mask is material 22's opacity texture), 1 cutout material, 4 cutout instances; at the camera above the geometry pass
  draws 3 decal submeshes and the shadow pass 9 (per submesh, per cascade).
- Shadows follow the masks; decals, lying just off their walls, cast mask-shaped shadows onto them (a follow-up in the roadmap).
- No validation messages; debug and release build.

## Open questions

None left; the decisions are locked (2026-10-13).

## Deferred / follow-ups

- **Translucency** (blended surfaces, sorting, a forward pass). Trigger: a scene that needs glass or smoke.
- **Coverage-preserving mips for opacity** (cutouts thinning out with distance). Trigger: foliage that vanishes at a distance.
- **Linear textures** (the roadmap item): masks would then be packed linear and the shader re-encoding removed. Trigger: the first normal or
  roughness map.
