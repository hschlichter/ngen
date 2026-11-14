# ngen glossary

The words the engine, its code and its documents use, each in a line or two, grouped by the part of the engine they belong to. It is for a
quick look-up: the library README named under each heading is where a term is explained in full. [`REFERENCE.md`](REFERENCE.md) is the
companion cheat sheet for commands, flags, verbs, methods and records.

Several common words mean different things in different parts of the engine (*level*, *pack*, *slot*, *group*, *view*, *record*…).
[Words with more than one meaning](#words-with-more-than-one-meaning) at the end lists them side by side.

- [Programs](#programs)
- [Build](#build)
- [Assets and packing](#assets-and-packing)
- [USD and composition](#usd-and-composition)
- [LOD and streaming](#lod-and-streaming)
- [The view's runtime](#the-views-runtime)
- [Cameras, selection and tools](#cameras-selection-and-tools)
- [Renderer](#renderer)
- [RHI](#rhi)
- [Runtime access: RPC, records, trace, profile, configuration](#runtime-access-rpc-records-trace-profile-configuration)
- [Project and process](#project-and-process)
- [Words with more than one meaning](#words-with-more-than-one-meaning)

## Programs

Every program's `main` is in `src/apps/` (the build system's and the RHI examples' are the exceptions). Binaries land in `_out/<platform>/<config>/`.

- **ngen-view** — the viewer: composes a world from packs, streams and LODs it by the camera, draws it GPU-driven. Links no OpenUSD. Often
  just "the view".
- **ngen-asset-server** — packs source files on request and streams the packed bytes over RPC. Rooted at its working directory. The view exits
  at start-up without one. Often "the asset server" or "the server".
- **ngen-introspect** — lists, reads and calls every running ngen process, and merges their traces; a window and a command line.
- **ngen-cli** — the front door: build, run the view and asset server, pack, introspect, for the *set variant*. `./ngen-cli` in the repository
  root is a symlink to the set variant's cli.
- **ngen-build** — the self-hosted build system's orchestrator (`_out/ngen-build`). It builds and runs `ngen-build-graph` (the project graph,
  `build.cpp`) and `ngen-build-run` (the runner).
- **ngen-pack** — asks the asset server to pack everything a scene can reach (every LOD level and variant), to fill the cache ahead of a view.
- **ngen-packdump** — prints scene packs as JSON.
- **ngen-worldcheck** / **ngen-usdcheck** — print a scene's *fingerprint* from the engine's World and from OpenUSD, to compare as files.
- **ngen-packer-\*** — one packer program per asset type: `usd`, `texture`, `exr`, `shader`, `copy`. Run by the asset server, never by hand.
- **ngen-example-\*** — the RHI example programs, one feature each, with `--check --validation`.
- **ngen-editor** — the USD editor process; its code is in `src/editor/`, unbuilt until the editor is built from it.

## Build

`src/build/README.md`; the project's own description is the root `build.cpp`.

- **Bootstrap / seed** — the one `c++` command that compiles `src/build/bootstrap.cpp` into `_out/ngen-build`. Re-run when the bootstrap or
  the IR format changes.
- **Build log** — the runner's record of each edge's inputs and outputs by content hash; `_out/<platform>/<config>/.ngen-buildlog` (project)
  and `_out/.system/.ngen-buildlog` (self-build).
- **Config** (build configuration) — `debug`, `release` or `gamerelease`; with the platform it names a variant. `gamerelease` compiles out
  the RPC endpoint (`NGEN_INTROSPECTION`) and Tracy's client.
- **Edge** — one command in the IR: a compile, archive, link, tool run or phony.
- **Extension** (`ExtensionMap`) — how a language module (`build::cxx`) or an auxiliary (`Tool`, `Alias`, `Phony`) attaches to the
  language-agnostic core types.
- **Fuzzy target matching** — a target name resolves by exact edge name, else by substring of source stems, else by substring of other edges.
- **Global tool** — a `Tool` that works on the source tree rather than a variant (`format`, `tidy`); runs only when named.
- **IR** — the frozen binary build graph per variant (`build.ngenir`), commands fully baked into shell strings.
- **ObjectFile** — one translation unit as its own graph node (`cxx::ObjectFile`).
- **Platform** — a toolchain and OS target, `linux-vulkan`.
- **Project graph** — `build.cpp`, a C++ program written against the header-only framework; compiled into `ngen-build-graph`.
- **Runner** — `ngen::run::execute`: dirty detection, scheduling, the build log. One runner does the self-build and the project build.
- **Set variant** — the variant `ngen-cli` uses, stored in `_out/set`. Shared with the human's session: agents don't change it unless asked.
- **Target** — a library, program, tool, alias or phony a build can name; with no target, every target except global tools is built.
- **Variant** — a (platform, config) pair, `linux-vulkan/debug`; each has its own output folder, binaries, asset server and cache.

## Assets and packing

`src/asset/README.md`.

- **Asset** — engine-ready data the asset server serves: a shader, a texture, a layer pack, a mesh, a material.
- **Asset id** — the source path relative to the asset server's working directory, forward slashes (`shaders/gbuffer.mesh`,
  `assets/textures/brick.png`). Never changes when the content does. A scene argument to the view is an asset id.
- **AssetClient** — the engine side of the stream: `request`, `find`, `take`, `takeChanged`, `log`. The rest of the engine uses it directly.
- **Cache** — `.ngen-assets/<platform>/<config>/` in the server's directory: packed files by asset id, and `.ngen-assetcache` recording what
  each was packed from.
- **Change notice** (`asset.changed`) — the server telling a client that the sources of assets it received changed. The client marks them
  *stale* and decides whether to request them again; the server packs nothing for a notice.
- **Depfile** — the Make-format list of every file a packer read; with the job key, what the up-to-date check compares.
- **Discovery** — `.ngen-discovery/<kind>-<pid>.json` files an endpoint writes in its working directory, so processes in the same directory find
  each other. See [RPC](#runtime-access-rpc-records-trace-profile-configuration).
- **Fragment / secondary output** — an asset a file's job writes besides the file itself, with the id `<file>#<fragment>`: a USD file's meshes
  and materials are `<file>#<prim path>`.
- **Job** — one run of a packer for one source file; one job per file at a time, shared by every request for any of its ids.
- **Job key** — a rule's name, version, parameters and packer name; part of the up-to-date check.
- **Layer pack** — one USD file's own opinions, as written (no composition), in the scene pack format. The asset whose id is the file.
- **Material pack** — a `def Material`'s `UsdPreviewSurface` network flattened: base colour (constant or texture), cutout opacity.
- **Mesh pack** — a prim's geometry: welded vertices, 32-bit indices, submeshes per `GeomSubset`, meshlets, bounds.
- **Pack** (verb) — turn a source file into its engine-ready form; the process inside the asset system. **Pack** (noun) — a packed file,
  usually a scene pack (layer, mesh or material pack).
- **Packed texture** — the `NGTX` format: a 28-byte header, an optional irradiance block, every mip level.
- **Packer** — a standalone program for one asset type, run with a fixed argument contract (`--source`, `--out`, `--depfile`, `--param`).
- **Pack rule** — an entry in the root `pack.cpp`: extensions, packer, parameters and a version to bump on a format change. Compiled into the
  asset server.
- **Repack** — ask the server to pack assets again even when its cache is up to date (`force`); the view's Assets window and `repack` verb.
- **Scene pack** — the binary `NGPK` format for layer, mesh and material packs: a header, a string table and typed sections.
- **Version** (asset version) — the FNV-1a 64 content hash of the packed file, 16 hex digits. A client sends the versions it holds (`have`) and
  gets `sent: false` when current.

## USD and composition

`src/world/README.md` for the World and how it composes; the USD terms are OpenUSD's.

- **Arc** — a composition arc: a sublayer, reference, payload, variant (and in USD inherits, specializes, relocates, which the engine doesn't
  compose).
- **Component** (World) — data beside the entities, per content: lights, LOD roots, point instancers, payload placements. Not a gameplay
  component; that framework is a roadmap step.
- **Compose round** — one `WorldComposer::compose()`: compose from the packs held, return the packs named and not held. The caller fetches them,
  `addPack`s them and composes again until nothing is needed; each round goes one level of arcs deeper.
- **Content** (`WorldContent`) — a separately composed, immutable piece of the World: the stage, a prototype, or a LOD level. Kept across
  composes at a stable index, with a `version` that changes each time it is composed.
- **Default prim** — the prim a reference or payload naming no prim targets.
- **Entity** — one composed prim in the World, whatever its type or purpose.
- **Fingerprint** — one line per prim (path, type, point count, optionally its world matrix) printed by `ngen-worldcheck` and `ngen-usdcheck`.
- **Gap** — an arc to a pack not held yet, composed as nothing for now; the content is composed again once the pack arrives.
- **Instance / instanceable** — an instanceable prim with arcs that bring specs; it names a shared *prototype* and has no children of its own.
- **Layer** — one USD file. **Root layer** — the scene's file. **Layer stack** — a layer and its sublayers, strongest first.
- **Linked** — a content is linked (an instance's `prototype`, a level stand-in's `linkedContent`) only once it is complete; until then it is
  -1 and nothing reads it.
- **Material key** — a bound material as its pack's asset id plus its inputs' values (`<asset>?<input>=<value>&…`); one pack used with
  different inputs is one material per set of values.
- **Opinion** — a value a layer authors for an attribute or metadata; the strongest wins.
- **Payload** — an arc whose target loads separately; in the engine, payloads under LOD levels are what streams.
- **Pcp** — OpenUSD's prim cache population, the composition engine `WorldComposer` follows.
- **PointInstancer / point** — a prim that draws prototypes at per-point transforms; each visible point draws its prototype.
- **Prim** — a node in USD's namespace, named by its path (`/World/Tree`).
- **Prim index** — a prim's tree of nodes in strength order, each a *site* (a prim path in a layer stack) contributing opinions.
- **Prototype** — the content shared by instances with the same arcs; its paths are relative to it.
- **Purpose** — `default`, `render`, `proxy` or `guide`; `guide` and `proxy` draw only when turned on (`scene.purposes.*`).
- **Relink / resolution** — deciding after composing which level contents LOD roots link to, without composing the stage again
  (`WorldComposer::relink`).
- **Release** — a kept content the resolution no longer reaches is emptied in place at once.
- **Spec** — a prim's opinions in one layer (`def`, `over`, `class`).
- **Stage** — USD's composed scene. In the World, the stage is the content holding the root layer stack's prims.
- **Stand-in** — the entity in a LOD root's content for one of its levels, linked to the level's own content.
- **USD / OpenUSD** — the scene format the worlds are authored in; OpenUSD (`pxr`) is linked only by the USD packer, `ngen-usdcheck` and the
  editor code.
- **Variant set / variant / selection** — named alternatives on a prim; the strongest `variants` opinion selects one. A set with no selection
  composes no variant.
- **View-local opinion** — a value the view holds over the scene for this run only: a gizmo drag, `translate`, the LOD tuning. Never saved.
- **World** — the runtime scene: every prim composed from layer packs, with no USD in it. `World::visit` walks it with full paths.
- **WorldComposer** — composes layer packs into a World as Pcp composes a stage.

## LOD and streaming

`src/world/README.md` ("Loading"), `src/view/README.md` (residency, switching, the LOD and Streaming tools), `src/renderer/README.md`
("Render LOD").

- **Authoring contract** — what a streaming world's thresholds must satisfy (a threshold per level boundary, imaging threshold ≤ load radius ≤
  unload radius, a child inside its parent). Checked by `checkLodAuthoring`; a violation is a *finding*, reported, never corrected.
- **Blend band / blend thresholds** — UsdLod's `blendThresholds`: a range past a threshold where a render-LOD group cross-fades to the next
  level; a streaming root inside one wants the finer level.
- **Bias** — a tuning factor: every root picks as if that many times closer.
- **Cell** — the test world's word for a streaming root's area. **Region** (1 km, levels `full` and `proxy`) and **tile** (250 m, levels
  `full` and `simplified`) are its two cell sizes, with asset LOD roots inside tiles.
- **Distance heuristic / screen-size heuristic** — UsdLod's two ways to measure a root: distance to a centre or box, or the extent's
  bounding sphere as a fraction of the view window. The view's *preference* picks one when a root names both.
- **Domain** — what a heuristic decides: `imaging` (which level is drawn) or `streaming` (which levels are resident; the engine's own domain
  token).
- **Drawn level** — the level a root actually draws: the finest loaded and *ready* level not finer than the selected one.
- **Fade / dither** — a render-LOD group inside a blend band draws both levels, split per pixel by a 4×4 Bayer pattern.
- **Forced level** — a render-LOD group's level set by hand (`lod`), or its default while the heuristic is off or unusable.
- **Heuristic** — a `LODDistanceHeuristic` or `LODScreenSizeHeuristic` prim a root names in `lod:heuristics`, with thresholds and a domain.
- **Hole** — a root that draws nothing because no loaded level at or coarser than the selected one is ready.
- **Hysteresis** — a margin before going coarser at a threshold, so a camera on the boundary doesn't flip a root (20% by default). For
  residency, the previous streaming state is the hysteresis.
- **Late** — a root drawing coarser than it selects, because the finer level isn't ready yet. **Lead** — seconds a level was ready before it
  was selected.
- **Load radius / unload radius** — a streaming heuristic's thresholds (load) and `streaming:unloadThresholds` (unload): a level becomes
  resident inside its load radius and stops past its unload radius.
- **LOD level** — one child of a LOD root, finest first; level 0 is the finest.
- **LOD root** — a prim with `LODRootAPI`; its levels are its defined, active children. Either a *streaming root* or a *render-LOD root*,
  decided by whether its levels have payloads.
- **LOD tag** — the drawn level plus flags written to the normal target's alpha, which the LOD Level view colours.
- **LOD tuning** — view-local threshold overrides, bias, hysteresis and blending (`view.lod.tuning`); kept until the view closes.
- **Pop** — a render-LOD group's drawn level changing without a fade; counted and flashed white for half a second.
- **Prefetched** — a finer level resident ahead of being drawn.
- **Ready** — a level whose placement waits for nothing: linked, and every mesh, material and texture its prims want held or failed.
- **Render LOD** — LOD roots whose levels are composed in place (no payloads); placed as a *render-LOD group* whose level the GPU picks per
  placement each frame.
- **Residency** (of a level) — whether a level is loaded, decided only by camera position and authored radii, never by time. The *streaming
  state* level and every coarser level are resident. Not the same as the scene being *resident* ([the view's runtime](#the-views-runtime)).
- **Selected / wanted level** — the level a root's imaging heuristic picks from the camera (or a hand-set level).
- **Streaming root** — a LOD root whose levels are payloads; each level is a content of its own, loaded and released by residency.
- **Streaming state** — a root's finest level that must be resident: finer at once by load radii, coarser only past unload radii.
- **Switching levels** — a root starting to draw another level; it happens in the pass the last piece of the new level arrives, so a district
  appears whole.
- **Test world / streamtest world / Eldermark** — the generated 10 km × 10 km world authored to stream (regions, tiles, payloaded levels,
  authored cameras); `assets/streamtest-world/world/root.usda`, not in the repository.
- **UsdLod** — the USD LOD schemas (`LODRootAPI`, heuristics, `ComputeLOD`); the engine's only LOD convention.
- **Unloading** — a level leaving its root's resident levels is let go at once, with its placement, contents, library entries and mesh packs.

## The view's runtime

`src/view/README.md`.

- **Arrival** — an asset the server answered, taken by the scene thread.
- **Batch** — one round of the scene thread: the requests and arrivals it took, a compose, a placement pass, and a publish.
- **Change list** / **`SceneChanges`** — what a placement pass changed (draw slots added, removed and updated, meshes, materials, textures,
  lights, LOD groups); the main thread hands it to the render thread, which changes only that on the GPU.
- **Draw** — one prim's submesh as drawn: a slot in `RenderWorld`, an instance on the GPU.
- **Draw slot** — a draw's index in `RenderWorld` and the GPU instance buffer; a draw keeps its slot while it exists.
- **Engine light** — the distant light the view adds when a scene authors none.
- **Framing** — until the scene is first resident, the camera frames what has arrived, unless moved by hand.
- **Handle** — a 1-based index into a library (`MeshHandle`, `MaterialHandle`, `TextureHandle`, `PrimHandle`); never reused.
- **Held** — a library entry a placement uses (drawn or an undrawn resident level), so switching to it uploads nothing.
- **Hooks** (`ViewHooks`) — the points in `ViewFrame` where the tools (`ToolsHooks`) and the UI (`UiHooks`, wrapping the tools') join the frame:
  `afterScene`, `cameras`, `event`, `afterCamera`, `debugDraw`, `wantReadbacks`, `takeReadbacks`, `drawUi`, `fillSnapshot`.
- **Library** (`AssetLibrary`: `MeshLibrary`, `MaterialLibrary`, `TextureLibrary`) — one immutable entry per asset; new content is a new entry
  with a new handle.
- **Main thread** — runs `ViewFrame`'s steps, the commands, the tools and the UI; never waits for the scene or render threads.
- **Placement** — a content placed at a world transform with the state it inherits there (or a PointInstancer's points); owns its prims'
  draws.
- **Placement pass** — after a compose, relink, level evaluation, arrivals or a gizmo move, placing the stage as the World is now; rebuilds only
  the placements whose inputs changed.
- **Placement tree** — the placements as a tree: the stage, prototypes at instances, a placement per loaded level of each streaming root.
- **Resident** (the scene) — everything the scene wants is loaded, uploaded and drawn, on both the scene and render threads (`SceneResident`).
  Script frames count only while nothing waits for it.
- **Scene thread** — `ViewScene`'s thread: owns the composer, the packs and the libraries; composes, places and publishes.
- **`SceneSnapshot`** — what the scene thread publishes; the main thread takes the latest once a frame and every query reads it.
- **Script** — `--script=<file>`, lines of `<frame> <verb> [args]`; frames count from the first residency. The *script clock* holds while
  something waits (residency, a flight).
- **Sky / environment** — the scene's dome light as the lit view's background and ambient (`RenderWorld::environment`).
- **Step** — one call of `ViewFrame`'s fixed order: `updateScene`, `runFrameCommands`, `advanceTime`, …, `buildSnapshot`, `plotFrame`.
- **`ViewContext`** — what the engine commands are registered over: references into the `ViewFrame` and the main loop.
- **`ViewFrame`** — one frame of the engine as a fixed order of steps, called by `src/apps/view.cpp`'s loop.
- **`ViewScene`** — the scene the view shows: the World composed from packs and flattened into the renderer's inputs.
- **Walk** (`ContentWalk`) — a content walked once per version and inherited state; every placement of it reuses the walk.

## Cameras, selection and tools

`src/view/README.md`, `src/tools/README.md`.

- **Camera modes** — *orbit* (Left Alt, around the surface under the cursor) and *following the ground* (G, eye height above the drawn
  surface).
- **Culling camera** — the camera culling and occlusion use: the main camera, or the debug camera when `tools.debugcamera.cull` hands it over,
  or the frozen frustum.
- **Debug camera** — a second camera to watch the main camera from; it renders, the main camera still decides (LOD, residency, culling,
  cascades) unless switched.
- **Flight** — `fly <prefix> <speed>`: the camera moving through the scene's authored cameras in path order; a test driver for streaming.
- **Frozen frustum** — culling held at one camera matrix while the camera moves on.
- **Gizmo** — the transform handles on the selection (translate, rotate, scale) and the corner axis gizmo; drawn by `GizmoRender`.
- **LOD camera** (`lodCamera`) — the camera LOD and streaming are decided from; the main camera unless switched.
- **LOD tool / Streaming tool** — the tools' data and state for LOD and streaming inspection, switching and tuning (`LodToolState`,
  `StreamingToolState`); their windows are the LOD and Streaming windows.
- **Main camera** — `ViewFrame::cam`, the player's camera; everything that places the camera moves it.
- **Overlay** (viewport) — lines and shapes drawn over the view: grid, origin, bounds, frusta, light gizmos, the LOD and streaming layers.
- **Picking** — a click reads the `gbuffer.instance` texel under the cursor back from the GPU and maps the instance to its prim, a few frames
  later.
- **Selection** — a prim's full path, instances expanded.
- **Tool** — a part of `src/tools`: data built from the engine, state in a plain struct (`ToolsState`), viewport layers; no UI.
- **Tool state / requests** — what a tool's hooks fill and its window reads; *requests* are fields the window sets and the hooks apply and clear.
- **Viewport layer** — a tool's drawing in the viewport (labels, rings, the heuristic gizmo), switched by `tools.<tool>.*` values.
- **Window** — an ImGui window in `src/ui` that draws a tool's or the engine's state; every window's data is also a record.

## Renderer

`src/renderer/README.md`, `src/rendertools/README.md`.

- **At-start / first-use pipeline** — a pipeline whose shaders are requested at start-up, or the first time it is asked for (the debug views).
- **Bucket** — one of four pipeline groups per view: single-sided, double-sided, and the same two for cutout materials.
- **Buffer view** (G-buffer view) — what the lighting pass shows (`view` verb, `--view`): lit, albedo, normals, depth, shadow views, worldpos,
  mip level, cascades, LOD level, hzb.
- **Capture / capture watch** (`CaptureService`, `CaptureWatch`) — a copy of any frame-graph resource after a pass, read back after the fence;
  picking, the GPU Scene window and dumps use it.
- **Cascades / shadow atlas** — cascaded shadow maps (three by default) as tiles of one atlas, fitted on the main thread.
- **Cull code** — per instance and view, the frustum plane that rejected it, or `D` (render-LOD level not drawn), `E` (visible), `F` (not tested).
- **`CullResult`** — culling's readback handed to the main thread: counts per view, meshlet counts, triangles.
- **Culling** — per instance (frustum, then occlusion), then per meshlet in the task shader (frustum, normal cone, occlusion).
- **Cutout material** — a material with `opacityThreshold > 0`: drawn where its opacity reaches the threshold, through `discard`, no blending.
- **Debug view** — what `DebugViewPass` draws (`debugview` verb): wireframe, triangle size, overdraw, instance, mesh, material and primitive ids,
  UV, streaming, meshlet, meshlet culling.
- **Deferred shading** — the geometry pass writes the G-buffer; a fullscreen lighting pass shades it.
- **Deletion queue** — the renderer's deferred destroys, each held until the frame that last used the resource has passed its fence.
- **Depth pyramid / HZB** — occlusion's hierarchical depth: each level the farthest depth of the 2×2 below.
- **Draw log** — per-draw records read back while the render debugger asks; lags a frame-slot cycle.
- **Draw record** (`DrawRecord`) — one per drawn instance per region: instance, meshlets, first work item.
- **Frame graph** — a frame's passes and resources: orders passes, culls unused ones, pools transient textures and records every barrier.
- **Frame slot** — the per-frame resources (command buffer, fence, UBO, staging, culling and readback buffers), one per swapchain image;
  everything the CPU shows about the GPU arrives one slot cycle late.
- **G-buffer** — albedo, normals, instance id and depth, written by the geometry pass.
- **Geometry pool** — one vertex buffer, one position buffer and the meshlet buffers for every mesh; ranges per mesh, grows by doubling.
- **GPU-driven** — the only path: persistent GPU tables, visibility and draw lists made on the GPU, one indirect call per region.
- **GPU scene** (`GpuScene`) — the scene's GPU tables: geometry pool, mesh table, material table, texture array, instance buffer, LOD groups.
- **Imported / transient resource** — a frame-graph resource from outside the graph (backbuffer, persistent buffers; all buffers are imported),
  or a texture the graph allocates and pools for the frame.
- **Instance** (GPU) — one draw slot's record in the instance buffer (`GpuInstanceRecord`, 128 bytes, with a 48-byte culling record after the
  draw records). Not a USD instance.
- **Meshlet** — at most 64 vertices and 124 triangles of one submesh, with a bounding sphere and a normal cone; the unit the renderer draws
  and culls.
- **Meshlet pack** — survivors of the task shader packed into one mesh workgroup (up to 64 vertices and 124 triangles). Not an asset pack.
- **Mesh shader / task shader** — the mesh shading stages: the task shader culls 32 meshlets and packs the survivors; the mesh shader emits a
  pack's triangles. Every mesh pass draws this way; there is no vertex-shader path.
- **Occlusion culling, two-phase** — the first phase draws what showed last frame (the *history*), builds the depth pyramid, and the second
  phase draws what the pyramid newly shows (view 5). Runs in the geometry pass by default, in the depth prepass when it is on.
- **Ordered compaction** — culling's reduce-then-scan that writes draws in instance order, so output is deterministic (no atomic appends).
- **Pass** — a node of the frame graph (`SceneUpload`, `InstanceCull`, `ShadowPass`, `GeometryPass`, `LightingPass`, `AAPass`, …).
- **Pipeline registry** — creates every pipeline from shader ids; nothing waits for a shader, a pass stays out until its pipelines are
  ready, and shaders hot-reload on change notices.
- **Readback** — GPU data copied into a host-visible buffer and read after the slot's fence.
- **Region** — one (view, bucket) pair: 6 views × 4 buckets = 24 regions, each drawn with one `drawMeshTasksIndirect`.
- **Render extension** (`RenderExtension`) — how what isn't the renderer's core joins its frame at fixed hooks: `RenderTools`, `GizmoRender`,
  `UiRender`. Their per-frame data rides in `RenderSnapshot::extensions`.
- **Render thread** — owns the GPU: takes one snapshot a frame, applies scene changes within the upload budget, builds, records, submits,
  presents.
- **Render tools** — the renderer's instrumentation as a render extension: debug snapshots, the frame debugger, counters, the texture
  inspector and dumps, screenshots, Tracy frame images. Free when off.
- **`RenderSnapshot`** — the main thread's per-frame hand-off to the render thread: camera, view modes, culling frustum, cascades, debug lines,
  extensions' data. One slot with back-pressure.
- **`RenderWorld`** — the renderer's scene input: draw slots, LOD groups, lights, environment.
- **Reversed-Z** — camera depth `near / distance`, 1 at the near plane and 0 at infinity; no far plane.
- **Texture slot** — a texture's index in the bindless 1024-texture array; slot 0 is the fallback, slot 1 white.
- **Upload budget** — bytes of queued scene uploads a frame takes (`render.upload.budgetMiB`, 64 MiB).
- **View** (culling) — 0 the camera, 1–4 the shadow cascades, 5 the camera's second occlusion phase.
- **Work item** — one `{instance, meshlet}` pair in a region; a task workgroup takes 32 consecutive ones.

## RHI

`src/rhi/README.md`.

- **Backend** — the one graphics API implementation linked per platform (`src/rhi/vulkan/`); the only code that includes that API.
- **Barrier / state** — synchronisation as resource-state transitions (`RhiTextureState`, `RhiBufferState`); the backend derives layouts,
  stages and accesses. The caller tracks state.
- **Desc** — a `*Desc` struct with defaults, filled with designated initializers, that `RhiDevice::createX` takes.
- **Frame pacing** — command buffers, fences and semaphores per frame slot and per swapchain image; the integrator's, not the RHI's.
- **GPU zone** — a named, nested timestamp range on a command buffer (`beginGpuZone`/`endGpuZone`); the profiler's GPU lane.
- **Integrator** — whatever sits on the RHI (the engine, an example); provides the window, frame pacing, uploads, deferred destruction.
- **Lavapipe** — Mesa's software Vulkan driver, which CI runs the RHI examples on.
- **RHI** — render hardware interface: a thin, header-only abstraction that executes and never schedules, waits or caches.
- **`RhiWindow`** — the window layer's contract with the RHI (instance extensions, `createSurface`); no window system code under `src/rhi/`.

## Runtime access: RPC, records, trace, profile, configuration

`src/rpc/README.md`, `src/introspect/README.md`, `src/trace/README.md`, `src/profile/README.md`, `src/config/README.md`.

- **Command** (engine command) — a method, verb or record of the engine, registered by domain in `src/view/*commands.cpp`; the tools and the UI
  register their own.
- **Configuration value** — a typed setting (`config::Bool`, `Int`, `Float`, `Enum`, `Vec3`) declared in code by its owner with a dotted
  name (`render.shadow.cascades`), a default and a description. Read and set through the `config` record, `config.*` methods, the `config`
  verb and the Configuration window. Its *group* is the name's first two parts.
- **Endpoint** (`RpcEndpoint`) — one per long-running process: an RPC server, a discovery file, main-thread dispatch and the trace stream.
- **Event** (trace) — `TRACE_EVENT` / `TRACE_WARNING` / `TRACE_ERROR` with a category (`Scene`, `Render`, `Engine`, `Asset`), a CamelCase
  type, a stable name, a text and fields.
- **Frame budget** — the frame time content is measured against (`profile.budget.frameMs`, 16.7 ms); the frame budget overlay (Ctrl+B) shows
  the GPU's passes by group as bars against it.
- **Group** (profile) — a zone's colour and category: Frame, Scene, Assets, Jobs, Ui, Tools, Render, Culling, Geometry, Shadow, Lighting, Post,
  Upload, Io, Wait.
- **Introspection** — reading and driving a running process from outside: records, methods and the trace through `ngen-introspect`.
- **Lane** — one thread's zones in the profiler (Main, Render, Scene, Worker N, RPC I/O, …), plus the GPU lane.
- **Level** (profile) — *guide* zones (the overview, shown by default) or *detail* zones (everything finer); both always recorded.
- **Level** (trace) — `info` (flow), `warning` (worked around), `error` (failed). A clean run has no warnings or errors.
- **Method** — a JSON-RPC method named `area.verb` (`view.camera.set`) with typed parameters; `rpc.describe` lists them.
- **Plot** — a value over time sampled where produced (frame times, uploads, GPU busy), kept with the zones and sent to Tracy.
- **Record** — one named piece of a process's data as JSON (`status`, `culling`, `streaming`, …), read with `introspect.get`. Data is a
  record, never a trace event. `record <name> <file>` writes one at an exact frame.
- **Responder** (`RpcResponder`) — how a handler answers, at once or frames later (a screenshot, a readback).
- **Ring** — a process's always-on buffer of its last 65,536 trace events.
- **RPC** — JSON-RPC 2.0 over loopback TCP, frames with optional binary attachments; how processes and agents talk.
- **Site** — a profiler marker: its name, file, line, function, group and level.
- **Trace** — a process's flow and messages (not data), always on, printed to its console and streamed over RPC to subscribers. The console is
  the trace.
- **Tracy** — the external profiler, a second sink inside the collector for whole sessions; nothing in the engine reads back from it.
- **Verb** — a script and command-line word (`camera`, `lod`, `record`) that maps to a method; script lines and live calls run the same code.
- **Zone** — a timed CPU or GPU range from a marker (`PROFILE_ZONE`, `PROFILE_GPU_ZONE`); frame-graph passes, RPC calls, jobs and render
  extension hooks get one by themselves.

## Project and process

`AGENTS.md`, `docs/README.md`.

- **ADR** — an architecture decision record in `docs/adr/`, Nygard format, numbered in decision order; superseded records are kept.
- **Plan** — `docs/plan_<topic>.md`: status, current state, scope, steps, visibility, verification. *Landed*, *in progress*, *superseded*.
- **Review** — a review artifact in `docs/reviews/`.
- **Roadmap** — `docs/roadmap.md`: planned, open and parked work. Items *parked* with a *trigger* that would bring them back.
- **Runtime access and visibility** — the standing rule that everything is reachable by an agent (records, methods, verbs) and visible to a
  human (a window, view or overlay).
- **Test scenes** — `assets/three_cubes.usda` (smoke test), `assets/layered/root.usda` (composition), Intel's New Sponza (materials), the test
  world (streaming), Caldera (large; no longer the streaming target).
- **Vertical slice** — the roadmap's order of work: the thinnest working version of every piece (streaming, meshlets, editor, gameplay,
  physics, animation, rendering gaps, a first game) before depth.
- **Visibility pass** — the check at the end of each vertical-slice step over what it touched: records, methods, verbs, windows, and where it
  sits in the engine's flow.

## Words with more than one meaning

| Word | Meanings |
|---|---|
| bucket | a culling bucket (single/double-sided × opaque/cutout); nothing else |
| cell, region, tile | test-world streaming areas; *region* is also a culling (view, bucket) pair, *tile* a shadow-atlas cascade tile |
| component | a World component (lights, LOD roots, instancers, payloads); a future gameplay component; a UI or software component in prose |
| content | a World content (stage, prototype, level); "content developers" means the people authoring worlds |
| frame | the main thread's `ViewFrame`; a render frame on a frame slot; an RPC frame (length-prefixed message); a script's frame number |
| group | a render-LOD group (GPU-picked LOD root); a profile group (zone colour); a config group (name prefix); a workgroup of 256 culling slots (group box) |
| history | occlusion culling's per-instance visibility from last frame; the profiler's seconds of zones (`profile.history.seconds`) |
| hooks | `ViewHooks` (main-thread frame steps); `RenderExtension` hooks (render-thread frame points) |
| instance | a USD instance (shares a prototype); a GPU instance (one draw slot's record); a point of a PointInstancer is neither |
| level | a LOD level; a mip level; a trace level (info/warning/error); a profile level (guide/detail); a depth pyramid level |
| pack | to pack (the process); a pack (a packed scene file: layer, mesh, material pack); a meshlet pack (task shader output) |
| pass | a frame-graph pass (GPU work); a placement pass (scene thread) |
| phase | occlusion culling's first and second phase |
| record | an RPC record (named JSON data); a GPU instance, culling or draw record; an asset cache record; the `record` verb |
| resident | the scene being resident (everything loaded, uploaded, drawn); a LOD level being resident (loaded by residency) |
| root | a LOD root; the root layer; the asset server's working directory (asset ids are relative to it) |
| slot | a frame slot; a draw slot; a texture slot; a streaming root's slot (streaming view) |
| snapshot | `SceneSnapshot` (scene thread → main); `RenderSnapshot` (main → render); a debug snapshot (render tools → main) |
| state | a resource state (RHI barriers); a streaming state (finest resident level); a tool's state struct |
| tag | the LOD tag (normal alpha); a GPU zone's tag (profiler site id) |
| variant | a build variant (platform/config); a USD variant (of a variant set) |
| view | ngen-view; the `view` library; a culling view (camera, cascades, phase 2); a buffer view (`view` verb) or debug view (`debugview` verb) |
| version | an asset version (content hash); a World content's version (each compose); a pack rule's version; the IR and scene-pack format versions |
