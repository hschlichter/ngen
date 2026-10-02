# A scene's textures in one request

**Status. Landed.**

Follow-up to [plan_asset_copy.md](plan_asset_copy.md) (Deferred) and [plan_texture_packer.md](plan_texture_packer.md) (Results): on an empty cache
Sponza takes 46 s, because the scene requests its textures one at a time and each waits for its own packing (about 1.2 s per 4096² texture in debug).

## Current state

- `USDScene::updateAssetBindings` extracts each renderable prim's mesh, and each bound material's base colour texture as it meets it
  (`extractMaterial` → `extractShaderTexture` → `loadTextureFromResolvedPath`). Every texture is one `OpenAsset` → one `asset.request` → wait.
- The asset server packs the ids of one request in parallel, on one worker per hardware thread.

## Scope

**In**

- **`prefetchAssets(ids)`** in the USD asset resolver: one `asset.request` for all of them; a later open of one of them waits for it instead of
  requesting it again.
- **A prefetch pass** at the start of `updateAssetBindings`: the base colour texture of every material bound to a renderable prim or one of its
  material-bind subsets, resolved the same way the extraction resolves it, in one prefetch.

**Out**

- Decoding or uploading in parallel in the view: the extraction stays serial; only the packing and streaming overlap.
- Prefetching layers (references and payloads); USD opens them during composition, before the scene code runs.

## Steps

1. **`usdassetresolver.h/.cpp`**: `prefetchAssets(std::span<const std::string> ids)`. Ids not already being fetched are sent in one request and
   remembered; `fetch` skips its own request for a remembered id and waits for it.
2. **`usdscene.cpp`**: `baseColorTextureFile(const UsdShadeMaterial&)`, the path from a material to its resolved texture id, shared by
   `extractMaterial` and the new `prefetchTextures` pass (profile zone `PrefetchTextures`).
3. **Docs**: `src/scene/README.md` (textures are requested in one batch).

## Verification

- **One request:** opening Sponza, the server's trace shows one request for the 25 textures instead of 25.
- **Cold load:** Sponza on an empty cache (60 frames, debug) is measured before (46.2 s) and after and recorded in Results; the server's trace shows
  the textures packing at the same time.
- **Nothing else changes:** the six headless screenshots are byte-identical to the baseline, and the 28 `TextureUploaded` events match.
- **No double streaming:** the server's summary for the batch shows each texture sent once, and no texture is requested a second time.

## Results

- **One request:** opening Sponza on an empty cache, the server's trace shows three requests in all: the 17 shaders, the layer, and one for the 25
  textures (`25 sent (2133.3 MiB)`, done in 5.8 s). No texture was requested again.
- **Cold load** (Sponza, 60 frames, debug): 18.6 s, from 46.2 s.
- **Warm load:** 12.4 s, from 14.0 s; the streams overlap.
- **Nothing else changes:** the six headless screenshots are byte-identical to the baseline, the 28 `TextureUploaded` events match cold and warm,
  and Kitchen_set opens with 2744 prims and the same image.
- All three configurations and the examples build.

Deviation: the prefetch skips materials already extracted (`materialPrimCache`). On a later `updateAssetBindings`, their textures would be
requested but never opened, and their bytes would stay in the asset client.
