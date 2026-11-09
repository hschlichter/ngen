# Detailed profiling: history by time, zone groups and levels, every thread

**Status. Landed.**

## Current state

Profiling is `src/profile/` ([ADR 23](adr/0023_profiling_markers.md)) with Tracy as a second sink inside the collector ([ADR 81](adr/0081_tracy_as_second_sink.md),
[plan_tracy.md](plan_tracy.md)). About 100 CPU zones, sized for a guide: 74 per frame in Sponza. GPU zones per frame-graph pass and per draw
region. The collector keeps a fixed ring of 16,384 zones per thread and 1,024 frames, and the GPU history cuts at 64 zones per frame. Zones
have a name only: no group, no colour, no level. Threads named today: Main, Render, Scene and the job workers; the RPC endpoint's I/O thread,
the asset client's I/O thread and the trace stream's thread have no name and no zones, so Tracy does not show them. Only `ngen-view` links
`profile`: the asset server, the packers, `ngen-pack`, `ngen-introspect` and the other tools have no profiling at all.

A Tracy capture shows too little: which work runs, on which thread, and where a thread waits is not readable.

## Goals

1. Much more detailed markers on the CPU and the GPU, so what executes is visible.
2. Colour by group: frame steps, render, scene, assets, jobs, UI, waits.
3. Every thread the view starts is visible, idle or not.
4. **Everything goes through the profile system.** The built-in collector keeps all of it, detail included, for a configurable time (10 seconds
   by default); agents read it from records and dumps without Tracy. Tracy shows the same zones.
5. The Performance window stays a guide: it shows the guide zones by default and the detail on request.
6. `profile` is a foundation library, like `trace`: every program links it, and the asset server and the packers are profiled too.

Plots (renderer stats, GPU usage, memory) are [plan_profile_counters.md](plan_profile_counters.md); frame images are
[plan_tracy_frame_images.md](plan_tracy_frame_images.md). Both use the groups and the time-based history from this plan.

## Decisions

Locked with Henrik on 2026-11-09.

1. **History kept by time, in per-thread chunks dropped by age.** A config value `profile.history.seconds` (default 10). Each lane writes into
   blocks allocated as needed; a block whose newest zone is older than the window is recycled. Memory follows the actual zone rate and the history
   is exactly the window, whatever the detail. Weighed: a fixed capacity in zones, configurable (simpler, but the seconds covered change with the
   zone rate). Frames and GPU frames move to the same window, and the GPU history loses its 64-zones-per-frame cut.
2. **A zone has a level, not a route.** Every zone goes to the built-in collector and to Tracy. A name registers with a level, guide (today's
   zones) or detail (`PROFILE_ZONE_DETAIL`); the Performance window shows guide zones by default, with a toggle for detail. Records and dumps carry
   everything.
3. **A zone has a group, and the group is its colour.** Every marker names a group; the colour is the group's, in Tracy and in the Performance
   window. Waits are grey. Jobs get a shade of the Jobs colour by job name.
4. **GPU detail behind a config value, off by default.** `profile.gpu.detail`. A GPU zone writes timestamps at the all-commands stage, which makes
   the GPU wait for earlier work there; many small GPU zones change the timings they measure. GPU guide zones (per pass, per draw region) stay on.
5. **Agents read it from records and the dump.** The `zones` record carries each zone's group and level; `dump-profile` takes how many seconds
   and which level, since ten seconds of detail can be hundreds of MB of JSON.
6. **`profile` is a foundation library.** `rpccore`, `trace` and `packer` link it, so every program does, with Tracy's client (in debug and
   release each program listens on its own port, on demand; Tracy's GUI lists them). The I/O threads get zones inside their loops, and the
   asset server and the packers are instrumented. Weighed: a thread-start hook in `rpccore` and `trace` (no new dependency, but a name and a row
   only, nothing inside the loops, and the tools stay unprofiled).
7. **Tracy's client starts by hand** (found while building, locked with Henrik on 2026-11-09). Its start-up sleeps 200 ms to calibrate its
   timer and its shutdown takes about as long: with every program linking it, a packer run went from 1.2 ms to 350 ms, and ngen-view's start
   had waited 200 ms since [plan_tracy.md](plan_tracy.md). Debug and release add `TRACY_MANUAL_LIFETIME` and `TRACY_DELAYED_INIT`;
   `profile::startTracy()` starts the client on a background thread in the long-running programs (ngen-view, the asset server),
   `profile::startTracyIfAsked()` starts it in the packers and tools when `NGEN_TRACY=1` is set, and it stops at exit. Weighed: starting it
   only when asked everywhere (no cost anywhere, but connecting to a running view means restarting it), and keeping it always on.

## Scope

**In**

- `src/profile/`: groups and levels on names, the time-based chunked history, the two config values, `PROFILE_ZONE_DETAIL` and
  `PROFILE_GPU_ZONE_DETAIL`, Tracy source location colours, a zone when a thread registers.
- Every `PROFILE_ZONE`/`PROFILE_GPU_ZONE`/direct `beginZone` site gets a group (about 100 sites).
- The instrumentation pass: automatic zones where a name already exists, hand-placed detail zones, and waits as Wait zones (Steps 6–8).
- Every thread the view starts named and given a zone.
- `rpccore`, `trace` and `packer` link `profile`; every program links `profile` and `tracy`.
- The asset server and the packers: threads named, zones on their work, and the profile records on the asset server's endpoint.
- The profile records and the dump method moved out of `src/view` so any process with an endpoint registers them.
- The `zones` and `profile` records, `dump-profile`'s parameters, and the Performance window's detail toggle and colours.
- `src/profile/README.md`, `src/rhi/README.md` (the zone cap), and the READMEs of the libraries that gain markers where they describe the flow.

**Out**

- Plots, memory and GPU usage ([plan_profile_counters.md](plan_profile_counters.md)); frame images ([plan_tracy_frame_images.md](plan_tracy_frame_images.md)).
- Threads the engine doesn't start (the driver's, SDL's, Tracy's own). They appear in Tracy only through its context-switch capture, which needs
  root or a lowered `kernel.perf_event_paranoid`: a machine setting, documented in the profile README, not code.
- The asset server's messages as structured trace events (they are text lines today): the same foundation argument for `trace`, a separate
  item in `docs/roadmap.md` ("Introspection follow-ups").
- Zones inside `ngen-introspect`'s window and `ngen-cli`: they link `profile` through `rpccore` and name their threads, nothing more.
- File and line on zones in round 1 (round 2 adds them).

## Steps

1. **Groups and levels on names.** `src/profile/profile.h`:

   ```cpp
   enum class Group : uint8_t {
       Frame,     // the main loop's steps
       Scene,     // the scene thread: composing, extracting, placing
       Assets,    // asset requests and arrivals
       Jobs,      // job system work; each job name a shade
       Ui,        // windows and overlays
       Tools,     // LOD and Streaming tools, the debug camera, gizmos
       Render,    // the render thread's CPU work
       Culling,   // GPU culling, the depth pyramid
       Geometry,  // depth prepass, geometry, debug views
       Shadow,
       Lighting,  // lighting, sky
       Post,      // anti-aliasing, overlays, present passes
       Upload,    // uploads and copies, CPU and GPU
       Io,        // RPC, trace stream, asset client I/O
       Wait,      // blocking: fences, snapshots, condition variables, idle
   };
   enum class Level : uint8_t {
       Guide,
       Detail,
   };

   auto registerName(const char* name, Group group, Level level = Level::Guide) -> uint32_t;
   auto groupOf(uint32_t nameId) -> Group;
   auto levelOf(uint32_t nameId) -> Level;
   auto groupColor(Group group, uint32_t nameId) -> uint32_t; // 0xRRGGBB; Jobs vary by name
   ```

   A name keeps the group and level of its first registration; a second registration with others asserts in debug builds. The Tracy source
   location of a name gets the group's colour. `Zone` is unchanged: its group and level are its name's.

   ```cpp
   #define PROFILE_ZONE(name, group) ...        // Level::Guide
   #define PROFILE_ZONE_DETAIL(name, group) ... // Level::Detail
   #define PROFILE_GPU_ZONE(cmd, name, group) ...
   #define PROFILE_GPU_ZONE_DETAIL(cmd, name, group) ... // nothing recorded unless profile::gpuDetail()
   ```

   GPU zone names are registered with their group at the marker (`profilegpu.h` keeps the id per call site where the name is a literal; the frame
   graph registers each pass's name once at `addPass`, which gains a `profile::Group` argument). `Renderer::readGpuTimings` already maps names to
   ids.

2. **History by time.** `src/profile/profile.cpp`. A lane becomes a list of blocks:

   ```cpp
   struct ZoneBlock {
       std::array<Zone, 4096> zones;
       std::atomic<uint32_t> count = 0; // published entries; readers copy below it
       uint64_t newestEndNs = 0;
   };
   struct Lane {
       std::string name;
       std::mutex blocksMutex; // taken by the writer only when it starts a block, by readers to copy the list
       std::vector<std::shared_ptr<ZoneBlock>> blocks;
       std::shared_ptr<ZoneBlock> current;
   };
   ```

   The writer appends to `current` without a lock. When it is full, it takes the mutex, drops blocks whose `newestEndNs` is older than the window
   (a dropped block no reader holds is reused, otherwise a new one is allocated) and starts the next. Readers copy the `shared_ptr` list under the
   mutex and read outside it. Frames and GPU frames become deques under their existing mutexes, evicted by age; a GPU frame keeps every zone.
   `frameHistorySize`, `ringSize` and `maxGpuZonesPerFrame` go.

3. **Config values.** `src/profile/profileconfig.cpp` declares `config::Float historySeconds{"profile.history.seconds", 10.0, …, {.min = 1,
   .max = 120}}` and `config::Bool gpuDetail{"profile.gpu.detail", false, …}`; `profile` links `config`. Values belong to the main thread:
   `ViewFrame` hands them to the collector on a change (`profile::setHistorySeconds`, `profile::setGpuDetail`, atomics read by any thread), as
   `sendSceneInputs` does for the upload budget.

4. **RHI zone cap.** `RhiCommandBufferVulkan::maxGpuZones` 128 → 1024 (2,048 timestamps per command buffer). `src/rhi/README.md` says so.

5. **Every thread visible.** `profile::registerThread` also opens and closes a zero-length `ThreadStarted` zone (group Wait), so each named thread
   has a row in Tracy from its start. `RpcServer` takes a thread name at `start` (the view's endpoint "RPC I/O", the asset client's "Asset I/O",
   the asset server's "RPC I/O"); `TraceStream` names its thread "Trace I/O". Inside `RpcServer::ioLoop`: the poll as a Wait detail zone, each
   frame read and written as an Io detail zone. `TraceStream::loop`: its wait and each batch sent.

5a. **Foundation linking.** `build.cpp`: `rpccore`, `trace` and `packer` `.link(profile)`; every program that links any of them also links
   `profile` and `tracy` (static libraries are not passed on transitively). `profile` links `tracy` and `config` only, so there is no cycle.

5b. **The asset server and the packers.**
   - Asset server (`src/asset/server/`): thread names for its pack workers ("Pack N"), the file watcher, the cache saver; zones on a request,
     a pack task (named after its rule), a packer run (the wait for the child process, group Wait), the cache's save and load, change notices.
     It has no frames, which Tracy and the collector handle: its history is by time.
   - Packers (`src/apps/packer*.cpp`, the packer contract in `src/asset/pack/packer.cpp`): a zone per stage the contract runs (read inputs,
     pack, write output) and inside the USD packer per prim kind and meshlet building; mip chain workers named ("Mip N").

5c. **Profile records anywhere.** The record builders and the dump move from `src/view/profilecommands.cpp` to
   `src/profile/rpc/profilecommands.cpp`, a small `profilerpc` library (links `profile` and `rpccore`): `registerProfileRecords(RpcRecords&)` for
   the `profile` and `zones` records, and `dumpProfile(const rpc::Json& params) -> std::expected<rpc::Json, std::string>` for the dump. The view
   keeps its `introspect.profile` method and `dump-profile` verb in `RpcRegistry`, calling `dumpProfile`; the asset server dispatches
   `introspect.profile` to it in its request handler, beside `asset.request`.

6. **Waits as Wait zones.** Group Wait for what blocks: `WaitSnapshot`, `WaitFence`, `Acquire`, `Present`, `SubmitSnapshot`'s wait for a slot,
   `JobWait`; new detail zones around the job workers' idle wait (`Idle`), the scene thread's wait for requests, and the main loop's frame pacing.

7. **Automatic zones (detail).** Names that already exist become zones at one place each:
   - each RPC method call in `RpcEndpoint::drain` (group Io; the method's name is registered when the method is added);
   - each window drawn in `src/ui/viewui.cpp` (group Ui; a zone per `drawXWindow` call, named after the window);
   - each render extension hook in `Renderer::render` (group Render; `RenderTools.beginFrame`, `Gizmos.addOverlayPasses`, …);
   - each `ViewFrame` step that has none yet (group Frame);
   - the frame graph's CPU record zone per pass gets its pass's group.

8. **Hand-placed detail zones.**
   - Scene thread (`src/view/viewscene.cpp`): compose, extract, placement passes, residency updates, asset arrivals (groups Scene, Assets).
   - Main thread: asset client takes and arrivals (`src/asset/assetclient.cpp`, group Assets), LOD and streaming selection (`src/tools`, group
     Tools), snapshot build stages.
   - Render thread (`src/renderer/`): upload batching, GPU scene table updates, draw list building, readback parsing, pipeline creation arrivals
     (groups Render, Upload).
   - GPU detail (`PROFILE_GPU_ZONE_DETAIL`): instance culling and both meshlet culling phases, each depth pyramid level, each shadow cascade,
     upload copy batches, the lighting and sky passes' sub-steps where they have them.

   The first pass covers these; more follow where a capture shows a gap.

9. **Readers.**
   - `zones` record: each zone gains `group` and `level`; a `level` parameter is not needed (both are listed).
   - `profile` record gains `historySeconds`, `historyBytes` (blocks held), and zones held per lane.
   - `introspect.profile {path, seconds?, level?}` and the verb `dump-profile <path> [seconds] [guide|detail]`: the last `seconds` of history
     (default all), guide zones only or everything (default everything). Each event's `args` gain `group`; `cat` is the group's name.

10. **Performance window** (`src/ui/performancewindow.cpp`): zones in their group's colour; a "Detail" toggle, off by default; the history's
    length from the window's scrub range follows `profile.history.seconds`.

11. **Docs.** `src/profile/README.md` (groups, levels, the history, the config values, every thread, context switches as a machine setting); the
    flow paragraphs in `src/renderer/README.md`, `src/view/README.md` and `src/rpc/README.md` where their new zones sit.

## Built

Where the code differs from the steps above:

- **Lanes are reused by name** (`profile.cpp`, `laneForThread`): a thread's lane is freed when it exits, and a thread registering the same name
  takes it over. The texture packer's mip chain starts its workers for every level, which would have used up the 64 lanes.
- **Idle waits are sliced** (`profile::idleSlice`, 100 ms): the job workers, the scene thread, the pack workers and the RPC poll wait in
  slices, each an `Idle` (or `RpcPoll`) zone. A thread idle since start had no closed zone, so no row in Tracy and nothing in the `zones` window.
- **Thread names before Tracy starts**: `registerThread` sets the system thread name (15 characters); Tracy reads it for threads named before
  its client started, since naming a thread through Tracy before it starts crashes with `TRACY_DELAYED_INIT`.
- **Several Tracy clients**: every program listens, the first on port 8086. A headless capture names its program's port (`TRACY_PORT` on the
  program, `tracy-capture -p`); the profile README says so.
- **Names are copied** by `registerName` (into a deque), so RPC method names and `"<extension>.<hook>"` names need no static storage.
- **Groups chosen**: the draw-region GPU zones are Geometry in every pass (one name, one group, also in the shadow pass); `SceneWindowLists`
  is Ui in both the tools and the UI; `SubmitSnapshot` is Wait.
- **GPU detail zones**: `HzbLevelN` per depth pyramid level, `CascadeN` per shadow cascade, `UploadBatch` and `UploadCopies` in the GPU scene's
  uploads. The instance culling passes were already one GPU zone each; the meshlet culling runs inside the task shader and has no zone.
- **`src/editor`** still uses the old one-argument macros: it is unbuilt, and gets groups when `ngen-editor` is built.

## Visibility

- **Records**: `zones` (group and level per zone), `profile` (`historySeconds`, `historyBytes`, zones held per lane), `config` (the two new
  values).
- **RPC methods and verbs**: `introspect.profile` and `dump-profile` gain `seconds` and `level`; `config.set profile.history.seconds` and
  `profile.gpu.detail` (and the `config` verb) set the values.
- **Windows**: the Performance window (colours, Detail toggle); the Configuration window lists the new values.
- **Tracy**: the same zones with their colours; every named thread from its start.
- **The asset server**: its endpoint gains the `profile` and `zones` records and the dump (`ngen-cli introspect get asset zones`).
- **Gap, named**: a packer is a short-lived child process without an endpoint; its zones reach Tracy (a client for as long as it runs) and its
  run time is in the asset server's records, but its zones are not readable without Tracy.

## Verification

Results (2026-11-09, release unless said, this machine) follow each criterion in **bold**.


- Headless, release, Sponza, a script recording `profile` and `zones` at frame 600 and `dump-profile` of the last 2 seconds:
  - `zones` lists every group from the enum that has work in Sponza (Frame, Scene, Jobs, Ui, Render, Culling, Geometry, Shadow, Lighting, Post,
    Upload, Io, Wait), and zones at both levels. **12 groups over the 60-frame window at steady state (Culling Frame Geometry Io Lighting Post
    Render Shadow Tools Ui Upload Wait), 114 guide and 37 detail names; Scene, Assets and Jobs have work only while loading and are in the dump
    of a three_cubes run (Assets 83, Scene 295 zones).**
  - Lanes in `profile`: Main, Render, Scene, every worker, RPC I/O, Asset I/O, Trace I/O. **All of them, 29 threads and the GPU; every lane has
    zones in the 60-frame window.**
  - `historyBytes` stays within 10% between frame 600 and frame 1200 (the window evicts); with `config profile.history.seconds 2` set at frame
    600, the dump at frame 1200 spans 2 seconds (±1 frame). **Measured at 12 s and 18 s instead (the window must have filled): 18.80 MB and
    18.80 MB, 2,510 frames held. After 2 s was set: 501 frames, 9.2 MB (blocks leave as lanes start new ones). The 2-second dump's zone ends
    span 2.007 s; its zone starts span 2.88 s, because zones overlapping the window are included whole.**
- A Tracy capture (`tracy-capture`, `tracy-csvexport -u`) has a row for every thread named above and the zone names of the `zones` record; the
  CPU zones per frame are at least 5× today's 74 in Sponza. **Rows for all 29 threads and every one of the record's 131 names (Tracy has 175).
  Not met: 109 CPU zones per frame in Sponza, 1.5× the 74. A capture connected 10 s after start has the GPU lane (75,000 GPU zones in 3 s) and
  all 29 threads.**
- `profile.gpu.detail` off: GPU zone count per frame equals today's; on: the culling, pyramid and cascade zones appear, nested in their passes.
  **Off: 39 GPU zones per frame; on: 54, with `HzbLevel0`–`11` and `Cascade0`–`2` at depth 1 inside their passes.**
- Cost: the zone loop microbenchmark from [plan_tracy.md](plan_tracy.md) stays within 10 ns of its 67 ns per zone (Tracy compiled in, no
  server). Sponza's `cpuMsAverage` with GPU detail off is within the noise of today's (3.99 ms). **Pinned to one core, best of 7: 75 → 82 ns
  with Tracy, 37 → 44 ns without. Sponza 3.98–3.99 ms. ngen-view's main loop starts 40 ms after launch. A packer run: 1.8 ms (gamerelease
  1.2 ms); with `NGEN_TRACY=1`, 350 ms.**
- The asset server, after the Sponza run: its `zones` record lists pack tasks, packer runs and request zones on named lanes ("RPC I/O",
  "Pack N", the file watcher); after a cache wipe, a `tracy-capture` of the asset server (`-p` its port) has the same names. **Debug asset
  server: lanes Main, Cache Saver, Pack 0–23, File Watcher, Trace I/O, RPC I/O; its dump holds `Pack` 25, the rule zones `shader` 20 and `usd`
  5, `PackerRun` 21, `AssetRequest`, `CacheRecord`, `CacheSave`, `Deliver`. The `zones` window (the last 10 s) held only I/O zones by then. The
  Tracy capture of the asset server was not run.**
- Trace: no warnings or errors on the runs above. The Performance window's colours and Detail toggle are Henrik's visual check. **No
  warnings or errors from the view; the asset server's two Sponza warnings (translucent glass drawn opaque) are the scene's, printed on its
  repack. Debug runs assert nothing (no name registered with two groups). Example sweep: all 17 exit 0. gamerelease: no Tracy in the binary.**

## Round 2: source locations, draws named for what they draw, coverage

Henrik's look at the first round in Tracy (2026-11-09): the depth stops too early and the names don't lead to the code. On the GPU,
`camera.single` is a 3.4 ms block whose name says nothing about what it draws; the shadow cascades help a little. On the scene thread, `Relink`
(82 ms) and `PlaceSync` (14 ms) are one or two levels below their parent with nothing under them, because the work is in `src/world`
(`WorldComposer`) and the placement code, which round 1 never entered. The 5× zone count measured the wrong thing: what matters is that time is
accounted for, down to the code.

### Decisions

Locked with Henrik on 2026-11-09.

8. **Every zone has its file, line and function.** A marker site is its own zone kind, not a name: the macros capture `std::source_location`;
   `registerName` and `FrameGraph::addPass` take one as a default argument, so the location is their caller's. Two sites with the same name
   (the `Idle` waits, a draw in four passes) stay apart, and the rule that a name keeps one group goes. Tracy gets the location (its source view
   and "go to source" work); the records, the dump and the Performance window show `file:line`.
9. **On the GPU, the pass is the level, and a draw is named for what it draws.** Timestamps cannot split a draw, so a pass's draws are its
   leaves. The draw-region zones are named by their bucket (`Opaque`, `Opaque two-sided`, `Cutout`, `Cutout two-sided`); the pass above says
   whose view (the geometry passes the camera, the shadow pass a `Cascade N` zone, now a guide zone, per cascade). The location points at the
   draw call.
10. **Coverage, not counts.** Every zone above a threshold has child zones accounting for at least 80% of its time; descent stops at a function
    that does one thing, or at a library or driver call (a `vkQueueSubmit`, an OpenUSD call), which is then the zone. Thresholds: 0.05 ms per
    frame on the main and render threads; 1 ms per call on the scene thread and in the asset server. A zone that stays under 80% is named in
    the plan with why.
11. **Self time in the `zones` record**, so the gaps show without Tracy: each zone's self time and the share its children cover.

### Steps

R1. **Sites.** `src/profile/profile.h`:

    ```cpp
    // A marker site: where a zone is opened, with its name, group and level. Zones and the GPU lane carry the site's id.
    struct SiteInfo {
        const char* name = "";
        const char* file = "";     // as compiled, relative to the project root
        const char* function = "";
        uint32_t line = 0;
        Group group = Group::Render;
        Level level = Level::Guide;
    };

    auto registerSite(const char* name, Group group, Level level = Level::Guide,
                      std::source_location location = std::source_location::current()) -> uint32_t;
    auto siteOf(uint32_t siteId) -> const SiteInfo&;
    ```

    `registerName` becomes `registerSite` (one per `(file, line, name)`; names copied, locations static). `Zone::nameId` becomes `siteId`.
    The macros pass `std::source_location::current()` from the line they are on. Tracy's source location per site gets the file, line and
    function; `nameOf`, `groupOf` and `levelOf` take a site id.

R2. **GPU sites through the RHI.** `RhiCommandBuffer::beginGpuZone(const char* name, uint32_t tag = 0)`; `RhiGpuZone` gains `tag`, returned as
    given: a number for the caller, which the RHI does not read. `ScopedGpuZone` registers its site and passes the id as the tag;
    `Renderer::readGpuTimings` uses the tag instead of looking the name up. `FrameGraph::addPass` keeps its caller's location for the pass's CPU
    record zone and GPU zone. `src/rhi/README.md` and the `gpuzones` example cover the tag.

R3. **Draws named for what they draw.** `DrawLists::bucketName(bucket)` (`Opaque`, `Opaque two-sided`, `Cutout`, `Cutout two-sided`) for the
    region zones in the depth prepass, geometry, debug view and shadow passes; `regionName` stays for the draw log and the culling record.
    The shadow pass's `Cascade N` zones become guide zones, around their four draws.

R4. **Self time and coverage.** The `zones` record, per site: `file`, `line`, `function`, `selfMs` (total minus its children's), and
    `coveredShare` (its children's time over its own, 0–1). The dump's `args` gain `file` and `line`. The Performance window's zone details show
    `file:line` and self time.

R5. **Coverage pass.** Instrument down to the rule on:
    - the streaming test world's flight (`assets/streamtest-world/world/root.usda`, `fly /cameras/flight_ 30`): the scene thread (`ViewScene`,
      `WorldComposer` in `src/world`: compose, relink, flatten; placement, LOD evaluation, streaming residency), the main thread's per-frame
      steps, the render thread's uploads;
    - Sponza: the render thread (frame graph build, record, the passes' recording) and the main thread;
    - the asset server and the USD packer while the test world packs after a cache wipe.

    `src/world` links `profile` (it already sits above it). The zones that the rule leaves under 80% are listed here with why.

R6. **Docs.** `src/profile/README.md` (sites, locations, self time), `src/world/README.md` (where its zones sit), `src/rhi/README.md` (the tag).

### Visibility

- **Records**: `zones` gains `file`, `line`, `function`, `selfMs`, `coveredShare` per zone, and lists sites, not names.
- **Dump**: `args.file`, `args.line`.
- **Windows**: the Performance window's zone details show the location and self time.
- **Tracy**: file, line and function on every zone; the GPU draws named by bucket.

### Built

Where round 2 differs from its steps, and why:

- **No per-item zones.** The first coverage pass put a zone on functions called per item (a placement, a draw, a mesh, material or texture
  entry, a LOD root): tens of millions of zones over a flight. With 120 seconds of history the dump of one flight was 11 GB, and reading it took
  the machine's memory (the terminal running the session was killed twice). Zones go to steps and loops; a loop over items is a leaf.
- **The placement tree is zoned by content**, not depth: `SyncStage` and `SyncLevel` (one per stage or LOD level placement synced), and a stage
  or level build in its stages (`PlacementWalks`, `PlaceSegments`, `FreeLeftovers`, `UseNode`, `SyncRoots`). Prototype instances and point
  instancers, tens of thousands in a full pass, count toward their level. `profile::ScopedZone` takes an `active` flag for it.
- **The world composer**: `ResolveContent` per reached content with LOD roots (its value: the roots), `ComposeLevel` for a level composed when
  first linked, `ComposePrototype`, `AddLayerPack`.
- **Coverage is read from the `zones` record**, polled every 2 seconds during the run, not from a dump: each record is the last 60 frames
  aggregated in the process (a few KB). The checks are scripts under `_out/profile` (not in the repository): `flight.sh` (the first ten flight
  cameras, `fly /cameras/flight_0 30`; the whole flight takes 15 minutes), `sponza.sh`, `assetrun.sh` (the release asset server in its own
  directory with an empty cache, packing Sponza through `ngen-pack`), `coverage_records.py`, and `packerrun.py` over a Tracy capture of one USD
  packer run. Every view in them runs under `systemd-run --user --scope -p MemoryMax=14G`.
- **The asset server**: `HashFile` (each file of a job hashed for the cache), `ReadPacked`, `Received` with `CacheFilesOf`, and `WatchLock` and
  `StreamLock` as Wait zones.
- **The render thread**: `BuildFrameGraph` in `AddCullPasses`, `AddGeometryPasses`, `AddLightingPasses`; `Record` in `BeginCommandBuffer`,
  `ExecuteFrameGraph`, `AfterExecute`, `EndCommandBuffer`; `GrowDrawLists` in `GrowInstanceLists`, `GrowMeshletWork`; the instance upload in
  `DirtySlots`, `InstanceStaging`, `PackInstanceRecords`. The main thread's `FitCascades` in `UpdateSceneBounds`, `FitShadowCascades`.

### Verification

Results (2026-11-09, release, this machine) in **bold**.

- Headless, release, the streaming test world flown with `fly /cameras/flight_ 30` and `dump-profile` of the flight, and Sponza with `record zones`
  at frame 3000: a check over the dump (self time per site from the zones' nesting) lists no zone above the thresholds with less than 80%
  covered, apart from the ones this plan names. **Run as described under Built (records polled, the first ten flight cameras, 2,700 frames).
  The zones still under 80% are leaves:**
  - **Loops over items:** `ResolveContent` (a content's LOD roots), `PlaceSegments` (a level's walk items), `EvaluateRoots`, `ResolveBindings`,
    `ReleaseUnreached`, `SlotChanges`, `DropStaleWalks`, `CollectChanges`, `ReleaseUnusedPacks`, `LibraryInUse`, `ForgetUnplacedRoots`,
    `MeasureStreaming`, `AssetIdList`, `ComposeLevel` (one level's prims), and `Resolve`'s own loop over the contents without LOD roots (66%
    covered); on the render thread `SceneBounds`, `PackInstanceRecords`, `CopyInstances`; on the main thread `UpdateSceneBounds`.
  - **Parsing and freeing:** `ReadLayerPack`, `ReadMeshPack`, `ReadTextureDesc`, `ParseFrame`, `AssetMessage`, `SceneRetire`,
    `FreeReplacedSnapshot`, `ShareDraws`.
  - **Driver and system calls:** `Submit`, `BeginCommandBuffer`, `RpcReceive` and `RpcSend` (the socket calls), a pass's recording
    (`ShadowPass`, `LightingPass`, `EditorUIPass`: one pass's commands).
  - **Asset server:** `HashFile`, `ReadPacked`, `Pump`, `CacheSave`. **USD packer** (Tracy capture): `PackMesh` 82% covered, `BuildMeshlets`
    one meshoptimizer call.
- `zones` on Sponza: every zone has a `file` that exists and a `line` inside it; the GeometryPass's children are `Opaque`, `Opaque two-sided`,
  `Cutout`, `Cutout two-sided`; the ShadowPass's are `Cascade 0`–`2`, each with the four draws. **Verified: all 106 sites of a debug three_cubes
  run point at an existing file and a line inside it; GeometryPass › `Opaque` (`geometrypass.cpp:229`), ShadowPass › `Cascade 2` › `Opaque`
  (`shadowpass.cpp:115`, `:140`).**
- Two sites with one name (`Idle` in `jobsystem.cpp` and `viewscene.cpp`) are two entries in `zones`, each with its own file. **Verified
  (`jobsystem.cpp:87`, `viewscene.cpp:232`).**
- Tracy (Henrik, visual): a zone's source view opens its line; the scene thread's `Relink` has its composer's steps underneath.
- Cost: the zone microbenchmark within 5 ns of round 1's 82 ns. Trace clean, debug runs without asserts, example sweep (`gpuzones` checks the
  tag comes back). **Pinned, best of 7: 44.5 ns without Tracy (round 1 43.6), 45 ns with Tracy compiled in and not started, 57 ns started with
  no server. Debug runs clean; all 17 examples exit 0, `gpuzones` reports `tags-returned: ok`.**

### Findings

What the zones showed, for whoever owns the code (none changed here):

- **The render thread, every frame on the streaming test world:** `SceneBounds` (a loop over every instance, 4–5.5 ms) and
  `PackInstanceRecords` (4–5.7 ms) — about 10 ms of each frame.
- **`CountEntities`** walks the whole World to count its entities for one trace message when a compose completes: up to 279 ms a call, 7–10 s
  over a flight.
- **The asset server's deliveries wait for `watchMutex`** (`WatchLock`): up to 29 ms per delivered asset.
- **`HashFile`**: hashing a job's files for the cache, up to 412 ms for one file.
- **ngen-view uses 6.6 GB on the streaming test world's flight** (resident, peak); the profiler's history 15 MB of it.

## Deferred / follow-ups

- **A short-lived program's outer zones in Tracy.** Tracy runs on demand: zones begun before a server connects are not sent, so a capture of a
  packer (`NGEN_TRACY=1`) holds its inner zones but not `PackerMain` or `PackUsdFile`. Left as it is (Henrik, 2026-11-09). Options when it
  matters: `startTracyIfAsked` waits a few seconds for a server, or the packer's history goes to the asset server (below). Trigger: a packer
  run whose start or end needs reading in Tracy.
- **Packer zones without Tracy.** A packer writing its history for the asset server to keep (a file next to the output, or sent on exit).
  Trigger: a packing question an agent has to answer without Tracy.
- **Per-draw GPU zones.** Trigger: a pass whose cost the region zones can't split.
