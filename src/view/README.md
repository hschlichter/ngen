# View

ngen-view's library: the scene it shows (`viewscene.*`), picking (`viewpicking.*`), its commands as RPC methods and script verbs
(`viewcommands.*`), and its dumps (`viewdumps.*`). The program's `main` is `src/apps/view.cpp`; its UI is `ViewUI` (`src/ui/viewui.*`).

## The scene: `ViewScene`

The view shows a scene composed from its packs, with nothing of USD in it (`src/world/README.md`). Nothing about it waits.

**The scene thread.** `ViewScene` owns a thread, which owns the composer, the packs and the libraries.
- **Requests.** It takes requests in order: `open`, `setLodLevel`, `setPurposeShown`, `refresh` (Repack and change notices) and
  `setLocalTransform` (gizmo drags and `translate`). Each returns at once.
- **Arrivals.** It takes what the asset server answered (`AssetClient::takeAnswered`, woken by the client).
- **Per batch** of requests and arrivals it composes with the layer packs held (one that hasn't arrived is a gap), requests the packs it names
  and lacks, requests the meshes and materials of what is drawn and the textures of the materials held, and extracts.
- **Publishing.** It publishes a `SceneSnapshot`, which the main thread takes once a frame (`update`) and all queries read, and the changes for
  the renderer.
- **Unchanged arrivals.** An asset fetched again that arrives with the same version keeps its library entry, so nothing is uploaded again.

**What draws while things arrive.** A prim draws once its mesh is on the GPU. A material whose texture is still uploading samples white. A
cutout whose opacity texture is still uploading draws opaque. A material that hasn't arrived uses the display colour.

**Resident.** The scene is resident when everything it wants is loaded, uploaded and drawn:
- the scene thread has nothing queued, requested or unextracted;
- the render thread has applied every change list in a frame it finished, with no upload or pipeline pending.

The main loop checks once a frame, and traces `SceneResident` when it becomes true, with the time since start and the longest frame while
loading.

Until the scene is first resident the camera frames what has arrived, unless it is moved by hand. Script frames, `--frames` and `--screenshot`
count only while nothing waits for residency, so a script sees the scene loaded:
- **The start.** Counting starts when the scene is first resident; `--no-wait-resident` counts from start-up instead, to measure loading.
- **Methods that change what is drawn** (`view.lod.set`, `view.purposes`, `view.repack`) answer once the scene is resident again, and a script
  holds its count until they have.
- **`wait-resident`** (`view.waitResident`) waits the same way.

The inputs persist across rebuilds:
- **Libraries.** `MeshLibrary`, `MaterialLibrary` and `TextureLibrary` keep one entry per mesh asset, per material (or display colour) and per
  texture asset, shared by every draw using it. An entry whose asset arrives again with new content is a new entry with a new handle.
- **Draws.** `RenderWorld` holds a slot per draw (a prim's submesh), kept by its key (path and submesh) across rebuilds. A gone draw's slot dies
  and is reused, lowest first, so a fresh open fills slots in visit order.
- **Changes.** Each rebuild compares what it draws with what it drew and records the difference as `SceneChanges`. The main loop takes them
  (`takeChanges`) and hands them to the renderer, which changes only that on the GPU (`src/renderer/README.md`, "Scene data on the GPU").

- **What is drawn**: entities that are active, defined and not abstract, visible (an invisible ancestor hides its subtree), not in a LOD
  level their root doesn't draw, with a mesh asset, and of a purpose that is shown: `default` and `render` always, `guide` and `proxy`
  when turned on (`purposes` verb, `view.purposes`, the View menu).
- **Materials**: a mesh's bound material, and each `GeomSubset`'s own binding for its submesh, unless an ancestor's binding is
  `strongerThanDescendants`. A material's texture, else its base colour, else the mesh's display colour; a constant colour is a 1x1
  texture. A mesh with no material bound gets one per display colour, shared. A cutout material's opacity texture is fetched with its base
  colour texture and counts as used by its draws (`drawsOf`).
- **Transforms**: in float, parent times local, then scaled to metres by the root layer's `metersPerUnit`, as the view always drew; the
  camera's up is the scene's up axis.
- **The engine's light**: when the scene has no distant light, one is added, upper-front-right in the scene's frame (intensity 1, colour
  (1, 0.95, 0.88), angle 0.53).
- **Changing what is drawn**: `setLodLevel` (the `lod` verb, `view.lod.set`) and `setPurposeShown` are requests: the scene thread
  composes, fetches what the change needs and rebuilds, and the renderer uploads the changes over the next frames within its budget.
- **Names**: draw instances and lights carry a prim index; `primPath` turns it into the entity's full path, for the render debugger, the
  frame debugger, the GPU scene window and the dumps.

The trace shows the steps:
- `SceneRequested`;
- `PacksComposed`, once every layer pack named has been answered (packs, prims, prototypes);
- `SceneAssetsReceived`, once the meshes, materials and textures wanted have been answered;
- `WorldExtracted`, per batch (instances, meshes, materials, textures, lights, time);
- `AssetsRefreshRequested`;
- `SceneResident`;
- and `SceneAssetFailed` or `CompositionMessage` warnings.

## What arrived: the Assets window

Windows → Assets (or `window assets`) lists every asset the view requested from `AssetClient::log()`: its state, whether the server packed it
or served its cache, its size, and its times. Total is request to arrival; Server is the server's own time (the cache check, and packing when
it ran); Rest is the difference, waiting in the server's queue and streaming. A filter by id and by state, sorting by any column, and a
failed asset's errors on hover. The `assets` record carries the same list next to the totals.

**Going to an asset.** A double-click, or Go to in the row's menu, moves the camera to the asset's draws: the nearest first, then the next at
each go-to ("draw 3 of 412"), each framed and its prim selected; Frame all frames every draw at once. An asset's draws are the prims whose
draws use it (`ViewScene::drawsOf`, the Draws column): a mesh's prims, a material's or a texture's users, the prims drawing the meshes a USD
file defines. A file that only overrides or arranges others, and a shader, draw nothing. The `goto-asset <id> [all]` verb and
`view.gotoAsset` do the same and answer with the prim.

The other way, **Selected prim's assets** selects the rows of every asset the prim selected in the view uses, and everything drawn under it
(`ViewScene::assetsUnder`): its meshes, the USD files defining them, its materials and their textures; Show selected only narrows the list to
them, ready for Repack selected. `assets-of [prim]` and `view.assetsOf` answer with the list (the selection when no prim is given).

**Repacking.** Rows select as in a file list: click, Ctrl+click to add or remove one, Shift+click for a range as sorted and filtered, Ctrl+A
or Select shown for every row shown. Repack selected (or the row's menu) has the asset server repack the selection even when its cache is up
to date; Repack scene does it for every asset of the scene (`ViewScene::assetIds`: layer packs, meshes, materials, textures). The view
fetches what was repacked, fetches again the meshes and materials of a repacked USD file, and rebuilds its scene, so an edit to a source shows
without a restart (`ViewScene::refresh` with `force`, the `repack <id>... | scene` verb and `view.repack`, which answers once the scene is resident
again; the trace has `AssetsRefreshRequested`). What arrives unchanged keeps its library entry and isn't uploaded again. A shader's repack
refreshes only the server's cache.

**Edits reach the view by themselves.** The asset server tells the view when a source of something it received changes (`asset.changed`,
`src/asset/README.md`, "Change notices"). Each frame the view takes the changed ids: shaders are requested again and their pipelines recreated
when they arrive (`src/renderer/README.md`, "Pipelines"); the scene's assets go through `ViewScene::refresh` without `force`, the same path as
Repack (`AssetsRefreshRequested` with `forced: false`). Only what changed is uploaded: an edit to a prim's display colour changes its mesh pack
(the colour is in it), so that one mesh, its material and the material's colour texture. The Assets window marks a changed row stale until
its new version arrives; the `assets` record has `stale` and `changes`.

## Selection, picking and gizmos

The selection is a prim's full path (instances expanded), so a prim inside an instance is that instance's. A click picks on the GPU
(`viewpicking.*`): the geometry pass writes each pixel's instance index + 1 into `gbuffer.instance`, the click asks for that one texel after the
pass, and a few frames later the instance under the cursor maps to its prim (`instancePath`); nothing drawn there deselects. The `pick <x> <y>`
verb and `view.pick` do the same at a framebuffer pixel and answer with the path; `select` and `view.select` select by path. Keys: Z
deselects, R selects the parent, F frames the selection, 1 to 4 pick the tool (select, translate, rotate, scale); the Edit menu has the same.

A gizmo drag is the view's own opinion on the prim's local transform (`setLocalTransform`), kept until the view closes and never saved: it
moves the draw instances and lights under the prim at once, without rebuilding the draws, and every later rebuild applies it too. The
`translate` verb and `view.translate` offset a local position the same way. Editing the scene for real is the editor's (`src/editor/`).
