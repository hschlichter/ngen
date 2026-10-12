# Scene pack

**Status. Draft.**

Step 2 of [plan_tool_architecture.md](plan_tool_architecture.md), the scene half: ngen-view stops reading USD. A USD packer turns every USD file
into a pack of engine-ready data, one pack job per file; a runtime scene library composes the packs into the scene the renderer draws; and the
view renders from it, with no USD stage and no OpenUSD linked. It takes the requirements of [plan_streaming_lod.md](plan_streaming_lod.md)
(decision 5), so the packs carry what streaming will need, but streaming itself is later plans.

## Current state

- **The view is the USD stage.** `USDScene` (`src/scene/usdscene.*`) composes the stage, keeps a record per prim, reads transforms, lights and
  materials, and builds meshes (`updateAssetBindings`); `USDRenderExtractor` turns it into `RenderWorld`, `MeshLibrary` and `MaterialLibrary`
  (`src/renderer/renderworld.h`, `src/scene/mesh.h`, `src/scene/material.h`); `Renderer::uploadRenderWorld` consumes them. The renderer's
  inputs have no USD in them. Editing (`SceneUpdater`, `UndoStack`, `src/ui/editcommand.h`) and the editor windows (scene tree, properties,
  layers, undo, asset browser, the File and Edit menus in `mainmenubar`) work on `USDScene` and `PrimHandle`, hosted by `EditorUI`
  (`src/ui/editorui.*`), which also hosts the introspection windows; so do picking (`SceneQuerySystem`, `src/scene/scenequery.*`), the gizmos
  (`src/apps/view.cpp`, through `SceneUpdater`'s preview edits) and the script verbs and RPC methods that name prims (`select`, `translate`,
  `camera-frame`, the `scene` record; `src/view/viewcommands.cpp`).
- **What the view takes from USD today**: materials through `ComputeBoundMaterial` (inherited bindings, per-face `GeomSubset` bindings, which
  Sponza uses); a prim without a material gets its `primvars:displayColor` folded into a material (three_cubes' cubes); a fallback distant light
  `/__DefaultEngineLight` authored into the session layer when the stage has none (intensity 1, colour (1, 0.95, 0.88), angle 0.53, pointing
  along (1, 2, 1) normalised in a Y-up scene, (1, 1, 2) in a Z-up one); every prim is drawn whatever its `purpose`.
- **USD files reach the view by copy.** The `copy` rule in `pack.cpp` sends `.usda`, `.usdc` and `.usd` files to the view unchanged; USD reads them
  through `NgenAssetResolver` (`src/scene/usdassetresolver.*`), which turns asset paths into ids (`assetid.h`: anchored to the layer, `\` read as
  a separator for Sponza's `@textures\brick.png@`, nothing above the server's directory). Textures are packed (`ngen-packer-texture`, `NGTX`).
- **The packer contract is one job, one output** (`src/asset/pack/packer.h`, `src/asset/server/packjobs.*`): a packer reads `--source`, writes
  `--out` and a depfile.
- **Start-up time is USD's.** Sponza: 2.6 s opening the stage, 1.3 s building meshes and materials, 1.0 s uploading, 5.4 s to the first frame
  (the start-up timeline in `src/trace/README.md`).
- **The test scenes**: `assets/three_cubes.usda` (one layer, no arcs, `displayColor` instead of materials), Sponza
  (`assets/main_sponza/NewSponza_Main_USD_Zup_003.usda`: one 437 MB layer whose 98 arcs are all internal references, `references = </path>`;
  textures beside it), and Caldera converted to UsdLod (`~/Code/caldera`, branch `usdlod`: 63,700 layers, references, payloads, instancing,
  nested UsdLod roots, every LOD level a payload; 12% of its prims have `guide` purpose, its clutter `render` purpose; `Points` prims for player
  data). Only Caldera composes across files.
- **Measured on Caldera** ([plan_streaming_lod.md](plan_streaming_lod.md), Current state): 40,702 unique payload files behind 2.85 M
  placements; across 4,046,238 mesh prims (every LOD root at full detail, each prototype once), no mesh has a geometry attribute authored in
  more than one layer or outside the file that defines it.

## Scope

**In**
- **The USD packer**, `ngen-packer-usd`: one job per USD file, writing the file's layer pack and the mesh and material assets it defines.
- **The pack formats**: hand-written binary with a versioned header, and `ngen-packdump` to print any pack as JSON.
- **The packer contract and the asset server** take several outputs per job.
- **The runtime scene library**, `src/world/`: composes layer packs into a `World` (entities, components, the transform hierarchy) that replaces
  `RenderWorld` as the renderer's input.
- **ngen-view renders from packs**, links no OpenUSD, and keeps selection, picking and gizmos on entities; the USD editor code leaves it.
- A **layered test scene** that exercises what three_cubes and Sponza don't.

**Out**
- **Editing USD** until `ngen-editor` exists ([plan_editor_split.md](plan_editor_split.md)): no scene tree, properties, layers, undo or saving in
  the view. A gizmo edit is a local opinion that lasts until the view closes.
- **Variant sets** (a later phase, with a test scene of their own); **inherits and specializes from a class in another file**, **relocates** and
  **collection-based material bindings**: the packer reports them.
- **Streaming**: the packs carry what it needs (payload placements with bounds, LOD roots), and the view applies the simplest form of its rule
  (decision 17): every payload is wanted, except those under a LOD root's levels other than its `lod:default:index`. Streaming is
  [plan_streaming_lod.md](plan_streaming_lod.md)'s steps 2–5.
- Deltas and the manifest (the editor split); texture mip levels as separate parts (texture streaming).
- Cameras, `Points` and other prims the renderer doesn't draw yet: they are entities in the `World`, nothing draws them.

## Decisions

Locked in discussion (2026-10-11 to 2026-10-12):
1. **Layer packs and sub-packs; variant sets later.** Every USD file packs into a layer pack of its own opinions, and the view composes them:
   the layer stack by strength (per attribute, the strongest layer wins), references and payloads by mapping the referenced pack under the
   placement, instancing by sharing one composed pack per referenced file. Variant sets follow in a later phase with a test scene.
2. **One pack job per USD file, with discovery.** A job reads one file's opinions with `Sdf` (no stage, no composition), and its pack names the
   files it references (sublayers, references, payloads, textures), which become jobs of their own when requested. Jobs are self-contained
   (sources and rule in; packs and depfile out), so they can later run on other machines. A scene authored as one large file is one job: that
   is the author's choice.
3. **Layer packs hold opinions per attribute, for the attributes the engine uses**: the transform operations and their order, `visibility`,
   `purpose`, `active`, `instanceable`, material bindings (relationships, with `bindMaterialAs`), light attributes, references and payloads (as
   placements, internal ones included), `extentsHint`, the UsdLod root's attributes and heuristics. The view resolves each attribute and
   evaluates it: the local transform from its operations (`translate`, `rotateX`/`Y`/`Z`, `rotateXYZ` and the other orders, `orient`, `scale`,
   `transform`, inverted ops for pivots, `!resetXformStack!`), visibility inherited down the hierarchy, a material binding inherited from the
   nearest bound ancestor and overridden by a stronger binding.
4. **Geometry and materials are assets, packed from where they are defined.** A mesh is packed from the prim spec that authors its geometry,
   with its `GeomSubset`s and their bindings, and its `primvars:displayColor` as the fallback colour when no material is bound; a material from
   the material prim's shading network (base colour factor and texture, as today). The layer pack's opinion names the asset. An `over` that
   authors geometry attributes is reported as a warning by the job of the file that authors it, and ignored (none in Caldera's 4 M meshes).
5. **The view composes in phase 1**: the layer stack, references and payloads (to other files and internal) with namespace mapping and
   instancing, relationship targets mapped through them (material bindings), `over`s from stronger layers onto prims inside referenced content
   (not inside instances, where USD ignores them too), and UsdLod roots. Anything else the packer meets (variant sets, inherits or specializes
   across files, relocates, collection-based bindings) is a warning naming the prim, never silently wrong. So is a payload without
   `extentsHint` on it or the model above it in the same file, unless it is a LOD level, which its root's heuristic bounds
   ([plan_streaming_lod.md](plan_streaming_lod.md) decision 6).
6. **The view links no OpenUSD.** No USD editing until `ngen-editor`; selection, picking and gizmos stay, on entities. This moves "ngen-view
   without pxr" from step 3 of the umbrella plan to this step.
7. **Hand-written binary formats**, with a versioned header, as the packed texture (`NGTX`) has; no serialization library.

Locked 2026-10-12:

8. **The view requests what a pack names.** References and textures when the pack that names them is loaded; payloads when wanted (decision 17
   here; by priority once streaming lands, [plan_streaming_lod.md](plan_streaming_lod.md) decision 7). The asset server doesn't chain jobs on its
   own: parallelism comes from the many requests. Eager packing of everything under an id is deferred, as an explicit request.
9. **A job writes several outputs.** One file defines many meshes and materials (Sponza's single file defines hundreds), and reading a 437 MB file
   once per mesh is not an option. The job for `a.usd` writes the layer pack, id `a.usd`, and an asset per mesh and material it defines, ids
   `a.usd#/path/to/prim` (USD's own convention for a prim in a file). Requesting any of them runs the file's job if its outputs are stale; the
   server caches them as one group with one depfile, each output with its own version.
10. **Engine defaults are the engine's, not the pack's.** The view adds the fallback distant light when the composed scene has no distant light,
    with today's values exactly (Current state), so three_cubes and Sponza look the same; and uses Y as the up axis when the root layer authors
    none, USD's own fallback. Both depend on the whole scene, which no single job sees.
11. **One pack kind per file**: a referenced file's pack and a sublayer's pack are the same layer pack; "sub-pack" is a layer pack used through a
    reference or payload. One reader, one format.
12. **Names**: the packer in `src/asset/pack/usd/` and `src/apps/packerusd.cpp` (`ngen-packer-usd`), as the umbrella plan says; the runtime scene
    library in `src/world/` with `World` as its type, keeping "scene" for the USD side that moves to the editor.
13. **Picking on the GPU**, as everything else in the renderer is: the entity under the cursor is read back from the GPU (the renderer already
    writes per-instance ids for the `instance` debug view), not a CPU structure over the `World`. The CPU picking (`SceneQuerySystem`,
    `SpatialIndex`, `BoundsCache`) leaves the view with the editor code.
14. **The editor code moves to `src/editor/`, untouched**, until `ngen-editor` is built from it: `USDScene` and the rest of `src/scene/`
    (`usd*`, `sceneupdater`, `undostack`, `scenequery`, `spatialindex`, `boundscache`; `primshapemesh` goes to the packer instead), and the editor
    windows from `src/ui/` (scene tree, properties, layers, undo, tools, asset browser, `editcommand`). It isn't built into anything meanwhile.
15. **Each scene keeps its own up axis and units.** The root layer's `upAxis` and `metersPerUnit` go into its pack as scene metadata, and the
    view adapts (the camera's up, as today; units for physics later). Nothing is converted, as in USD, which doesn't rescale referenced files
    either: keeping them consistent is the author's. three_cubes is Y-up in metres, Sponza Z-up in centimetres, Caldera Z-up in inches.
    Rejected: one engine frame the packer converts everything into, which would rewrite transforms and geometry in per-file jobs from metadata
    only the root layer has.
16. **Purpose: the end result by default, guides on request.** Every prim's `purpose` is kept. The renderer draws `default` and `render`;
    `guide` and `proxy` are drawn when their display toggle is on (an overlay setting, like the grid and bounds overlays, and a script verb), as
    usdview's display purposes do. Today every prim is drawn; three_cubes and Sponza author no purpose, so they look the same. Caldera's guides
    (annotation boxes, lighting and collision volumes, 12% of its prims) are hidden until asked for.
17. **What this plan loads**, the simplest form of [plan_streaming_lod.md](plan_streaming_lod.md)'s rule (its decisions 1 and 6): a LOD root
    wants its `lod:default:index` level (the coarsest, in converted Caldera), and its coarsest level is always loaded too (the floor); every other
    payload is wanted once its parent is loaded, except those under levels that aren't wanted. A LOD root draws its finest loaded level not finer
    than the wanted one, so a level still loading falls back to a coarser one. The wanted level can be set by hand (a script verb and RPC method,
    and an `ngen-worldcheck` option), to load and check other levels. So Caldera opens as its default map, not at full detail (43,733 files,
    3.4 M placements).
    - **The level is per instance.** A LOD root inside instanced content (every Caldera model) exists once in the shared composed pack, and its
      wanted and drawn level is state of each instance, kept outside the shared pack: setting a level names the instance's path. Nothing about
      levels is baked into what instances share ([plan_streaming_lod.md](plan_streaming_lod.md) decision 5).
18. **Every prim is an entity, and instances aren't expanded.** The `World` keeps every composed prim, whatever its type or purpose (scopes,
    `GeomSubset`s, guides, cameras, `Points`), so its fingerprint compares with USD's prim for prim; what is drawn is a separate decision (16).
    Instanced content is kept once per shared pack, with per-instance state (transform, wanted level, local opinions) beside it; it is expanded
    only when a fingerprint is printed. Stored expanded, Caldera at full detail would be tens of millions of entities, against streaming's
    memory budgets.
19. **The USD side of the comparison is a C++ tool in ngen**, `ngen-usdcheck`, linking OpenUSD as `ngen-packer-usd` does: it prints USD's
    fingerprint of any scene in the same format as `ngen-worldcheck`, so every check runs from this repository and is built by `ngen-build`.
    The Caldera fork's `verify.py` stays for verifying the conversion itself. Rejected: using the fork's Python script for ngen's scenes too,
    which would make ngen's verification depend on another repository and its Python environment.

## Steps

### Phase A: packs

Ships: every test scene packs, and every pack can be read back and printed.

1. **The pack format** (`src/asset/pack/scenepack.h`, `scenepack.cpp`): a 32-byte header (`NGPK`, kind, format version, section count),
   a string table (paths, names, asset ids) and sections, little-endian, every count and offset 64-bit. Kinds: layer pack, mesh, material.
   - **Layer pack**: the layer's `upAxis` and `metersPerUnit` (used from the root layer only, decision 15); sublayer ids in strength order; prim
     specs, each with its path, specifier (`def`, `over`, `class`), type, and the opinions of decision 3 as typed fields; placements (reference or
     payload, to another file or internal: asset id or none, target prim path, instanceable); for prims that define geometry or a material, the
     asset id; the warnings the packer raised (decisions 4 and 5).
   - **Mesh**: what `MeshDesc` holds today (`Vertex` array, `uint32` indices, submeshes with their material binding by path), the fallback
     colour from `displayColor`, local bounds.
   - **Material**: what `MaterialDesc` holds except pixels: base colour factor, base colour texture id.
   - Reader and writer in the same files; a format version change makes every old pack stale (the rule's version bumps too).

   ```cpp
   struct ScenePackHeader {
       std::array<char, 4> magic; // "NGPK"
       uint32_t kind;             // ScenePackKind
       uint32_t formatVersion;
       uint32_t sectionCount;
       uint64_t stringTableOffset;
       uint64_t sectionTableOffset;
   };
   ```
2. **The packer contract takes several outputs** (`src/asset/pack/packer.h`, `src/asset/server/packjobs.*`, `assetcache.*`): `--out` stays the
   primary output; a job also writes `<out>.outputs`, one line per secondary output (`<id>\t<file>`). The cache records them under the
   primary's job; a request for a secondary id maps to its primary's job (`<file>#<prim>` → `<file>`). `ngen-packer-texture` and
   `ngen-packer-shader` are unchanged (no secondary outputs).
3. **`ngen-packer-usd`** (`src/apps/packerusd.cpp`, logic in `src/asset/pack/usd/`, linking OpenUSD; grown out of `USDScene`'s loading half and
   `USDRenderExtractor`): opens `--source` as an `Sdf` layer, walks its prim specs, writes the layer pack, and builds each mesh (triangulation,
   flattened primvars, subsets, `displayColor`, the shape schemas through `primshapemesh`) and material it defines. Asset paths become ids by
   `assetid.h`'s rules, as `NgenAssetResolver` does today (anchored to the layer, `\` as a separator, nothing above the directory). Depfile: the
   source only. Warnings (decisions 4 and 5, payloads without bounds among them) go to stderr and into the pack.
4. **`pack.cpp`**: a `usd` rule for `.usda`, `.usdc`, `.usd` with `ngen-packer-usd`; the `copy` rule keeps `.hdr`.
5. **`ngen-packdump`** (`src/apps/packdump.cpp`): prints a pack file, or a packed asset fetched from the server by id, as JSON.
6. **The layered test scene**, `assets/layered/`: `root.usda` with two sublayers (`override.usda` over `base.usda`); `base.usda` places
   `model.usda` twice by reference (one instanceable) and `building.usda` by payload, has an internal reference, a material bound on a parent
   and a stronger one on a child, an invisible parent, and a `guide` prim; `override.usda` overrides a transform and a visibility; a UsdLod root
   with two payload levels; a material with a texture; and one inherit from another file, to see its warning.

### Phase B: the runtime scene

Ships: a `World` composed from packs that matches USD's composition, checked outside the view.

7. **`src/world/`** (no OpenUSD): `World` holds an entity per composed prim (decision 18: stable id from the prim path, the path as a debug name,
   type, purpose, parent, children) and components (local transform, mesh asset id with its submeshes and bound materials, light, visibility,
   LOD root with levels and heuristics, placement with bounds); world transforms and visibility resolved down the hierarchy. `WorldComposer`
   composes it from layer packs: the root layer stack by strength per attribute; placements, internal ones included, mapped under their prim,
   with relationship targets mapped through them; one composed pack per instanced file, shared by its placements; `over`s into referenced
   content outside instances; material bindings resolved (decision 3); each LOD root's wanted, loaded and drawn level (decision 17). Instanced
   content is composed once per shared pack, with per-instance state beside it (decision 18).

   ```cpp
   class WorldComposer {
   public:
       // A pack has arrived; returns the asset ids it names that the world now needs.
       auto addPack(const std::string& assetId, std::span<const std::byte> bytes) -> std::vector<std::string>;
       // Which level a LOD root wants; lod:default:index until set. rootPath may lie inside an instance: the level is that instance's.
       // Returns the asset ids the change needs; the coarsest level stays loaded.
       auto setLodLevel(const std::string& rootPath, uint32_t level) -> std::vector<std::string>;
       auto world() const -> const World&;
   };
   ```
8. **`ngen-worldcheck`** (`src/apps/worldcheck.cpp`): loads a scene's packs from the asset server, composes a `World`, and prints its fingerprint
   (entity path, type, point count per mesh, sorted, instances expanded), the format the Caldera fork's `verify.py` uses. `--lod <root>=<level>`
   sets a LOD root's level before composing; `--instancing` prints instance and prototype counts.
   **`ngen-usdcheck`** (`src/apps/usdcheck.cpp`, linking OpenUSD; decision 19): opens the scene with OpenUSD, applies the same load rules (the
   coarsest level of every LOD root but those `--lod` sets), and prints USD's fingerprint and, with `--instancing`, its counts. The two outputs
   are compared as files.

### Phase C: the view renders from packs

Ships: ngen-view renders the test scenes from packs, with no OpenUSD, and is looked at but not interacted with.

9. **The renderer takes the `World`**: `Renderer::uploadRenderWorld` becomes `uploadWorld(const World&, const WorldAssets&)`, with meshes,
   materials and textures from their packed assets (`WorldAssets`, in `src/world/`, keyed by asset id); `RenderWorld`, `MeshLibrary` and
   `MaterialLibrary` go once nothing uses them. The renderer draws by purpose (decision 16).
10. **The view loads packs**: it requests the scene's id, feeds arriving packs to `WorldComposer` and requests what each names (decision 8),
    textures included; applies the engine defaults (decisions 10 and 15); trace events for each step of the start-up timeline (scene
    requested, packs composed with counts and time, world ready).
11. **What reads the scene reads the `World`**: the `scene` and `status` records (the layer stack from the root pack, entity counts),
    `camera-frame` by entity path, the scene bounds for the initial camera; a `purposes` overlay setting and script verb (decision 16); a
    `lod <root> <level>` script verb and `view.lod.set` method (decision 17).
12. **The view's own UI host**: `EditorUI` hosts both the editor windows and the introspection windows today, and its `draw` takes `USDScene`.
    A `ViewUI` (`src/ui/viewui.*`) takes over what stays in the view (the introspection windows, overlays, camera window, status bar, the View
    menu); `EditorUI` goes with the editor code (decision 14).

### Phase D: selection, picking and gizmos; the editor leaves

Ships: the view is interactive again on entities, and nothing of the editor or OpenUSD is built into it.

13. **GPU picking** (decision 13): the instance id under the cursor is copied from an id target after the geometry pass (1×1, read back the
    frame after a click) and mapped to its entity.
14. **Selection and gizmos on entities**: the selection is an entity id; gizmo drags are local opinions on the entity's transform, kept until
    the view closes; `select` and `translate` script verbs and methods name entities by path; a `pick <x> <y>` script verb and `view.pick`
    method pick at a pixel as a click does, and answer with the entity's path.
15. **The editor code moves** to `src/editor/` with `git mv` (decision 14), unchanged and unbuilt; `build.cpp` stops building `sceneusd`,
    `scene` and the editor windows, and links no OpenUSD into ngen-view. A `src/editor/README.md` says what the code is and that `ngen-editor`
    will be built from it; `AGENTS.md`'s folder list gains `src/editor/`.
16. **Docs**: `src/world/README.md`; `src/asset/README.md` (the USD packer, the contract's secondary outputs, the formats); `src/renderer/README.md`
    (its input, purposes, GPU picking); `src/editor/README.md`; the umbrella plan's steps 2 and 3 (no pxr in the view moves here); the
    run-headless skill.

## Verification

- **Packing is per file**: packing `assets/layered/` runs one `ngen-packer-usd` job per USD file (the asset server's trace and `cache` record);
  three_cubes and Sponza run one job each; a second request runs no job (`packed: false` on every `asset.ready`).
- **Warnings, not silence**: the layered scene's cross-file inherit is a warning in its pack and in the server's trace, naming the prim.
- **The same scene as USD**: `ngen-worldcheck`'s fingerprint equals `ngen-usdcheck`'s, byte for byte, every prim included (decision 18), for
  `assets/layered/`, three_cubes, Sponza (its internal references), Caldera's default map (19,156 meshes, 21,937,137 points, as the USD
  fingerprint counts them) and `map_capital` at its full level (`--lod`; 596,867 meshes, 141,852,999 points, instances expanded).
- **The same instancing as USD, or explained**: `ngen-worldcheck --instancing` against `ngen-usdcheck --instancing` for `map_capital` at full
  (every instance at the same level, as USD splits prototypes by what is loaded in them; USD's are 139,137 instances and 2,675 prototypes,
  [plan_caldera_usdlod.md](plan_caldera_usdlod.md)); any difference listed with its cause (the view shares one composed pack per instanced
  file, USD one prototype per identical composition).
- **The same pictures**: three_cubes and Sponza screenshots from the packed view (end of phase C) are byte-identical to today's (baselines
  captured before phase C); the `render` record has the same meshes, textures and draw counts.
- **Levels**: setting a model root inside one instance of `map_capital` to its finest level changes that instance only (its draws in the
  `render` record), and the coarser level draws while the finer one loads.
- **Purposes**: Caldera's default map draws no `guide` prim (the `render` record's draw log names none), and does with the `purposes` overlay on.
- **No USD in the view** (end of phase D): `ldd` and `nm` on ngen-view show no `libusd_*` or `pxr` symbols; `build.cpp`'s link graph has none
  under the view.
- **Start-up without USD**: Sponza's start-up trace has no USD stage events (`SceneOpened`, `AssetBindingsUpdated`, `SceneExtracted`) and, with a
  warm cache, no packer run; the time to first frame is recorded next to today's 5.4 s.
- **Gizmos and picking** (end of phase D): a `pick` at a pixel selects the entity under it, read back from the GPU (its path in the
  `status` record), and a `translate` script verb moves it, visible in a screenshot.

## Open questions

None left; the decisions are locked (2026-10-12).

## Deferred / follow-ups

- **Variant sets**: packed as alternative opinions, switched in the view. Trigger: a test scene with a variant set, or a project that uses them.
- **Inherits, specializes, relocates and collection-based bindings across files.** Trigger: a scene that reports them.
- **Eager packing** of everything a scene names, as an explicit request. Trigger: warming a cache for a demo or CI.
- **Remote packer machines.** Trigger: packing time on one machine is the bottleneck.
- **Drawing `Points` and other prim types.** Trigger: a scene that needs them shown (Caldera's player data).
- **Splitting one large file into several jobs.** Not planned: a large file is the author's choice (decision 2).
- **[plan_editor_split.md](plan_editor_split.md) needs revising** when the editor split is picked up: it assumes the editor still runs inside the
  view during its phase A, which this plan ends (decision 6).
