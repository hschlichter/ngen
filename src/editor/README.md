# Editor

The USD editor code, kept here unchanged and unbuilt until `ngen-editor` is built from it. It was ngen-view's: the view composed the scene
as a USD stage, and its editor windows edited it in process. The view now renders from packs and links no OpenUSD (`src/view/README.md`),
so this code has no program; nothing in `build.cpp` compiles it.

| Folder | What |
|---|---|
| `scene/` | `USDScene` (the stage, its layers, prim records, transforms, lights, meshes and materials), `USDRenderExtractor` (the stage to `RenderWorld`), `NgenAssetResolver` and its plugin metadata (`usdplugins/`, every asset read through the asset server), USD diagnostics into the trace, `SceneUpdater` and `UndoStack` (edits with undo), `SceneQuerySystem`, `SpatialIndex` and `BoundsCache` (CPU picking and bounds) |
| `ui/` | `EditorUI` and the editor windows: scene tree, properties, layers, undo history, tools, asset browser, the editor's menu bar, `editcommand.h` |

What it relied on when it was last built:

- **C++20 for OpenUSD**: `usd*.cpp` compiled as C++20, because OpenUSD's `usd/usd/schemaRegistry.h` holds `unique_ptr`s to an incomplete type that
  C++23's standard library rejects; `usdassetresolver.cpp` compiled as C++23 to use `AssetClient`, including only `usd/ar`, `usd/sdf` and `base`
  headers.
- **The resolver plugin**: `usdplugins/plugInfo.json` copied to `_out/<platform>/<config>/usdplugins/` and registered before anything used USD;
  identifiers were asset ids, `\` read as a separator, textures arriving packed.
- **The view's types**: `MeshLibrary`, `MaterialLibrary` and the plain types in `src/scene/`, `RenderWorld` in `src/renderer/`, and the
  introspection windows of `src/ui/`. Those have moved on since (the render debug window names prims through a path function), so building it
  again starts with bringing it up to date.

The editor that is built from it edits USD and sends the view what changed; the view keeps rendering from packs.
