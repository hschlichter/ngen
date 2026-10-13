# Roadmap

What is planned, open or parked, as of 2026-10-08. One line per item; the linked plans hold the detail. Items marked *parked* were discussed and
set aside on purpose; *trigger* says what would bring an item back. When an item lands it moves to [Landed](#landed) at the end, with its
follow-ups left behind as open items.

## Objective

An engine for developer and AI collaboration.
- **AI builds games on top of the engine's components, without changing the engine.** Engine and game are separate: games are made from
  components, data and gameplay code against stable engine APIs, driven through the same tools a human uses (RPC, editor, `ngen-cli`).
- **Humans and AI edit together in the editor, interactively.** The AI does the broad and bulk work; the human tweaks, steers and decides. Every
  AI edit is an ordinary, undoable editor edit that the human can see, adjust or reject.
- **Everything is async.** A scene opens instantly and its data pops in as it arrives: geometry, textures, shaders, sub-scenes and LODs.

Items below that follow from the objective rather than from an existing plan are marked *from the objective*.

## Direction: the tool architecture

The umbrella is [plan_tool_architecture.md](plan_tool_architecture.md): a USD-free ngen-view fed by packed data, an editor that owns USD, an asset
server that packs, and an introspection tool. What has landed of it is under [Landed](#landed).

1. **Editor split (step 3)** — [plan_editor_split.md](plan_editor_split.md), draft. Deltas and incremental GPU scene, then `ngen-editor` as its
   own process. View gizmos as local opinions and ngen-view without pxr landed with the scene pack; the plan needs revising for that
   (it assumes the editor still runs in the view).
   - Open: how unsaved editor edits reach packed data.
   - Fix: the plan still says "build server" in places.
2. **Everything async** — *from the objective.* The view opens a scene and draws its first frame at once, and the scene arrives while it runs,
   with no start-up shaders (since [plan_async_scene.md](plan_async_scene.md), under Landed). What is left:
   - **Off-thread allocation**: large buffers and textures are still made on the render thread, so a load shows hitches (Sponza 80–104 ms,
     Caldera 312 ms at worst). In that plan's Deferred, with its trigger: streaming.
   - **Sub-packs and LOD levels streaming in by residency**: the streaming steps of [plan_streaming_lod.md](plan_streaming_lod.md).

## Engine features

- **LOD with UsdLod** — the USD Level of Detail schema (`LodRootAPI`, distance and screen-size heuristics, overrides, separate imaging,
  physics and audio domains), in OpenUSD since v26.08, which is the vendored version (`libusd_usdLod`; not linked yet). The packer reads LOD
  roots into the scene pack; the view selects levels at runtime; levels stream in on demand.
  Ties into GPU-side LOD selection and meshlets.
- **Streaming** — [plan_streaming_lod.md](plan_streaming_lod.md), design in progress. Load and unload scene data by where the camera is
  and what fits in memory, so a world much larger than memory (Caldera, below) can be opened and moved through; geometry and textures (mip
  levels by screen size). Decided: one system gives both LOD's wanted level and what is resident (the coarsest level always resident as the
  placeholder); UsdLod is the only LOD; the streaming structure is authored with standard USD (payloads as units, `extentsHint` as their bounds,
  load rules for residency), never computed at pack time; budgets per memory kind, priority by on-screen error.
  - Also decided: no grouping beyond payloads; draw whatever is resident as soon as it is (gaps while content arrives are accepted); a payload
    without `extentsHint` is always wanted and reported; every wanted payload is requested with a priority, nearest first, and never cancelled.
  - Open: tuned thresholds, the far-field floor's cost.
  - Next: Caldera's proxy map through the scene pack (it opens as its default map since the scene pack landed), districts streaming, models
    streaming, textures streaming, each a plan of its own.
- **Physics** — UsdPhysics schemas (rigid bodies, colliders, joints, scenes; already in the vendored OpenUSD) mapped to physics components by
  the packer, simulated with **Jolt**. Physics is a UsdLod domain of its own.
- **Skinning** — UsdSkel skeletons and skinned meshes; GPU skinning.
- **Animation** — skeletal and transform animation from USD time samples, playback and blending; a fixed timestep for deterministic runs.
- **Gameplay framework** — the layer games are built on: entities and components above the runtime scene, game logic and its update loop,
  input, spawning, game state. The API AI-made games use (see Objective).
- **Networking and multiplayer** — state replication over the entity/component model, client/server, prediction. Shapes the gameplay framework,
  so it is designed with it rather than after it.

## Caldera

Load [Activision's Caldera](https://github.com/Activision/caldera) (Warzone's map as USD: about 17.5 million prims and over 2 billion points over
2×2 miles, Z-up in inches; non-commercial licence). It is the large-scene test for most of the above. The UsdLod-converted fork (branch
`usdlod`) is checked out at `assets/caldera/` (git-ignored) and served by the repo's asset server like any test asset.
- **Load it at all**: payloads and proxies as sub-packs, loaded on demand; the default lightweight view first, detail streamed in (Streaming,
  above).
- **LOD through UsdLod**: the engine's LOD system selects and streams from Caldera's UsdLod roots (the converted branch, under Landed); it
  reads no other LOD convention. The converter's thresholds are a starting point, tuned once the engine draws levels.
- **Shading without textures**: Caldera has no textures or materials. An authored layer adds them: generated materials by prim type, name or
  region (colour palettes, procedural or triplanar shading), so the map reads well. Good work for the AI and human collaboration flow.
- **Scale it exposes**: occlusion culling, meshlets, GPU memory and streaming budgets, packing time on cold caches; lighting, depth range and
  camera framing at kilometre scale (Rendering, below).

## Assets and packing

- **Variant sets in scene packs** — packed as alternative opinions, switched in the view ([plan_scene_pack.md](plan_scene_pack.md), Deferred).
  Trigger: a test scene with a variant set, or a project that uses them.
- **BC7 / BC5 textures** — needs approval for Vulkan's `textureCompressionBC`, a vendored encoder, RHI formats. 4× smaller cache and stream.
- **Linear (non-colour) textures** — the request must carry intent (e.g. `#linear` id or a per-pattern rule). Trigger: first normal/roughness map.
- **Mip-level streaming** — now part of **Streaming** (Engine features). Packing side: levels addressable on their own, so the server can send
  the lowest first.
- **An edit while its file's job packs** can be recorded as packed from the new content
  ([plan_asset_change_notices.md](plan_asset_change_notices.md), Deferred). Trigger: a stale asset after a quick double save.
- **Client-side cache** keyed by id and version. Trigger: start-up transfer time, or a remote view.
- **Remote connections and authentication** (beyond loopback). Trigger: a view on another machine or a devkit.
- **Priority** of pack requests, and changing a request's priority; no cancellation: a request no longer needed still completes and is cached,
  and the view ignores it ([plan_streaming_lod.md](plan_streaming_lod.md), decision 7). Trigger: streaming (Engine features).
- **Refuse a second asset server** for the same variant in the same directory.
- **Unified build and asset server** — the likely end state. Trigger: the two keep needing the same state.
- **Long-running packer workers**; **shared content store across variants**; **bundling packed assets for shipping**.
- **`.usdz` packages** through an `ArPackageResolver`. Trigger: a `.usdz` test asset.
- **Asset and reference browsers listing from the server**. Trigger: editor split, or a remote view.
- *Parked:* saves through the asset server (`asset.write`); engine assets vs project assets; running a view without an asset server (offline,
  shipping).

## Rendering

- **Multiple lights** — [plan_usd_lights.md](plan_usd_lights.md) not implemented: point/sphere lights, then spot, area, dome/IBL; clustered or
  tiled light culling.
- **HDR lighting + tone mapping** (in the AA pass).
- **Shadows**: spot/point light shadows; cascade blending / softer techniques. Trigger: a seam PCF doesn't hide.
- **Culling**: occlusion (two-phase HZB); meshlets and LOD — need a meshlet/LOD pack step.
- **Frame graph**: transient buffers; cross-frame history resources (TAA); multi-queue/async compute; parallel recording.
- **Cutout follow-ups** (from [plan_cutout_materials.md](plan_cutout_materials.md)): translucency (blended surfaces, sorting; trigger: glass or
  smoke — Sponza's `glass` and `lamp_glass_01_usd` draw opaque, with a packer warning); coverage-preserving opacity mips (trigger: foliage vanishing
  at a distance); a per-material "casts no shadow" for decals, which today cast mask-shaped shadows onto the wall they lie on (trigger: it bothers).
- **Material model**: factors and more texture maps; per-material sampler state from USD.
- **Incremental GPU updates** for non-transform changes (part of the editor split).
- **Lighting at kilometre scale** — one sun and a hemisphere ambient (an engine sky and ground colour blended by how much a face turns up,
  since 2026-10-13; it replaced a constant that made every face out of the sun one flat colour). Vertical faces out of the sun still look
  alike whichever way they face; dome lights and IBL (under Multiple lights) replace the hemisphere. Shadows end at a shadow distance since
  [plan_depth_range.md](plan_depth_range.md); a fade before it is deferred there.
- **Framing Caldera** — framing the whole scene fits a large flat ground disc, so the map frames from about 60 km with the island small in the
  middle. Authored cameras are read (the Camera window, `--camera=/cameras/<name>`; Caldera authors 13, per district, none of the whole
  island). Open: a default camera, the one a `UsdRenderSettings` prim's `camera` names (stage metadata `renderSettingsPrimPath`), which needs
  that metadata and the relationship in the scene pack; and an authored one for Caldera. Also open: a camera's field of view (focal length,
  aperture) and roll.
- **Swapchain format** — the backbuffer takes whatever format the driver lists first: headless runs get UNORM, a window sRGB, so headless
  screenshots don't match the window's brightness. Needs a decision (the imgui colours are corrected for sRGB targets).
- Smaller items with triggers: octahedral normals, front-to-back sort, de-interleaved/16-bit vertex streams, 16-bit indices.

## RHI

Known gaps, all trigger-gated ([src/rhi/README.md](../src/rhi/README.md)):
- single queue and command pool; no async compute or indirect dispatch
- one device allocation per resource (no suballocator)
- no compressed formats (BC7 above brings them)
- one blend state for all attachments; one push constant range per pipeline; blits on layer 0 only
- raw-pointer resources and a Vulkan-shaped descriptor model — revisit with a second backend (D3D12, Metal)

## Tools, build and CI

- **Relocatable install** — ship the OpenUSD shared libraries next to the binaries with an `$ORIGIN`-relative rpath (recommended over a
  monolithic or static build). Only matters until ngen-view stops linking USD.
- **CI**: move run cancellation from the workflow to the `build` job, so a push can't cancel an OpenUSD build before its cache is saved; report
  the lavapipe push-constant bug to Mesa (offered, not done).
- **Introspection follow-ups** ([plan_introspect_tool.md](plan_introspect_tool.md), "Deferred"): profiler zones on the trace stream, session
  recording in the windowed tool, structured asset server events (its messages are text lines today), `get --every` for a record over time.
- **Introspection extras**: Tracy; GPU crash diagnostics (breadcrumbs, `VK_EXT_device_fault`); scene/USD provenance; per-pass GPU time column;
  zone statistics; a headless performance regression check; the GPU-driven debugging plan.
- **Build system**: static libraries aren't passed on transitively (programs link each library directly); watch mode; Windows; deterministic
  archives (`ar rcsD`); de-duplicated merged `compile_commands.json`.
- **ngen-cli**: `editor` and `introspect` commands when those tools exist; machine-readable target listing.
- **Restructure `src/apps/view.cpp`** — about 1,000 lines mixing start-up, the frame loop, input, editor wiring and RPC; split it into parts
  with clear ownership, keeping `main` thin as the apps folder rule says.
- **Editor UX** (until the editor split): gizmo plane handles, local space, snapping, multi-select; undo for layer operations; drag coalescing.

## Housekeeping

- Early-exit hang: a failed `Renderer::init` leaves the process stuck and ignoring SIGTERM ([notes.md](../notes.md)).
- Stale docs: [usd_scene_system_status.md](usd_scene_system_status.md) says Phase 5 hasn't started (it has); the docs index calls
  [plan_usd_lights.md](plan_usd_lights.md) in progress (it isn't started); [plan_editor_split.md](plan_editor_split.md) says "build server".
- Plans referenced but never written: `plan_gpu_driven_debugging.md`, `plan_tracy.md`, `plan_rhi_queues.md`, `plan_shader_introspection.md`.
- Headless runs started in the repo root write window layout to the developer's `imgui.ini`.

## Landed

Moved here from the sections above when they landed, newest first; the plan holds what was built.

- **Async scene** — [plan_async_scene.md](plan_async_scene.md). ngen-view never waits for its scene. A scene thread composes, extracts and
  diffs as assets arrive, and publishes snapshots and change lists. Meshes, materials and textures are kept per asset across rebuilds, and the
  GPU scene changes piece by piece: pool ranges, texture slots, material entries, instance slots, uploads a budget per frame. Pipelines are made
  as their shaders arrive; a broken one no longer stops the view. Scripts count frames from when the scene is resident (`wait-resident`,
  `--no-wait-resident`), and the asset server has a `--delay` test aid. First frame 30–40 ms after start on Sponza and Caldera.
- **Depth range for large scenes** — [plan_depth_range.md](plan_depth_range.md). The camera uses reversed-Z with no far plane, so scenes of
  any size draw and nothing is clipped at a distance. Shadows end at a shadow distance (200 m by default), and framing sets the fly speed. One
  `cameraProjection` replaces four copies of the range, and frustum planes, corners and gizmo rays follow the convention. Caldera draws with no
  camera flag.
- **Asset change notices** — [plan_asset_change_notices.md](plan_asset_change_notices.md). The asset server watches the sources of what each
  client received (inotify on directories, polling as the fallback and `--watch=poll`) and sends `asset.changed`; clients request again, and
  nothing packs without a request. ngen-view reloads changed shaders in place (pipelines recreated, old ones retired, a broken shader keeps the
  old ones) and fetches and rebuilds changed scene assets through the blocking Repack path.
- **Cutout materials** — [plan_cutout_materials.md](plan_cutout_materials.md). UsdPreviewSurface opacity with `opacityThreshold > 0` is packed
  (constant or texture channel, raw flag, threshold) and drawn through four buckets per view, with cutout pipelines that discard in the geometry
  pass, depth prepass, shadow pass and debug views; prims with a cutout submesh cast shadows per submesh. Sponza's decals draw as dirt, not quads,
  and picks go through them.
- **Async shaders** — [plan_async_shaders.md](plan_async_shaders.md). `PipelineRegistry` creates every pipeline from requests the passes
  register (descriptions with shader ids). Start-up shaders go out in one request derived from the registrations and pack while the scene
  loads; the debug views' shaders are requested the first time a view is turned on, and the lit image shows until they arrive. A `pipelines`
  record and a Render Debug tab show each pipeline's state. Hot reload, reflection and off-thread creation are deferred.
- **Scene pack (step 2, the scene half)** — [plan_scene_pack.md](plan_scene_pack.md). `ngen-packer-usd` packs each USD file in one job: a
  layer pack of its opinions plus a mesh or material asset per prim, `file.usd#/prim`. `src/world/` composes the packs as USD composes a stage
  (layer stack, references and payloads, instancing, UsdLod roots), checked byte for byte against OpenUSD with `ngen-worldcheck` and
  `ngen-usdcheck` up to Caldera's `map_capital` at full detail. ngen-view renders from packs, with screenshots byte-identical to the USD view,
  links no OpenUSD, picks on the GPU, and moves entities with gizmos as local opinions; the USD editor code waits in `src/editor/`, unbuilt.
- **Caldera in UsdLod** — [plan_caldera_usdlod.md](plan_caldera_usdlod.md). A converter in a Caldera fork (branch `usdlod` of
  `hschlichter/caldera`) rewrites Caldera's `districtLod` and `modelLod` variant sets into UsdLod roots: levels as payloads, a screen-size
  heuristic per root, instanceable models kept instanced through a class prim. 45 district and tile roots, 2,563 model roots; verified to
  compose the same geometry and instancing, level for level.

- **OpenUSD v26.08** — the submodule moved from v26.03, rebuilt with the same minimal flags; it brings the UsdLod schema. three_cubes and
  Sponza load the same (prims, meshes, instances, textures). Its headers still don't compile as C++23, so `usd*.cpp` stay C++20. CI's
  OpenUSD cache key follows the submodule commit, so CI rebuilds it once.

- **Introspection tool (step 4)** — [plan_introspect_tool.md](plan_introspect_tool.md). `ngen-introspect` shows every process's records and
  merged trace, in a window or on the command line; `src/trace/` replaced the observation bus. The trace is flow and messages (levels, text,
  printed on the console), data is records; start-up reads as a timeline; agents verify with records and `ngen-introspect trace`.
- **Scenes and textures through the asset server (step 2)** — [plan_asset_copy.md](plan_asset_copy.md),
  [plan_texture_packer.md](plan_texture_packer.md), [plan_texture_batch.md](plan_texture_batch.md). USD layers and textures stream from the
  asset server through a USD asset resolver; textures are decoded and mip-mapped at pack time and requested in one batch per scene.
- **Asset server (step 2)** — [plan_asset_server.md](plan_asset_server.md), after [plan_pack_rules.md](plan_pack_rules.md).
  `ngen-asset-server` with rules by extension in `pack.cpp`, request-driven packing with its own cache, packed data streamed to clients;
  shaders packed through it.
- **RPC (step 1)** — [plan_rpc.md](plan_rpc.md). RPC core, the engine method registry, every view verb as a method.
