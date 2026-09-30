# Editor split and deltas

**Status. Draft.**

Step 3 of [plan_tool_architecture.md](plan_tool_architecture.md). It needs step 2:
- pack rules and packers ([plan_pack_rules.md](plan_pack_rules.md))
- the USD packer, the scene pack format and the runtime scene library: layer packs, sub-packs, variants, components. Its plan isn't written yet.
- the build server ([plan_build_server.md](plan_build_server.md))

It also needs RPC from step 1 ([plan_rpc.md](plan_rpc.md)).

After step 2, ngen-view renders from packs, but the editor is still inside it, using USD in-process. This plan moves the editor out and makes every
change reach the view as a delta.

## Current state

What exists today (after step 2, the scene side has moved to packs, but the editor code below is unchanged):

- **The editor UI lives in ngen-view** (`src/ui/`):
  - the scene tree (`scenewindow`), properties, layers, undo, asset browser and tools windows
  - the main menu's File and Edit menus
  - `EditorUI` holds them together
- **Edits are `SceneEditCommand`s** (`src/ui/editcommand.h`): mute layer, set transform, set visibility, add sublayer, clear session, create prim,
  create reference prim, remove prim, set display colour. `SceneUpdater` applies them to the `USDScene` with undo (`UndoStack`), through a fast path
  for transforms and an asynchronous path for the rest.
- **Gizmos already separate preview from authoring.** `TranslateGizmo`, `RotateGizmo` and `ScaleGizmo` run on ngen-view's main thread
  (`src/main.cpp`). A drag sends `Preview` edits, which update the runtime transforms only; the release sends one `Authoring` edit, with the drag-start
  transform as the undo inverse hint.
- **Picking is CPU-side on USD data.** `SceneQuerySystem::raycast` works over prim bounds (`src/scene/scenequery.*`, `spatialindex.*`,
  `boundscache.*`) and returns a `PrimHandle`. Selection is a `PrimHandle` in `main.cpp`.
- **Only transforms update incrementally on the GPU.** Any other change re-uploads the GPU scene in full (`src/renderer/README.md`, Known gaps).

## Scope

**In**

- **Phase A, deltas in the view** (the editor is still in-process):
  - the view's **manifest**: scene version, layer stack with each layer pack's version, variant selections, sub-packs and assets held
  - **`scene.applyDelta`**: base version, layer pack replacements, layer stack and variant changes, and required sub-packs and assets by id
  - **`scene.manifest`**, and **resync**: on a version mismatch the view reports its manifest, and the producer answers with one catch-up delta
  - **incremental GPU scene updates:**
    - geometry pool allocation and freeing per mesh
    - material table and texture slot patches
    - instance adds, removes and updates
    - transform-only updates, as today
  - the in-view editor's authored edits go editor code → changed layer → build server repack → delta → view, the same path the split editor will
    use
- **Phase B, the split:**
  - **`ngen-editor`**, its own process: the USD stage (`USDScene`), `SceneUpdater`, `UndoStack`, `SceneEditCommand`, and the editor windows (scene
    tree, properties, layers, undo, asset browser, tools, File and Edit menus)
  - **editor methods over RPC**, for agents and other tools:
    - `scene.open`, `scene.new`, `scene.save`, `scene.saveAs`, `scene.reload`
    - `layer.list`, `layer.mute`, `layer.addSublayer`, `layer.setEditTarget`
    - `prim.list`, `prim.get`, `prim.create`, `prim.createReference`, `prim.remove`, `prim.setTransform`, `prim.setVisibility`,
      `prim.setDisplayColor`, `variant.setSelection`
    - `undo.undo`, `undo.redo`, `undo.list`
    - `selection.get`, `selection.set`
  - **interactive edits from the view:** gizmos and picking move onto the runtime scene in ngen-view, and are reported to the editor by entity id
  - **the viewport:** the editor starts an ngen-view for the open scene, as a separate window, and connects to it. Further views can attach
  - **ngen-view links no pxr**, and no USD code at all
- **`ngen-cli editor [scene]`** starts the editor (the tool table gains `{ "editor", "ngen-editor" }`). The editor makes sure a build server is
  running, like `ngen-cli build` does.

**Out**

- Embedding the viewport in the editor window. It's a separate ngen-view window in this step.
- Several editors on one scene, and multi-user semantics.
- Interactive viewport tools beyond transforms (placing prims, painting, material tweaks).
- Geometry pool compaction.
- New USD features in the packer. The editor shows and edits everything USD has; the view shows what the packer maps (the scene pack format plan).

## Decisions

Proposed; pushback welcome.

1. **Unsaved edits stay in memory until `scene.save`** (Henrik). How an unsaved edit reaches packed data in the view is **not decided**; see
   Open questions. Packers stay standalone programs that take arguments and read their sources, with one flow for every job.
2. **Interactive edits follow the existing Preview/Authoring split, across the process boundary.**
   - **During a drag**, the view applies the gizmo's transform as a local opinion at the top of its stack. Every 100 ms it sends
     `prim.setTransform { entity, local, purpose: preview }`, so other views follow; the editor updates its runtime state without writing USD.
   - **On release**, the view sends `prim.setTransform { entity, local, purpose: authoring, inverse: dragStart, editSeq }`. The editor authors it
     into the edit target layer with undo, using the inverse hint as today. It then reaches packed data by the route still to be designed (Open
     questions).
   - **The delta names the edit.** It carries the highest `editSeq` it covers. When the view receives it, it drops its local opinions up to that
     number. The image doesn't change, because the packed value equals the local one.
3. **The editor owns selection.**
   - The view picks on the runtime scene: a new spatial index over entity bounds from packed mesh bounds, replacing `SceneQuerySystem::raycast` on
     the view side.
   - It sends `selection.pick { entity, ray }` to the editor, which maps the entity to a prim and updates the selection.
   - The editor tells every view `view.selection { entities }`, for highlights and the gizmo anchor.
   - Selecting in the scene tree works the same way, without the pick.
4. **Entity ids map back to prims in the editor, not the view.**
   - The packer's stable ids (prim path, plus the submesh index) are parseable, but the editor keeps an explicit map built from the scene manifest,
     so the view never interprets a path.
   - A pick on a submesh entity selects the prim it came from.
5. **The editor and the introspection tool share one tool application shell** (`src/apps/tool/`, [plan_apps_folder.md](plan_apps_folder.md)):
   - an SDL window, the RHI device, the ImGui backend, the RPC endpoint, and the frame loop
   - the editor links it plus USD and the editor windows; the introspection tool (step 4) links it plus its windows
   - no renderer: neither draws a 3D scene
6. **The editor starts its viewport.**
   - `scene.open` makes sure a build server is running, requests the scene's packs, and starts `ngen-view --editor <editor endpoint>`, which
     connects back.
   - Views started by hand can attach with `--editor`, found by discovery when there's one editor.
   - Closing the editor leaves attached views running, showing the last state.
7. **Phase A before Phase B.** Deltas and incremental GPU updates are the risky part. Proving them with the editor still in-process means the split
   in Phase B only moves code and changes transport. Nothing about the data path is new at that point.

## Steps

### Phase A: deltas and incremental updates

1. **Delta and manifest types** in the runtime scene library (no pxr), with serialisation as binary for the RPC attachment slot, and methods
   `scene.applyDelta` and `scene.manifest` on ngen-view.
2. **Versioning and resync:** the view rejects a delta whose base isn't its version, with `resync-needed` and its manifest. The producer's resync
   builds one delta from the manifest diff.
3. **Incremental GPU scene** (`src/renderer/gpuscene.*`):
   - allocations in the geometry pool per mesh, with a free list
   - patches to the material table and texture slots, with the descriptor set rewritten only for changed slots
   - instance buffer edits for added and removed entities, through the existing dirty-span upload path
4. **The build server delta:** on `asset.ready` for a layer pack, the server sends each interested view the delta against the version that view
   reported, built from the old and new layer packs.
5. **The in-view editor through the server:** `SceneUpdater`'s authored edits reach packed data by the route still to be designed (Decision 1, Open
   questions) instead of re-extracting in-process. The view's scene changes only through deltas.

### Phase B: the split

6. **`src/apps/tool/`**, the shared application shell (Decision 5).
7. **`ngen-editor`**: its main is `src/apps/editor.cpp`, and its library is `src/editor/`, a program in `build.cpp`:
   - `USDScene`, `SceneUpdater`, `UndoStack`, `EditCommand`, `BoundsCache` and the editor windows move from `src/scene/` and `src/ui/` into
     `src/editor/`
   - the editor methods (Scope) are registered on its endpoint
8. **ngen-view:**
   - the gizmos move onto runtime-scene entities, with local opinions and `prim.setTransform` (Decision 2)
   - picking uses the runtime-scene spatial index, and `selection.pick` goes to the editor (Decision 3)
   - `--editor <endpoint>` connects to an editor
   - the editor windows and File and Edit menus leave ngen-view
9. **Link graph:** ngen-view no longer links `sceneusd` or pxr. `build.cpp` enforces it: the engine libraries and ngen-view have no path to a pxr
   library.
10. **`ngen-cli editor`**, and the tool table row.
11. **Docs:**
    - `src/editor/README.md`: the process, methods, how edits reach the view, selection and edit sequencing
    - `src/renderer/README.md`: incremental GPU scene, deltas
    - `AGENTS.md` and the run-headless skill: scene edits go through `ngen-cli rpc` to the editor; the view has no scene editing methods

## Verification

**Phase A**
- **Transform:** moving a prim with the in-view gizmo sends one delta on release. The `InstanceUpload` event shows only that entity's records, and
  no `GeometryPoolBuilt` or texture upload.
- **Adding a prim that references an existing mesh** loads no new asset. The delta lists the new entity, and the geometry pool doesn't grow.
- **Removing a prim** frees its pool allocation if nothing else uses the mesh. The Memory window's pool usage drops by the mesh's size.
- **Swapping a texture** in a layer shows the new texture after `asset.ready`, and the old one until then. Only that texture's slot is rewritten.
- **Resync:** a delta forced to the wrong base triggers resync, and the result renders byte-identical to a fresh load of the same state.
- **Parity:** after a scripted sequence of edits, the image is byte-identical to a fresh load of the saved scene.

**Phase B**
- **Split:** with `ngen-editor`, the build server and ngen-view running on three_cubes:
  - a gizmo drag in the view moves the cube every frame with no RPC round trip inside the frame
  - after release, the editor's layer holds the new transform, the delta with that `editSeq` arrives, the local opinion is dropped, and the image
    before and after is byte-identical
  - `scene.save` writes the transform to the layer file on disk
  - `undo.undo` in the editor moves the cube back in the view through a delta
- **Picking:** clicking the cube in the view selects `/World/Cube` in the editor's scene tree. Selecting `/World/Cube_1` in the tree moves the gizmo
  to that cube in the view.
- **Agents:** `ngen-cli rpc call editor prim.setTransform '{…}'` moves a prim in the view, and `ngen-cli rpc call editor scene.save` persists it.
- **Linking:** `nm` and `ldd` on ngen-view show no `libusd_*`, and `build.cpp` fails to link if an engine library adds a pxr dependency.
- **Lifetime:** closing the editor leaves the view running with the last state. Reopening the editor and attaching resyncs to an identical image.

## Gaps

- The viewport is a separate window next to the editor, not embedded.
- One editor per scene. Two views dragging the same prim is last-writer-wins.
- The cost of an authored edit depends on the open question below. Interactive transforms don't pay it, because they're local first.
- No geometry pool compaction; long editing sessions fragment the pool.
- Only transforms are interactive in the view; every other edit goes through the editor.
- USD features the packer doesn't map are editable in the editor but invisible in the view.

## Open questions

- **How modifications reach packed data.** Unsaved edits live only in the editor's memory, packers are standalone programs that read their sources,
  and the build server is reached only through RPC. How an edit becomes a delta in the view under those three rules needs its own design, once the
  architecture is in: steps 1–2, and this plan's split without authored-edit deltas. Two approaches were tried and set aside: working copies of
  layers on disk, and layer content sent with the request and handed to the packer in memory. Interactive transforms aren't affected (Decision 2).

## Deferred / follow-ups

- **Embedded viewport** (shared texture or embedded window). Trigger: the side-by-side window gets in the way of editing.
- **Per-prim layer pack patches** instead of repacking a whole layer per edit. Trigger: property-edit latency on large layers.
- **Geometry pool compaction.** Trigger: pool fragmentation shows up in the Memory window during long sessions.
- **More interactive viewport tools** on the local-opinion path. Trigger: placement or material editing in the viewport.
- **Multi-editor semantics.** Trigger: two people or agents editing one scene at once.
