# Scene

The scene is the USD stage. `USDScene` composes it and keeps a runtime record per prim, `USDRenderExtractor` turns it into the renderer's inputs
(`RenderWorld`, `MeshLibrary`, `MaterialLibrary`), and `SceneUpdater` with `UndoStack` applies edits with undo. Everything that touches OpenUSD
lives in the `usd*.cpp` files (the `sceneusd` library); the headers keep pxr out of engine code behind a pimpl.

| Files | What |
|---|---|
| `usdscene.*` | the stage, its layers, prim records, transforms and lights, texture loading |
| `usdrenderextractor.*` | the stage to `RenderWorld`, meshes and materials |
| `usdassetresolver.*` | USD's asset resolver: every asset read through the asset server |
| `sceneupdater.*`, `undostack.*` | edits and undo |
| `scenequery.*`, `spatialindex.*`, `boundscache.*` | picking and bounds |
| `primshapemesh.*` | triangle meshes for the USD shape schemas (cube, sphere, cylinder, cone) |
| `mesh.h`, `material.h`, `scenetypes.h`, `scenehandles.h` | the plain types the renderer and the editor share |

## Where the scene's bytes come from

Every file the scene reads goes through USD's asset resolver: the root layer, sublayers, references, payloads, and textures
(`loadTextureFromResolvedPath` opens them with `ArGetResolver().OpenAsset`). Textures arrive packed (`src/asset/pack/packedtexture.h`): RGBA8 sRGB
with every mip level, copied into `MaterialDesc` as they are; the scene decodes nothing. ngen-view replaces USD's default resolver with
`NgenAssetResolver` (`usdassetresolver.*`), which reads through the view's `AssetClient` (`src/asset/README.md`), so the scene's data comes
streamed from the asset server and the view never opens an asset file.

- **Identifiers are asset ids**: paths relative to the asset server's working directory, with forward slashes. A relative asset path is anchored to
  the directory of the layer that authors it; an absolute path, or one that climbs above the server's directory, doesn't resolve. `\` in an asset
  path is read as a separator (Windows-authored assets such as NewSponza write `@textures\brick.png@`).
- **Resolving asks the server nothing.** A missing asset fails when it is opened, with the server's reason in the log (`asset.failed`).
- **Reads** fetch the asset (one request, shared by concurrent opens of the same id), take its bytes out of the client, and hand them to USD as an
  in-memory asset that owns them.
- **Textures are requested in one batch.** Before extracting materials, `updateAssetBindings` collects the base colour texture of every material
  bound to a renderable prim or its material-bind subsets, and `prefetchAssets` requests them together, so the asset server packs and streams them
  at the same time. Each open then waits for its texture instead of requesting it. Materials already extracted are skipped, since nothing would
  open their textures again.
- **Writes go to disk.** A layer save writes the file at its id, relative to the view's working directory, which is the asset server's (the view
  finds the server through `.ngen-discovery/` there); the editor still lives in the view. The next request for that asset repacks it.
- **USD's own files are not assets.** Files under a registered USD plugin's resources (the schema definitions, `generatedSchema.usda`) keep their
  absolute paths and are read from disk.
- **Registration:** USD only uses a resolver whose type a registered plugin declares. `usdplugins/plugInfo.json` is a `resource` plugin (metadata,
  no library) declaring `NgenAssetResolver`; the build copies it to `_out/<platform>/<config>/usdplugins/`, and `registerAssetResolver` registers
  it and makes the resolver USD's preferred one. It runs before anything uses USD.

## C++ standard

`usd*.cpp` compile as C++20: OpenUSD's `usd/usd/schemaRegistry.h` holds `unique_ptr`s to an incomplete type that C++23's standard library
rejects, and every header that includes it (`usd/usd/stage.h`, `prim.h`, the schema headers) fails as C++23. `usdassetresolver.cpp` is the
exception, compiled as C++23 so it can use `AssetClient`: it includes only `usd/ar`, `usd/sdf` and `base` headers, and must keep it that way.
