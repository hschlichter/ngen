# GPU scene inspector

**Status. Landed.**

Stage 4 of [plan_introspection.md](plan_introspection.md). It builds on the capture service ([plan_frame_capture.md](plan_frame_capture.md)).

## Current state

The GPU scene tables (instance records, mesh table, material table, texture slots) exist only on the GPU. The Capture window can show them as raw
typed rows, but nothing joins them to prims or to each other. Culling is visible only as counts and a camera-only red/green AABB overlay:
- there's no view of what each cascade culled
- there's no reason why an instance was culled
- the compaction step that turns visibility into ordered commands can't be seen at all

## Scope

**In**

- **Culling reasons.** `instancecull.comp` writes, per instance, which frustum plane culled it in each view: one 4-bit code per view in a new
  `cullPlanes` buffer. Codes: 0–5 is the plane (left, right, bottom, top, near, far); `E` is visible; `F` is not tested (culling off, or bounds
  invalid). It becomes a frame-graph resource with a schema.
- **All views in the readback.** The culling readback keeps every view's visibility bit, not only the camera's; `CullResult` carries them.
- **GPU Scene window** (Windows > Introspection):
  - **Instances:** the captured instance buffer decoded and joined with prim path, mesh, material and texture slot, plus visibility and cull reason
    per view. Clicking a row selects the prim.
  - **Meshes:** the mesh table with index ranges and triangle counts.
  - **Materials:** the material table joined with texture slots and texture names.
  - **Culling:** per view, counts and the instances culled with their plane.
  - **Compaction:** per-workgroup counters and offsets per region, and the final command order per region with prims.
- **Viewport:**
  - a "Cascade frusta" overlay drawing each shadow cascade's light frustum in its own colour
  - the Culling window gains a view selector that colours the AABB overlay by the chosen view's visibility (camera or one cascade)
- **Dump and verb:** `dump-gpuscene DIR` captures all the tables and culling buffers into `DIR` (typed JSON). It also writes
  `DIR/instances_joined.json`, one row per instance with prim path, mesh, material, flags, bounds, and visibility and cull plane per view.

**Out**

- Editing the GPU tables from the UI.
- Meshlet and cluster views (no meshlets yet).

## Decisions

Made while planning, following the umbrella's recommendations.

- **Cull reasons are computed by the same shader pass that culls.** A CPU re-computation could disagree with the GPU. Writing the failing plane next
  to the visibility bit shows what the GPU decided. It costs one extra `u32` store per instance.
- **The joined instance table is built on the main thread.** Prim paths live in the USD scene, which is main-thread data. The GPU tables arrive as
  captures and are joined there, for the window and the dump alike.
- **The overlay's view selector reuses the readback.** Colouring by any view needs every view's visibility bit, which the readback now carries. No
  extra capture per frame.

## Steps

1. `instancecull.comp`: plane index from `insideView`; a `cullPlanes` buffer (binding 8); `DrawLists` owns it per slot, imports it and names it; the
   `InstanceCull` pass writes it; `gpuschema` gets `CullPlanes` (hex nibbles per view).
2. `DrawLists::parseReadback` keeps full visibility bits; `CullResult::viewBits`.
3. `GpuSceneWindow` (`src/ui/gpuscenewindow.h/.cpp`) with capture watches for `instances`, `gpuscene.meshtable`, `gpuscene.materials`,
   `cullVisibility`, `cullPlanes`, `cullGroupCounters`, `cullGroupOffsets`, `drawCommands` and `drawCounts`, refreshed on demand or live.
4. Cascade frusta overlay (`EditorUI::drawDebug`, `overlay cascadefrusta=on`); Culling window view selector (`cull view N` verb).
5. `dump-gpuscene DIR` in `main.cpp`: capture dumps plus the joined table.

## Verification

- `dump-gpuscene` on Sponza at the courtyard camera:
  - `instances_joined.json` has 407 rows with prim paths
  - the camera-visible count is 65 among instances with a mesh, and 342 instances have a camera cull reason other than `E`
  - every culled instance names one plane
- Cull reasons agree with visibility: every instance whose visibility bit is off has a plane code 0–5 for that view; every one with the bit on has `E`
  (or `F` with culling off).
- `cull off`: every code is `F`.
- The `translate` script moves a cube 1000 units up. Its camera cull reason changes from `E` to a plane code. The code is the first plane in test
  order (left, right, bottom, top, near, far) that rejects the box, so a box above and behind the eye reports `left`.
- Screenshots unchanged; validation clean.
- UI: the `window gpuscene on` verb opens the window headless; it draws with validation clean.
- UI: the cascade frusta overlay shows three coloured boxes around the view; the AABB overlay recolours when the view selector changes.

## Results

Sponza, courtyard camera, frame 149 (`150 dump-gpuscene DIR`):

- `instances_joined.json`: 407 rows, every one with a prim path; 4 views (camera and 3 cascades).
- Camera: 65 visible (all with a mesh in the pool); 342 culled, `left` 314 and `bottom` 28.
- Cascades: 106, 255 and 379 visible; each culled instance names `left` or `top`.
- Visibility bits and reasons agree for every instance in every view (0 mismatches).
- `cull off`: all 407 × 4 codes are `F` (printed `-`), every instance visible.
- three_cubes, `translate /World/Cube 0,1000,0`: the cube's camera reason goes from `visible` to `left` (see the note in Verification); the others
  stay visible.
- The six screenshots are byte-identical to the baseline; every run is validation clean (`--fail-on-validation`, exit 0).
- The GPU Scene, Frame Debugger, Capture and Memory windows open together through `window <name> on` and draw without errors.
- Not checked: the overlay colours and the cascade frusta overlay by eye. They need the windowed UI.
