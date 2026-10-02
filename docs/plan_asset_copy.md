# Scenes and textures through the asset server, by copy

**Status. Landed.**

Part of step 2 of [plan_tool_architecture.md](plan_tool_architecture.md), before the engine-ready formats. It builds on
[plan_asset_server.md](plan_asset_server.md). Every file ngen-view reads for a scene (USD layers, references, payloads and textures) comes streamed from
the asset server, packed by a copy packer that doesn't transform anything. The engine keeps parsing USD and decoding PNGs as it does today; only where
the bytes come from changes. Later packers replace the copy rule per type (a texture packer for `.png`, the USD packer for scenes) without the view's
asset ids changing.

## Current state

- **All of the scene's file reads go through USD's asset resolver.** `USDScene::open` calls `UsdStage::Open(path)`; sublayers, references,
  payloads and muted layers open through `SdfLayer::FindOrOpen`; textures are opened with `ArGetResolver().OpenAsset` and decoded with
  `stbi_load_from_memory` (`loadTextureFromResolvedPath` in `src/scene/usdscene.cpp`). Nothing in `src/scene/` reads an asset with `std` file IO.
- **Saves go through the resolver too:** `SdfLayer::Save`, `Export` and `CreateNew` write through `ArResolver::OpenAssetForWrite`.
- **A resolver can be replaced** (spikes, 2026-10-01, not in the repo): an `ArResolver` subclass, declared in a `plugInfo.json`, registered with
  `PlugRegistry::RegisterPlugins` and chosen with `ArSetPreferredResolver`, served a stage and a texture from in-memory buffers
  (`ArInMemoryAsset`). Sponza's 437 MB layer opened that way in 2.7 s (1170 prims). USD requires the resolver's type to belong to a registered
  plugin, but a `resource` plugin (metadata only, no library) is enough: a resolver compiled into the program itself worked the same way.
- **The USD code compiles as C++20.** `src/scene/usd*.cpp` build with `-std=c++20` (`sceneusd` in `build.cpp`), because `usd/usd/schemaRegistry.h`
  holds `unique_ptr`s to an incomplete type that C++23's standard library rejects (clang 22, libstdc++ 16). Every header that includes it fails
  (`usd/usd/stage.h`, `prim.h`, the schema headers); `usd/ar/*` and `usd/sdf/layer.h` compile as C++23, and so does a resolver using them together
  with `assetclient.h`. Upstream OpenUSD (`dev`, 2026-09-30) still targets C++17, and the header is unchanged since v26.03.
- **The asset server** ([plan_asset_server.md](plan_asset_server.md)) packs by rule per extension, caches, and streams; `AssetClient` keeps every
  asset it received for the life of the process.
- **Test assets are inside the project:** `assets/main_sponza/` (git-ignored; one 437 MB `.usda`, 143 PNGs, 2.7 GB; 6.3 GB with the other files),
  `assets/models/Kitchen_set/` (`.usd` files with references and payloads), `assets/three_cubes.usda`.

## Scope

**In**

- **`ngen-packer-copy`**: a packer whose packed asset is the source's bytes, unchanged.
- **A `copy` rule in `pack.cpp`** for `.usda`, `.usdc`, `.usd`, `.png`, `.jpg`, `.jpeg` and `.hdr`.
- **An asset resolver for USD** (`NgenAssetResolver`) in the scene library, using `AssetClient`: reads come from the asset server, writes go to disk.
- **`AssetClient::take(id)`**, so the bytes of an asset can be moved out and freed once USD or the decoder has them.
- **ngen-view** registers the resolver before the scene opens, and names the scene by asset id.

**Out**

- **Any transformation of the data**: the texture packer, the USD packer and engine-ready scene formats are later plans.
- **Requesting textures ahead of use** (one batch per material or scene). Textures are fetched one at a time, in the order the material loop needs
  them. See Deferred.
- **The asset browser and the reference browser**, which list files on disk (`referenceBrowser.rootDir = rootLayerDirectory()`). They keep
  listing the local project; an id they produce is still a project-relative path, so it works.
- **Package formats** (`.usdz`), which need an `ArPackageResolver`.
- **Removing USD from ngen-view**: step 3, the editor split.

## Decisions

Proposed; pushback welcome.

1. **The copy packer copies with `cp --reflink=auto` semantics.** On btrfs (this machine) and XFS the packed copy shares the source's blocks until
   either is written, so Sponza's 6.3 GB is not duplicated on disk. Elsewhere it is a plain copy. The packer uses `ioctl(FICLONE)` and falls back to
   copying the bytes; its depfile lists only the source. A hard link was the alternative: free on any filesystem, but an in-place edit of the source
   would then also change the packed file behind the cache's back.
2. **The resolver lives in the scene library** (locked with Henrik). `src/scene/usdassetresolver.cpp` is part of `sceneusd`, which ngen-view
   already links. Its type is declared by a `resource` plugin: a `plugInfo.json` with no library, copied into
   `_out/<platform>/<config>/usdplugins/` by a build tool. A separate shared library was the alternative; the second spike showed it isn't needed.
3. **The resolver uses `AssetClient` directly** (locked with Henrik). The asset system is lower level than the scene, and systems above it use it:
   `sceneusd` links `assetclient`. The view connects the client and gives the resolver a pointer to it; `OpenAsset` does `request({id})`,
   `wait`, `take`. `AssetClient` keeps its interface, `std::expected` included.
4. **The resolver's file compiles as C++23** (locked with Henrik). `sceneusd` overrides the standard for that one file, so it can include
   `assetclient.h`:

   ```cpp
   .for_source("src/scene/usdassetresolver.cpp", [](cxx::ObjectFile& file) { file.std("c++23"); })
   ```

   The rule that makes it work: the file includes only `usd/ar/*`, `usd/sdf/*` and `base/*` headers, never `usd/usd/*` or a schema. A comment at
   its includes and at the build line says so. The alternatives were patching `schemaRegistry.h` in the vendored OpenUSD (the whole scene library
   could then move to C++23, at the cost of carrying a local change), giving `AssetClient` a C++20 interface, and a C++23 wrapper file between the
   resolver and the client.
5. **Identifiers are asset ids.** The resolver's identifier for a path is its project-relative id:
   - a path relative to an anchoring layer is joined to that layer's directory and normalised: `@./textures/brick.png@` in
     `assets/main_sponza/NewSponza_Main_USD_Zup_003.usda` becomes `assets/main_sponza/textures/brick.png`
   - an absolute path inside the project is made relative to the project root; one outside it fails to resolve
   - Windows separators are left for `resolveTexturePath`, which already normalises them before resolving
   - the resolved path is the id itself. `Resolve` asks the server nothing, so a missing asset fails when it is opened (`asset.failed`), not when
     it is resolved
6. **Reads are in-memory assets without a copy.** `OpenAsset` fetches the bytes, and wraps them in an `ArInMemoryAsset` whose buffer owns the
   `std::vector` it came from. The asset client no longer holds them after `take`.
7. **Writes go to disk under the project root.** `OpenAssetForWrite` maps the id to `<project root>/<id>` and uses `ArFilesystemWritableAsset`.
   The editor still lives in the view, and saving a layer writes its source; the server repacks it on the next request. This is the one place the
   view touches project files until the editor split.
8. **The view's scene argument becomes an id.** A path given on the command line is made absolute against the working directory, then relative
   to the project root; a scene outside the project is refused with a message saying so.

## Steps

1. **`ngen-packer-copy`** (`src/apps/packercopy.cpp`, on `src/asset/pack/packer.h`): copy `--source` to `--out` by `FICLONE`, falling back to a
   byte copy; write `--depfile` with the source. A program target in `build.cpp`; `ngen-asset-server` depends on it.
2. **The rule** in `pack.cpp`:

   ```cpp
   rules.push_back(PackRule{
       .name = "copy",
       .extensions = {".usda", ".usdc", ".usd", ".png", ".jpg", ".jpeg", ".hdr"},
       .packer = "ngen-packer-copy",
       .params = {},
       .version = 1,
   });
   ```
3. **`AssetClient::take`** (`src/asset/assetclient.*`):

   ```cpp
   // Moves an arrived asset out of the client; nullopt if it hasn't arrived. A later request for the id streams it again.
   auto take(const std::string& id) -> std::optional<PackedAsset>;
   ```
4. **The resolver** (`src/scene/usdassetresolver.h/.cpp`, `src/scene/usdplugins/plugInfo.json`):

   ```cpp
   class AssetClient;

   // Registers the resolver's plugin from <bin>/usdplugins, makes it USD's preferred resolver, and has it read every asset through `client`.
   // Before any Ar use; `client` must be connected and outlive every stage.
   auto registerAssetResolver(AssetClient* client, const std::filesystem::path& binDirectory, const std::filesystem::path& projectRoot) -> bool;
   ```

   `NgenAssetResolver : ArResolver` overrides `_CreateIdentifier`, `_CreateIdentifierForNewAsset`, `_Resolve`, `_ResolveForNewAsset`,
   `_OpenAsset`, `_OpenAssetForWrite` and `_GetModificationTimestamp` (the asset's version, so a layer's `Reload` sees a change). A failed fetch
   logs the server's errors with the id.
   - `build.cpp`: `sceneusd` links `assetclient` and compiles `usdassetresolver.cpp` as C++23 (Decision 4); a tool copies
     `src/scene/usdplugins/plugInfo.json` into `_out/<platform>/<config>/usdplugins/`, and ngen-view depends on it.
   - `usdassetresolver.h` has no pxr includes, so ngen-view's C++23 code can include it.
5. **ngen-view start-up** (`src/apps/view.cpp`): after `AssetClient::connect`, `registerAssetResolver(&assetClient, …)`, then the scene
   argument turned into an id and opened.
6. **Docs:** `src/asset/README.md` (the copy rule, `take`); a new `src/scene/README.md`, linked from the root README, for the resolver (ids, reads
   from the server, writes to disk, the `resource` plugin, the C++23 file and its include rule; `src/scene/` has no README yet); the run-headless
   skill (Sponza's path is now `assets/main_sponza/NewSponza_Main_USD_Zup_003.usda`).

## Verification

- **Nothing read from disk:** `strace -f -e trace=openat` on ngen-view opening Sponza shows no file opened under `assets/` or `_out/*/assets/`;
  the scene's layers and textures all appear as `client … request` lines in the server's trace.
- **The same pictures:** the six headless screenshots (three_cubes and Sponza, prepass on and off) are byte-identical to the baseline, with Sponza
  opened by id.
- **Same textures:** Sponza's run has the same `TextureUploaded` events as before (count and sizes), from the obs bus.
- **Composition arcs:** `assets/models/Kitchen_set/Kitchen_set.usd`, with its references and payloads, loads with the same prim count
  (`dump-scene` or `view.status`) as with the default resolver, and every file it opens is a request in the server's trace.
- **Copying costs no disk on btrfs:** after packing Sponza, `btrfs filesystem du` shows `_out/linux-vulkan/debug/assets/assets/main_sponza/` as
  shared with `assets/main_sponza/`, not exclusive.
- **Memory:** after Sponza has loaded, the view's resident size is within 10% of a run with the default resolver (the streamed bytes were taken
  and freed).
- **Save still works:** in the view, a transform edit and `scene.save` writes the layer file on disk; the next request for it repacks it (a
  `packing` line in the trace) and streams the new bytes.
- **Failures are named:** a scene referencing a missing texture logs the server's `no such file` for that id and renders with the fallback
  texture, as a missing texture does today. A scene path outside the project is refused before anything is requested.
- **Time:** Sponza's cold load (empty cache) and warm load through the server are each within 20% of today's load time from disk; the numbers are
  recorded in Results.

## Deferred / follow-ups

- **Requesting textures in batches** (all of a scene's texture ids at once, decoded as they arrive). Done in
  [plan_texture_batch.md](plan_texture_batch.md).
- **A texture packer** that writes an engine-ready format (mip chains, block compression) instead of PNG. Trigger: this plan lands; it is the first
  real packer after shaders.
- **The USD packer** and the scene pack format. Trigger: the plan for the scene pack format.
- **`.usdz` packages** through an `ArPackageResolver`. Trigger: a test asset in `.usdz`.
- **The asset and reference browsers listing assets from the server** instead of the disk. Trigger: the editor split, or a view on another
  machine.

## Results

- **Nothing read from disk:** `strace -f -e trace=openat,open,stat,newfstatat` on ngen-view opening Sponza (902 opens) and Kitchen_set (604)
  shows no file under `assets/` or the cache `_out/linux-vulkan/debug/assets/`. The layer and its 28 textures appear as requests in the server's
  trace.
- **The same pictures:** the six headless screenshots, with Sponza opened as `assets/main_sponza/...`, are byte-identical to the baseline.
- **Same textures:** Sponza's 28 `TextureUploaded` events have identical fields (sizes, mips, bytes; 2,236,962,112 bytes in all) through the server,
  cold and warm, and with USD's default resolver.
- **Composition arcs:** Kitchen_set, with its references and payloads, opens with 2744 prims both through the server and with the default resolver,
  and renders the same image.
- **No disk cost on btrfs:** the cache's copy of what Sponza requested is 1.01 GiB, with 0 bytes exclusive (`btrfs filesystem du`).
- **Memory:** peak resident size on Sponza is 4.60 GB through the server, against 4.69 GB with the default resolver.
- **Time** (Sponza, 200 frames, debug): 33.2 s with the default resolver, 37.4 s through the server on an empty cache (+13%), 34.6 s warm (+4%).
- **Saves:** a scratch program using the resolver opened a copy of three_cubes by id, added a prim and saved it. The file on disk had the change,
  and the next request repacked it (a `packing` line) and streamed the new bytes. ngen-view has no save verb, so it wasn't checked through the view.
- **Failures are named:** a texture that doesn't exist logs `NgenAssetResolver: cannot open assets/textures/missing_basecolor.png` with the
  server's `no such file`, and the scene renders; a scene path outside the project is refused before anything is requested.
- All three configurations and the examples build.

Deviations from the plan:

- **The resolver turns `\` into `/`.** The plan left Windows separators to `resolveTexturePath`, which only rewrites them when the path fails to
  resolve. Resolving asks the server nothing, so it never fails, and Sponza's `@textures\…@` paths reached the server as ids with a backslash
  (`no such file`, untextured). Asset ids use forward slashes, so the resolver normalises them.
- **USD's own files are read from disk.** USD's schema registry opens every schema plugin's `generatedSchema.usda` through the resolver. They are
  part of the USD runtime, not assets (they only resolved as ids because the OpenUSD build is inside the project). Paths under a registered plugin's
  resource folder keep their absolute path and are read with `ArFilesystemAsset`.
- **`assetIdForPath` is in the asset system** (`src/asset/assetid.h`), shared by the view's scene argument and the resolver.
- **A new request clears an earlier failure** in `AssetClient`, so a retried id doesn't return the stale error at once.
- **Concurrent opens of one id share a request** in the resolver, so the client never sees two streams for the same id.

Changed after landing (2026-10-02): there is no project root. The asset server's working directory is the root asset ids are relative to; its
cache is `.ngen-assets/<platform>/<config>/` there, and every tool's discovery file is in `.ngen-discovery/` in its working directory, so a view
finds the asset server it runs next to. The view's scene argument is an asset id as given. Binaries can be copied anywhere: a copy of the debug
binaries in an unrelated folder, run against a folder holding only `shaders/` and a scene, rendered the baseline image.
