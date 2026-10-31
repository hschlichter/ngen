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
  level, as below. A render-LOD root (levels in place) is a *group* where it is placed (`RenderLodGroup`, slots in `RenderWorld::lodGroups`
  taken and freed with its placement, as draw slots): the bounding sphere of its heuristic's extent placed by the root's transform, as the
  heuristic measures it, its thresholds (up to 8 levels; levels past them aren't drawn), its default, and a forced level (hand-set by its full
  path, else its default while the heuristic is off or it has no usable heuristic). Every level is walked and placed, its draws tagged with
  the group and their level, and an instance or point instancer under a level passes the tag to its draws; the renderer draws the level the
  group picks each frame (`src/renderer/README.md`, "Render LOD"). A camera move changes nothing here. A render-LOD root has no placed root,
  no row in the `lod` and `streaming` records and no CPU evaluation; `lod` and `lod-reset` on its path and `lod-heuristic` change its group's
  forced level. A level holding it is ready once every level's meshes are held, as all of them are placed. The `renderlod` record (next
  frames: it asks the renderer for every group's state) has the groups and their slots per level, what the GPU picked, and a check of every
  pick against this heuristic at the camera the GPU measured from (`check.differ` 0 but on a threshold's edge); `view.renderlod {path}`
  lists the groups under a path. The LOD window lists every group beside the streaming roots ("The LOD tool" below); the Culling window
  counts the slots of levels not drawn; the Engine Map's "LOD select" is the GPU's pick.
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
  texture asset, shared by every draw using it. An entry whose asset arrives again with new content is a new entry with a new handle. An
  entry is held while a placement uses it: the slots drawing it, and an undrawn placement (a level loading, a resident level coarser or finer
  than the one drawn) holding the entries its prims would draw with (`heldMeshes`, `heldMaterials`), so a root switching to a loaded
  level, finer or coarser, uploads nothing. The uses are counted as they change, so no pass looks at every slot; what no placement uses
  leaves the libraries and the renderer at once. A placement no longer drawn gives up its slots and holds their entries without being built
  again.
- **Shared with the main thread.** The scene thread's tree and lists are shared with what it published: a placement or a chunk of slots
  (`ChunkedList`) a published snapshot holds is copied before it changes, so publishing copies no slots. `SceneDraws` holds the slots, the
  tree, the prim paths and the assets per prim; what the queries derive from them (the records, the prims per asset, the slots per prim) is
  gathered from it on the first query. The rest a snapshot carries that seldom changes (the asset ids, the layer stack, the World's LOD
  levels, the hand-set levels, the tuning) is shared with the snapshot before until it changes, so a snapshot published for a camera move
  copies, and later frees, little more than the LOD choices.
- **Freed off the main thread.** A snapshot the main thread lets go of (`update` takes the next) goes back to the scene thread, which frees
  it on its next wake without publishing; one the main thread never took is freed after the lock, and a change list joins the pending
  one by moving its lists when that is empty. So `update` and `takeChanges` swap pointers under the lock and wait on nothing large.

- **What is drawn**: entities that are active, defined and not abstract, visible (an invisible ancestor hides its subtree), not in a LOD
  level their root doesn't draw, with a mesh asset, and of a purpose that is shown: `default` and `render` always, `guide` and `proxy`
  when turned on (`purposes` verb, `view.purposes`, the View menu).
- **Point instancers**: each visible point of a `PointInstancer` draws its prototype at the instancer's world transform times the point's.
  The prototype's walk (rooted at the prototype, in the instancer's content) is kept once and used per point; a point's draws have a path
  of their own, `<prototype>#<id>`, and no transform for the queries. A prototype beneath its instancer draws only through it. A level
  waits for its prototypes' meshes as for its own. An instancer is one placement, its points' draws its own.
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
  heuristics (every one it names, the one used marked), the level it defaults to, wants and draws, the levels loaded, and whether its level was
  set by hand.
- **The LOD heuristic**: every LOD root reached selects the level its imaging heuristic picks from the camera (`lodMeasure`, `pickLodLevel`),
  the level it draws. A root naming several measures by the view's preference (`lod-prefer distance|screen|authored`, `view.lod.heuristic {prefer}`, the LOD window; distance first
  by default): a distance is UsdLod's, to the heuristic's center or the nearest point of its bounding box, in the stage's units like its
  thresholds; a screen size is its extent's bounding sphere over the view window at depth 1, as a fraction of the window, which unlike USD's
  reference query ignores the frustum, so what is near stays loaded whichever way the camera looks. Inside an authored blend band
  (`blendThresholds`) a streaming root wants the finer level (render-LOD groups fade, src/renderer/README.md); at a boundary without one,
  going coarser waits for a margin (`lodHysteresis`, 20% until the tuning changes it; for a distance the same margin, as a size falls with
  its square), so a camera at a threshold doesn't flip a root. The view's LOD tuning applies here and to the groups ("The LOD tool"). A level set by hand (`lod`, the LOD window) wins until `lod-reset`; with the heuristic off (`lod-heuristic off`,
  `view.lod.heuristic`, the LOD window's toggle) every other root wants its default; a root without a usable heuristic (`heuristicChosen`,
  src/world/README.md "Loading") wants its default ("no usable imaging heuristic"). Every root the heuristic picks finer goes finer: what fits in
  memory is the budgets' to decide, not a count of roots. The roots evaluated are the streaming roots of drawn placements, kept as placements are built
  and released (`placedRoots`), so an evaluation costs per placed root and walks no tree. The main thread posts the camera (`setCamera`) once it has been placed, not while it follows the scene
  loading; the scene thread evaluates when it moved, after a pass that drew roots not drawn before and when levels changed, and gives the composer only what changed, which links
  the levels again without composing the stage (`WorldComposer::relink`). `ResidencyChanged` traces each change of selected and resident
  levels.
- **Residency**: what is loaded is decided by where the camera is and what is authored, never by time. A root's streaming heuristic (domain
  `streaming`: its thresholds the load radii, `streaming:unloadThresholds` the unload radii; src/world/README.md "Loading") gives its state, the
  finest level that must be resident (`streamingState`): finer at once by the load radii, coarser only past the unload radii, the previous state
  its hysteresis. The state's level and every coarser one are resident (`WorldComposer::setResidentLevels`), so every authored level is a ready
  fallback both ways: coming closer a root steps through its levels, moving away the coarser level is loaded before the finer one stops being
  selected. A root's children are placed, and so evaluated, only while its finest level is resident. A level set by hand is resident too; a root
  without a streaming heuristic keeps the level selected and every coarser one. Nothing covers for the authoring: a level selected finer than
  the state isn't loaded early, it draws late. A root no longer placed forgets its state and the composer its resident levels.
- **The authoring contract** (`checkLodAuthoring`, src/world/README.md): per root, with the thresholds in force, against the streaming root
  nearest above it: a threshold per level boundary, each imaging threshold at or before its load radius, each load radius at or before its
  unload radius, a child's imaging thresholds and unload radii inside its parent's first imaging threshold and load radius. A finding is
  reported (the `streaming` record's `findings`, with the values), not corrected.
- **Streaming measured** (`SceneStreaming`, `measureStreaming` after each batch): per root, which levels are ready (placed and waiting for
  nothing), each level's lead (seconds from ready to selected; 0 when selected before it was ready), late (drawing coarser than selected) or a
  hole (drawing nothing), its reloads; totals since the scene opened or `view.streaming.reset` (`streaming-reset`): late and holes now, late
  switches, holes, root-seconds drawn coarser, loads, reloads and releases, the leads' count, minimum and median, mesh packs that arrived with
  no placement naming them any more (`arrivedUnwanted`: asked for by a level no longer resident; requests aren't cancelled), and authoring
  findings. Time is measured here and never acted on.
- **The `streaming` record** has per root the imaging pick (`selected`, its heuristic, measure, thresholds and why), its streaming heuristic
  (measure, load and unload radii in force), `state`, `resident` and why, `drawn`, `late`, `hole`, `reloads`, per level its name, whether it is
  requested, loaded, ready and drawn, what its placement waits for and its lead, and its authoring findings; whether its extent is in the view's
  frustum (what USD's query would go by); the measured totals.
- **Flying a camera path** (`fly <camera path prefix> <speed m/s>`, `view.camera.fly`; `fly-stop`): the camera moves through the scene's
  cameras whose paths start with the prefix, in path order, at the speed in metres a second of each frame's time, eye and direction
  interpolated; a script holds its clock while it flies, and the call answers at the last camera (`FlightStarted`, `FlightDone`). A test
  driver for streaming: it moves the camera and decides nothing. Its numbers depend on the build, the asset server's cache and the machine,
  so a flight compares the engine with itself under the same conditions.
- **The debug camera** (`debugcamera.*`, the Camera window's Debug camera section, `debug-camera on|off|place`, `view.camera.debug`): a
  second camera to watch the main camera from. `cam` stays the main camera (the player): everything that places the camera (`camera`,
  `camera-use`, `fly`, `--camera`, scene cameras and bookmarks) moves it, and it drives what is decided from a viewpoint: residency, the
  LOD heuristic and render LOD (`RenderSnapshot::lodEye`; the LOD and Streaming inspectors and the Streaming map measure from the same
  camera, `lodCamera`), culling and the shadow cascades. While the debug camera is on, the view renders from it (and picks, drags gizmos
  and places labels through it; the lighting picks each pixel's shadow cascade by its depth from the shadow camera, not the one rendered
  from), and each of the three can be switched to it: `debug-camera lod|cull|shadows main|debug` (`lodFromDebug`,
  `cullFromDebug`, `shadowsFromDebug`, the Camera window's switches), all main by default. Turned on, it is placed 15 m behind and 10 m above the
  main camera, looking at it. The keys and mouse move it; C (`debug-camera control main|debug`, `controlMain`) hands them to the main
  camera while the view stays. The main camera is drawn in it: its frustum to the shadow distance, a box at its eye with its forward axis,
  and its trail, a point every 2 m it moved (the last 500; `debug-camera trail on|off|clear`). `camera <pose> debug` and
  `view.camera.set/get {camera: "debug"}` place and read it; the `status` record's `debugCamera` holds its state. The corner axis gizmo still
  shows and turns the main camera.
- **Camera modes** (`ground.*`, the Camera window's mode section, the View menu): for the camera that has the controls.
  - *Orbit*: while Left Alt is held, right-drag turns around a pivot (yaw around the up axis through it, pitch around the camera's right,
    the distance kept, `Camera::orbit`) and the wheel moves toward it (`Camera::dolly`); WASD carry the pivot along. The pivot is the
    surface under the cursor when Alt went down: a depth pick (`ViewPicking::requestDepth`, the `depth` texel after the geometry pass)
    turned into a world point a few frames later, and until then (or over the sky) the point at the last pivot's distance along the cursor
    ray. `camera-orbit x,y,z yaw pitch [debug]` and `view.camera.orbit` turn a camera by angles.
  - *Following the ground* (G, `camera-follow on|off|height <m>`, `view.camera.follow`): the eye kept a height (1.8 m by default) above the
    highest drawn surface under it, set each frame with no smoothing; WASD move across the ground by yaw, Q/E change the height. The ground
    is the render geometry the main thread holds: a ray down from the height above the eye (so ground that rose by up to the height since
    the last frame is found) through the draws whose bounds hold the camera across the up axis, the chunks of 4,096 draws skipped by the
    bounds `SceneBoundsCache` keeps for the shadow cascades, and every triangle of the draws left, brute force (`groundBelow`); the meshes
    are kept from `SceneChanges` as they pass to the renderer (`GroundMeshes`). Ground cover and roofs lift the camera, and every level of a
    render-LOD group counts; with nothing drawn under the camera it holds its height. `fly` suspends it for the main camera, orbiting for
    either. Ground from collision geometry waits for physics. The `status` record's `cameraModes` has the hit, its prim and what was tested
    (chunks, draws, triangles, milliseconds); the overlay draws a line from the eye to the hit and the orbit's pivot.
- **Switching levels**: a root draws the finest loaded level, not finer than the one it selects, whose placement waits for nothing: linked,
  which the composer does only once it is complete, and every mesh, material pack and texture its prims want (under their own purpose)
  held or failed, through the instances and point instancers in it; a LOD root inside it counts by its coarsest level. Each placement
  counts what it waits for by kind (`PlacementWait`) as it is built, summed up the tree, so nothing walks a level to judge it. Until then
  the root keeps drawing the coarser level, and with no loaded level at or coarser than the selected one it draws nothing (a hole, counted),
  never a finer level. A root's level placements are synced before it picks, so it switches in the pass the last
  piece arrives, and a district appears whole, with its materials, instead of model by model. `LevelsSwitched` traces the roots whose drawn
  level changed, once per pass; the `streaming` record's `waiting` and the LOD window's tooltip on a loading row count what the wanted level
  waits for (meshes, materials and textures not held, levels still composing and so not linked). GPU uploads aren't part of it: a switched
  level's new meshes are uploaded over the next frames within the upload budget.
- **Unloading**: a level taken out of its root's resident levels (past its unload radius, or its root no longer placed) is let go at once,
  and everything follows: its placement is released, the composer releases the contents no level reaches any more, the library entries no
  placement uses leave the libraries and the renderer, and a mesh's pack goes once no placement names it. While resident and not drawn, a
  level is placed undrawn and holds its library and GPU entries, so switching to it draws at once. Layer packs and material and texture packs
  stay held. `LevelsUnloaded` traces each batch that let something go (levels, contents, mesh packs), and the `streaming` and `scenethread`
  records, the LOD window (a loaded level's tooltip: the resident levels and why), the Scene Flow window and the Engine Map show what was
  unloaded.
- **The LOD tool** (`lodtool.*`, the LOD window `src/ui/lodwindow.*`): one place for LOD inspection, switching and tuning, both kinds.
  - *The list*: every streaming root drawn and every render-LOD group placed (`buildLodRows` per snapshot: the roots' rows each time, the groups'
    only when the draws, the tuning, the hand-set levels, the preference or the heuristic's state changed; a group's wanted, drawn, fade
    and measure from the readback each frame while the window or a layer is shown, so the frames read every group's state back then: 16
    bytes a group). Filters by path, kind, under the selection, not drawing what it wants, fading. A level picker per row (`lod`,
    `lod-reset` on a group's path force its level), "Set shown", click selects, double-click frames.
  - *The inspector* (`inspectLodRoot`, `view.lod.inspect {root}`, the `lodtool` record for the root at or above the selection): kind,
    authored root, state and why; every heuristic it authors, used or why not, its thresholds authored and in force, its measure from the
    camera now and the level it would pick under the tuning; every level's triangles and draws (a group's from its slots; a streaming
    root's drawn level only).
  - *Tuning*, view-local opinions kept until the view closes, never saved (`LodTuning`, a `TuningRequest`): thresholds and blend thresholds
    per heuristic of an authored root (`view.lod.override`, `lod-thresholds <root> [h=<i>] <t>…`, `lod-blend-thresholds`; reset with
    `view.lod.override.reset`, `lod-override-reset [<root>]`), keyed by `lodRootKey` (the content the root is authored in and its path
    there: every instance of an instanceable asset shares one; a root reached through a plain reference is its own); a bias (`lod-bias`:
    every root picks as if that many times closer, applied to the thresholds, a distance's times it, a screen size's over its square); the
    hysteresis (`lod-hysteresis`, the GPU pick's push constant); blending (`lod-blending on|off`, off drops every band). All in
    `view.lod.tuning`, the window's header and fields. `tunedLodHeuristic` is what the CPU pick, the groups' records (`lodGroupOf`) and the
    records use. `LodTuningChanged` traces a change; `LodTuningRefused` a path with no root, a heuristic not there, thresholds out of order.
    The `lod`, `streaming`, `renderlod` and `lodtool` records carry the tuning; rows say `tuned`.
  - *Layers* (`view.lod.visualize`, `lod-show labels|gizmo|gizmo-all|flash on|off`, the window): the LOD Level view (`view lodlevel`: at
    half strength over the lit image, the drawn level of the nearest LOD root, or with `lod-show color depth` (`view.lod.visualize {color}`)
    how many LOD roots deep it is, since a level's index means a different resolution at each depth (a region's `medium` and a tile's
    `simplified` are both 5 m terrain); the window's swatches and labels follow, the gizmo and the inspector's levels stay by level; a render-LOD group at a forced level striped, one drawing another level than it wants checkered); labels over
    the roots (name, drawn level, fade, measure; a click selects); the heuristic gizmo of the selected or every shown root (`DebugDraw`: the
    distance's center or box, a ring per threshold at the distance it is reached, a screen size's from the view's window, in the colour of
    the level beyond it, a faint ring where a band ends); the pop flash (a group whose drawn level changed without a fade flashes white
    for half a second, over any view: `src/renderer/README.md`, "Render LOD"; the `renderlod` record's `popped`, `flashing`, `popsSeen`).
    A streaming root shows its level, label and gizmo; its flags and flash are render LOD's only, as its state is the CPU's.
- **The Streaming tool** (`streamingtool.*`, the Streaming window `src/ui/streamingwindow.*`, `window streaming`): residency seen and
  steered; it adds no rule, it shows the `streaming` record's data and sets the tuning.
  - *Rows* (`buildStreamingRows`, once per snapshot while the window, a layer or the streaming view is on): every streaming root drawn, its
    selected, drawn and resident levels, state, streaming measure and radii in force (metres for a distance), its state for the colours
    (`StreamingRootState`, worst first: a hole, late, refused by a budget, a resident level still requested, a finer level prefetched,
    drawn as selected; findings marked apart), and its streaming bounds in world metres.
  - *The window*: an overview (the measured totals, the legend, a strip chart of the last 600 frames: assets in flight, roots late, holes),
    the measurement controls (reset counters, fly a camera path at a speed, stop), the layers, and two tabs: the roots (filters: path,
    late, holes, prefetching, findings, under the selection; click selects, double-click frames) with the inspector of the selected root,
    and the map (the roots from above in their state's colour, finer over coarser, the camera and where it looks; hovering a root draws its
    next boundary's load and unload rings around its bounds; drag pans, the wheel zooms, a click selects).
  - *The inspector* (`inspectStreamingRoot`, `view.streaming.inspect {root}`, the `streamingtool` record for the root at or above the
    selection): its residency and why (`stateWhy`: which radius the camera is inside or past, or the previous state holding it finer), every
    heuristic it names by domain with its thresholds authored and in force and its measure, its levels (requested, loaded, ready, drawn,
    lead), its findings, its parent and children; its load and unload radii editable (`lod-thresholds <root> h=<i> …` and
    `lod-unload-thresholds <root> [h=<i>] …`, `view.lod.override {thresholds, unloadThresholds}`: the unload radii alone name the streaming
    heuristic; refused unless one per load radius, in order, each at or past it).
  - *Layers* (`view.streaming.visualize`, `streaming-show bounds|rings|rings-all|labels on|off`, the window): each root's bounds in its
    state's colour (yellow with findings); the selected or every shown root's load (solid) and unload (faint) radii per boundary, its bounds
    grown by the radius in the plane across the up axis; labels under the windows (name, drawn / selected, state, its name; a click
    selects); and the streaming view (`debugview streaming`): every draw in its streaming root's state's colour at half strength over the lit image, a root with findings
    striped yellow, grey under none. Each draw carries its streaming root's slot (`RenderMeshInstance::streamRoot`, taken by the root while
    it is drawn, `streamRootSlot`); the main thread gives the renderer each slot's state while the view is on (`src/renderer/README.md`,
    the debug views).
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
- `ContentComposed`, per prototype or level content the composer completes (the composes it took);
- `PlacementsChanged`, per pass that changed something (placements built, slots added, removed and updated, time);
- `ResidencyChanged`, `LevelsSwitched` and `LevelsUnloaded`, as streaming roots change the level they select and the levels resident, the
  level they draw, and let levels go;
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
deselects, R selects the parent, F frames the selection; the Edit menu has the same, and picks the tool (select, translate, rotate, scale).

The window and view toggles have keys too (`ViewUI::toggleShortcuts`), each pressed again to turn it off. Ctrl+D turns the debug camera on and
off. They go through ImGui's shortcut routing, so they work with the cursor over a window, never while typing in a field, and a focused window's own chord wins (the Assets
window's Ctrl+A selects all there). Ctrl+P, Ctrl+C, Ctrl+S, Ctrl+L and Ctrl+A open and close the Performance, Camera, Streaming, LOD and
Assets windows; 1 to 5 the wireframe, triangle size, overdraw, instance and streaming debug views; Ctrl+1 and Ctrl+2 the buffer and shadow
overlays; Ctrl+3 and Ctrl+4 the mip level and LOD level views; Ctrl+5 turns every view and overlay off for the plain lit image, and pressed
again restores them. The menus show each key. The camera's keys are in "Camera modes" and "The debug camera" (G, C, Left Alt).

A gizmo drag is the view's own opinion on the prim's local transform (`setLocalTransform`), kept until the view closes and never saved: the
placements holding the prim are built again in the next pass, walking it afresh with the opinion, and what is drawn under it moves with them;
every later pass applies it too. The `translate` verb and `view.translate` offset a local position the same way; `view.transform` answers a
drawn prim's local and world transforms. Editing the scene for real is the editor's (`src/editor/`).
