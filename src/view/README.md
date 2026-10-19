# View

ngen-view's library: the scene it shows (`viewscene.*`, its placement tree in `placement.*`), picking (`viewpicking.*`), its commands as RPC methods and script verbs
(`viewcommands.*`), and its dumps (`viewdumps.*`). The program's `main` is `src/apps/view.cpp`; its UI is `ViewUI` (`src/ui/viewui.*`).

## The scene: `ViewScene`

The view shows a scene composed from its packs, with nothing of USD in it (`src/world/README.md`). Nothing about it waits.

**The scene thread.** `ViewScene` owns a thread, which owns the composer, the packs and the libraries.
- **Requests.** It takes requests in order: `open`, `setLodLevel`, `setPurposeShown`, `refresh` (Repack and change notices) and
  `setLocalTransform` (gizmo drags and `translate`). Each returns at once.
- **Arrivals.** It takes what the asset server answered (`AssetClient::takeAnswered`, woken by the client).
- **Per batch** of requests and arrivals it composes with the layer packs held (one that hasn't arrived is a gap), requests the packs it names
  and lacks, and runs a placement pass, which requests the meshes, materials and textures the placements want and lack.
- **Publishing.** It publishes a `SceneSnapshot`, which the main thread takes once a frame (`update`) and all queries read, and the changes for
  the renderer.
- **Unchanged arrivals.** An asset fetched again that arrives with the same version keeps its library entry, so nothing is uploaded again.

**What draws while things arrive.** A prim draws once its mesh is on the GPU. A material whose texture is still uploading samples white. A
cutout whose opacity texture is still uploading draws opaque. A material that hasn't arrived uses the display colour.

**Resident.** The scene is resident when everything it wants is loaded, uploaded and drawn:
- the scene thread has nothing queued, requested or unplaced, no camera posted it hasn't evaluated, and no batch it took and hasn't
  published (so a moved camera's levels count: while the camera moves, the scene isn't resident);
- the render thread has applied every change list in a frame it finished, with no upload or pipeline pending.

The main loop checks once a frame, and traces `SceneResident` when it becomes true, with the time since start and the longest frame while
loading.

Until the scene is first resident the camera frames what has arrived, unless it is moved by hand. Script frames, `--frames` and `--screenshot`
count only while nothing waits for residency, so a script sees the scene loaded:
- **The start.** Counting starts when the scene is first resident; `--no-wait-resident` counts from start-up instead, to measure loading.
- **Methods that change what is drawn** (`view.lod.set`, `view.purposes`, `view.repack`) answer once the scene is resident again, and a script
  holds its count until they have.
- **`wait-resident`** (`view.waitResident`) waits the same way.

### The placement tree

What is drawn is a tree of placements (`placement.h`): a placement is a content of the World (the stage, a prototype at an instance, a LOD
level at its root; `src/world/README.md`) at a world transform, with the state it inherits there (purpose, the material binding in force),
or a `PointInstancer`'s points. It owns the draws of its own prims and its children: the instances and point instancers in it, and a
placement per loaded level of each LOD root in it, of which only the drawn level draws. A content never changes once linked, so neither
do its draws: only where it is placed.
- **LOD roots.** A streaming root (its levels are payloads; `src/world/README.md` "Loading") is a placed root with a placement per loaded
  level, as below. A render-LOD root (levels in place) draws its default level for now; its levels picked per placement on the GPU are the
  next step. It has no row in the `lod` and `streaming` records and the heuristic doesn't evaluate it.
- **Walks.** A content is walked once per version and inherited state: the walk keeps, in walk order, the prims that draw, light, frame,
  root LOD or have variant sets, with transforms relative to the content's root, and where its instances and levels sit (`ContentWalk`).
  Every placement of the content uses the kept walk; one with a gizmo's move under it is walked afresh, with the move.
- **Passes.** After a compose, a relink, a level evaluation, arrivals or a gizmo's move, a pass places the stage as the World is now. A
  placement is built again only when its content was composed again, it moved, its inherited state changed, a gizmo moved something in it, or
  an asset it waited for arrived; else it is kept as it is. A LOD root switching levels removes one level's draws and adds the other's. A
  pass looks only where something can change: a placement counts what it and everything below it wait for (`pending`: assets not held, and
  roots not drawing the level they want or whose loaded levels aren't linked yet), and a pass goes into a placement only when that count
  isn't 0, a root under it changed its wanted level, or a gizmo moved something under it; a placed content composed again makes it look
  everywhere. A purpose shown or hidden, or a mesh, material or texture repacked, places the tree afresh.
- **Draws.** `RenderWorld` holds a slot per draw (a prim's submesh). A placement built again puts its draws back in the slots and prim indices
  it had, in order, so a draw that didn't change sends nothing; others take the lowest free slot. A placement draws what is held and is
  built again as the assets it waits for arrive, so a scene opens progressively; a material not held yet draws with the display colour
  until it arrives.
- **Changes.** The slots a pass touched, against what they held, are its `SceneChanges`; the main loop takes them (`takeChanges`) and hands
  them to the renderer, which changes only that on the GPU (`src/renderer/README.md`, "Scene data on the GPU"). `PlacementsChanged`
  traces each pass that changed something: placements built, slots added, removed and updated, its time.
- **Requests.** A placement asks for the meshes, materials and textures its prims want and lack when it is built, drawn or not (a level
  loading is placed undrawn), so what a level needs is requested as soon as the level is linked.
- **Libraries.** `MeshLibrary`, `MaterialLibrary` and `TextureLibrary` keep one entry per mesh asset, per material (or display colour) and per
  texture asset, shared by every draw using it. An entry whose asset arrives again with new content is a new entry with a new handle. The
  slots using each mesh and material are counted as they change, so no pass looks at every slot.
- **Shared with the main thread.** The scene thread's tree and lists are shared with what it published: a placement or a chunk of slots
  (`ChunkedList`) a published snapshot holds is copied before it changes, so publishing copies no slots. `SceneDraws` holds the slots, the
  tree, the prim paths and the assets per prim; what the queries derive from them (the records, the prims per asset, the slots per prim) is
  gathered from it on the first query.

- **What is drawn**: entities that are active, defined and not abstract, visible (an invisible ancestor hides its subtree), not in a LOD
  level their root doesn't draw, with a mesh asset, and of a purpose that is shown: `default` and `render` always, `guide` and `proxy`
  when turned on (`purposes` verb, `view.purposes`, the View menu).
- **Point instancers**: each visible point of a `PointInstancer` draws its prototype at the instancer's world transform times the point's.
  The prototype's walk (rooted at the prototype, in the instancer's content) is kept once and used per point; a point's draws have a path
  of their own, `<prototype>#<id>`, and no transform for the queries. A prototype beneath its instancer draws only through it. A level
  waits for its prototypes' meshes as for its own (`itemsReady`). An instancer is one placement, its points' draws its own.
- **Materials**: a material is its binding's key (`parseMaterialKey`): the pack, and the values of the inputs its textures take their files from
  (`resolveMaterial`), so one pack used with different inputs is one material per set of values. A mesh's bound material, and each `GeomSubset`'s own
  binding for its submesh, unless an ancestor's binding is `strongerThanDescendants`. A material's texture, else its base colour, else the mesh's
  display colour; a constant colour is a 1x1 texture. A mesh with no material bound gets one per display colour, shared. A cutout material's opacity
  texture is fetched with its base colour texture and counts as used by its draws (`drawsOf`).
- **Transforms**: in float, parent times local, then scaled to metres by the root layer's `metersPerUnit`, as the view always drew; the
  camera's up is the scene's up axis. A prim's transform for the queries (`transform`, selection, the gizmo) is found in the published tree
  (`findTransform`): the deepest drawn placement at or above the path that holds it, the prim's transform in its walk, placed. A gizmo's
  move is the view's local opinion of the prim; the placements holding it are built again in the same batch, and what is under it moves.
- **Cameras**: every `Camera` prim reached while drawing, by full path with its world transform (`cameras()`). The Camera window lists them
  and moves the view to one on a click; `--camera=/path`, the `camera-use` verb and `view.camera.use` do the same, and `view.camera.list`
  lists them with their poses. The view takes a camera's position and the direction it looks (its local -Z); roll, focal length and
  aperture aren't read (the packer doesn't keep them), so the view's 45° field of view stays.
- **The engine's light**: when the scene has no distant light, one is added, upper-front-right in the scene's frame (intensity 1, colour
  (1, 0.95, 0.88), angle 0.53).
- **Changing what is drawn**: `setLodLevel` (the `lod` verb, `view.lod.set`), `resetLodLevel` (`lod-reset`, `view.lod.reset`: back to the
  root's default) and `setPurposeShown` are requests: the scene thread composes, fetches what the change needs and rebuilds, and the
  renderer uploads the changes over the next frames within its budget.
- **LOD roots**: every LOD root reached while drawing (`lodRoots()`, the `lod` record): its levels and each level's payloads, its
  heuristic (type, thresholds, extent, projection), the level it defaults to, wants and draws, the levels loaded, and whether its level was
  set by hand.
- **The LOD heuristic**: every LOD root reached wants the level its screen size picks: UsdLod's `LODScreenSizeHeuristic`, its extent's bounding sphere
  placed by the root's transform, as a fraction of the view window at depth 1, against its `thresholds`. Unlike USD's reference query it ignores the
  frustum, so what is near stays loaded whichever way the camera looks. Going coarser waits until the size is 20% below the threshold, so a camera at
  a threshold doesn't flip a root. A level set by hand (`lod`, the LOD window) wins until `lod-reset`; with the heuristic off (`lod-heuristic off`,
  `view.lod.heuristic`, the LOD window's toggle) every other root wants its default; a root without a usable heuristic (`heuristicChosen`,
  src/world/README.md "Loading") wants its default ("no usable heuristic"). Every root the heuristic picks finer goes finer: what fits in memory is
  the budgets' to decide, not a count of roots. The main thread posts the camera (`setCamera`) once it has been placed, not while it follows the scene
  loading; the scene thread evaluates when it moved, after a pass that drew roots not drawn before and when levels changed, and gives the composer only what changed, which links
  the levels again without composing the stage (`WorldComposer::relink`). The `streaming` record has per root its screen size, whether its extent is
  in the view's frustum (what USD's query would go by), the level wanted and why, drawn and loaded, and for a level waited for what it waits for;
  totals over the roots. `LevelsWanted` traces each change of wanted levels.
- **Switching levels**: a root draws the finest loaded level, not finer than the one it wants, that is ready (`contentReady`): linked, which the
  composer does only once it is complete, and every mesh it draws (under its own purpose) held or failed, through the instances in it; a LOD
  root inside it counts by its coarsest level. Until then the root keeps drawing the coarser level, and switches in the pass after the last
  piece arrives, so a district appears whole instead of model by model. `LevelsSwitched` traces the roots whose drawn level changed, once
  per pass; the `streaming` record's `waiting` and the LOD window's tooltip on a loading row count what the wanted level waits for
  (meshes not held, those asked for, levels still composing and so not linked). GPU uploads aren't part of it: a switched level's meshes are
  uploaded over the next frames within the upload budget.
- **Unloading**: what stops being drawn (a mesh, material or texture: its library entry and its GPU entry) is kept for the unload delay
  (10 s by default; `lod-unload-delay S`, `view.lod.heuristic {unloadDelay}`, the LOD window), so turning back draws it without loading;
  after that it leaves the libraries and the renderer. A mesh's pack is released once no placement names it (drawn or not), so a root's coarsest level
  (always loaded) keeps its packs while a finer level is drawn. Composed contents not reached for the delay leave the composer
  (`WorldComposer::evict`). Layer packs stay held. `LevelsUnloaded` traces each release, and the `streaming` and `scenethread` records,
  the Scene Flow window and the Engine Map count what was unloaded.
- **Point instancers and variants reached**: every PointInstancer reached while drawing (`instancers()`, the `instancers` record): its
  points and those shown, its prototypes, and the prims its points drew; every prim with variant sets (`variantPrims()`, the `variants`
  record): each set and the variant it composed to (`Entity::variantSelections`). A LOD root in a point's prototype draws its default
  level and has no row in `lodRoots()`: the heuristic doesn't choose per point (the composer says so once per instancer).
- **Payload placements**: every prim with payloads reached while drawing (`payloads()`, the `payloads` record): its asset ids, whether
  they are loaded, and its `extentsHint` in world metres. A level not loaded isn't reached, so what is under it isn't listed; its root's
  `levelPayloads` name the level's own payloads. Each draw carries the drawn level of the nearest LOD root above it,
  which the `lodlevel` view colours by. The LOD window lists the roots with a level picker per row, "Set shown" for every row its filter
  shows (default, coarsest or one level), a click to select a root and a double-click to frame it; it highlights the root at or above the
  selection, and marks a root still loading the level it wants.
- **Names**: draw instances and lights carry a prim index; `primPath` turns it into the entity's full path, for the render debugger, the
  frame debugger, the GPU scene window and the dumps.

The trace shows the steps:
- `SceneRequested`;
- `PacksComposed`, once every layer pack named has been answered (packs, prims, prototypes);
- `SceneAssetsReceived`, once the meshes, materials and textures wanted have been answered;
- `PlacementsChanged`, per pass that changed something (placements built, slots added, removed and updated, time);
- `AssetsRefreshRequested`;
- `SceneResident`;
- and `SceneAssetFailed` or `CompositionMessage` warnings.

## Where everything sits: the Engine Map

The Engine Map (Windows > Engine Map, `window enginemap`) is the engine drawn as lanes, one per process or thread (ngen-asset-server, the
asset client's I/O thread, the scene thread, the main thread, the render thread), with its parts as nodes in the order data flows and arrows
for what flows between them. Selecting a node labels its arrows and shows what it is, what it receives and hands on, its live numbers, its
records (those of the process it runs in), the windows that show it (a button opens each) and its README. Windows about one part carry a
"Map: <lane> > <part>" link that opens the map at it.

The map is hand-written (`src/ui/enginemap.cpp`): it says what each part means, which a map derived from threads or profiler zones
couldn't. A new part of the engine adds its node and edges there with its records and windows; its live numbers come from
`engineMapStats` (`viewcommands.cpp`), which the `map` record serves too.

## How a scene reaches the screen: the Scene Flow window

The Scene Flow window (Windows > Scene Flow) shows the path from the asset server to the screen, stage by stage in the order data flows, each
saying what it is and where its data goes next:
1. **Asset client**: requested, received (packed or from the server's cache), failed, in flight.
2. **Scene thread**: requests posted and applied, batches, composes and placement passes (count, whether one is needed, the placements and
   those drawn, the last changing pass's time, placements built and slots added, removed and updated, walks built and kept), assets in flight
   by kind, packs held, library entries, idle.
3. **Change lists**: what a placement pass changed, which the main thread takes each frame and hands to the render thread.
4. **GPU scene** (render thread): change lists submitted and drawn, the tables, the upload queue (queued, budget, the last SceneUpload pass)
   and the geometry pool (use against capacity and limit, meshes waiting for room). The upload budget and the pool limit are set here.
5. **Residency**: resident or loading, the script's hold, and the last load's frames and longest frame.

The same numbers are the `scenethread` record (stages 2 and 5) and the `gpuscene` record (stage 4), which the render thread publishes after
every frame whatever window is open. `view.gpuscene.set {uploadBudgetMiB, geometryLimitMiB}` (the `gpuscene budget=…,limit=…` verb) sets the
budget and the limit from the next frame; lowering the limit stops growth, it doesn't shrink the pool.

The hemisphere ambient is a setting too: `view.ambient {sky, ground, reset}` (the `ambient sky=r,g,b ground=r,g,b` verb), the Render Debug
window's Ambient section, and the `status` record's `ambient`. `view.camera.set` takes an optional `speed`.

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
