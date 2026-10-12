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

1. **Scene pack format** — [plan_scene_pack.md](plan_scene_pack.md), draft. The critical path: one pack job per USD file, layer packs composed
   in the view, no OpenUSD in the view, no USD editing until `ngen-editor`.
   - USD packer: layer packs (references and payloads packed once, instanced), typed components; mesh and material assets as
     `file.usd#/prim`, written by the file's one job.
   - Stable entity ids (prim path + submesh index).
   - Runtime scene library (no pxr, `src/world/`): layer stack composition, transform hierarchy; replaces `RenderWorld`.
   - Packer-discovered assets (references, payloads, textures) named in the pack and requested by the view.
   - Later: variant sets, with a test scene of their own.
2. **Async shaders** — [plan_async_shaders.md](plan_async_shaders.md), draft. Pipeline registry, start-up shaders from registrations (replaces
   `startupShaderIds()`), debug view shaders on first use.
3. **Editor split (step 3)** — [plan_editor_split.md](plan_editor_split.md), draft. Deltas and incremental GPU scene, then `ngen-editor` as its
   own process, view gizmos as local opinions, ngen-view without pxr.
   - Open: how unsaved editor edits reach packed data.
   - Fix: the plan still says "build server" in places.
4. **Everything async** — *from the objective.* Scenes open instantly with placeholders; geometry, textures, shaders, sub-packs and LOD levels
   arrive and appear when ready, never blocking a frame. Builds on the asset server's streaming, async shaders, sub-packs as load units, and
   the editor split's incremental GPU scene. Today the view still waits for shaders and loads the scene synchronously.

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
  - Next: the scene pack plan takes the design's requirements (decision 5); then Caldera's proxy map through the scene pack, districts
    streaming, models streaming, textures streaming, each a plan of its own.
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
2×2 miles, Z-up in inches; non-commercial licence). It is the large-scene test for most of the above.
- **Load it at all**: payloads and proxies as sub-packs, loaded on demand; the default lightweight view first, detail streamed in (Streaming,
  above).
- **LOD through UsdLod**: the engine's LOD system selects and streams from Caldera's UsdLod roots (the converted branch, under Landed); it
  reads no other LOD convention. The converter's thresholds are a starting point, tuned once the engine draws levels.
- **Shading without textures**: Caldera has no textures or materials. An authored layer adds them: generated materials by prim type, name or
  region (colour palettes, procedural or triplanar shading), so the map reads well. Good work for the AI and human collaboration flow.
- **Scale it exposes**: occlusion culling, meshlets, GPU memory and streaming budgets, packing time on cold caches.

## Assets and packing

- **BC7 / BC5 textures** — needs approval for Vulkan's `textureCompressionBC`, a vendored encoder, RHI formats. 4× smaller cache and stream.
- **Linear (non-colour) textures** — the request must carry intent (e.g. `#linear` id or a per-pattern rule). Trigger: first normal/roughness map.
- **Mip-level streaming** — now part of **Streaming** (Engine features). Packing side: levels addressable on their own, so the server can send
  the lowest first.
- **Hot reload / source edits reaching a running view** — *parked* until the architecture is up. Options: server sends `pack.stale`, or clients
  re-request on command. Nothing packs without a request either way.
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
- **Material model**: factors and more texture maps; per-material sampler state from USD.
- **Incremental GPU updates** for non-transform changes (part of the editor split).
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
