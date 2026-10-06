# Introspection tool

**Status. Landed.**

Step 4 of [plan_tool_architecture.md](plan_tool_architecture.md), reshaped. `ngen-introspect` is the one tool for seeing what goes on in the engine
as a whole, for humans and for agents. It has two purposes:

1. **Query and view the data of every process**: ngen-view, the asset server, and later the editor.
2. **Gather and view every trace message** from those processes, in one timeline.

It replaces the observation bus as the way events are recorded and read, and it becomes how agents verify changes: a command-line face of the
same tool replaces `--obs-output` JSONL files. It also absorbs `ngen-rpc`. One tool instead of several that do similar jobs, built on the RPC system:
everything the tool shows or does is an RPC method on some process.

ngen-view keeps all the windows it has: data is best looked at where it is used. The umbrella's plan to move the examine-in-depth windows out of the
view is dropped. The tool doesn't look into GPU buffers, textures or captures, which are native to the view and stay there.

## Current state

- **ngen-view's windows** (Memory, Capture, Frame Debugger, GPU Scene, Counters, Frame Graph, Performance, Render Debug, Culling, …) read the
  renderer's C++ structures on the main thread.
- **ngen-view's RPC methods** already return most of its data as JSON: `introspect.render`, `.memory`, `.counters`, `.profile`, and
  `view.status`. Three are GPU-native: `introspect.gpuscene` (GPU tables), `introspect.frame` (a frame capture) and `capture.request`. The
  `--dump-render-debug`, `--dump-memory`, `--dump-profile` flags and `dump-*` script verbs write the same data to files.
- **The observation bus** (`src/obs/`, usage in `obs.md`): 41 `OBS_EVENT` call sites in 9 files, plus direct uses of its event builder. One
  sink, installed at start-up only with `--obs-output` (JSONL); otherwise events are skipped. Categories are fixed at start-up. Timestamps count from
  each process's own start (`monotonicOriginNs`), so two processes' events can't be merged. Agents verify changes by reading that JSONL
  (`AGENTS.md`, the run-headless skill).
- **The asset server** prints its trace as text lines on stdout (`src/asset/server/assettrace.h`) and answers `server.status`.
- **`ngen-rpc`** (`list`, `describe`, `call`) is the command-line way into every endpoint.
- **RPC** carries requests and notifications both ways, with binary attachments; endpoints announce themselves in `.ngen-discovery/` in their
  working directory.

## Scope

**In**

- **A trace library, `src/trace/`, replacing the observation bus.**
  - Structured events: system-wide monotonic time, process, thread, category, type, name, fields.
  - An always-on in-memory ring per process, streamed to subscribers over RPC (`trace.subscribe`, then `trace.events` batches).
  - Every process uses it: ngen-view, the asset server (its trace lines become events, still printed), later the editor.
  - The 41 `OBS_EVENT` call sites move to it; `src/obs/`, `--obs-output`, `--obs-only` and `--obs-exclude` go.
- **Records on every endpoint**: `introspect.list` (the names and descriptions of the data a process can give) and `introspect.get` (one record as
  JSON).
  - ngen-view: the existing CPU-side data (`render`, `memory`, `counters`, `profile`, `status`), and new records for the scene, the asset client
    and culling.
  - The asset server: status, rules, cache records, requests in flight and recent, and clients.
- **`ngen-introspect`, windowed**: an SDL3 window with Dear ImGui, drawn with SDL's renderer. It connects to every endpoint in its working
  directory's `.ngen-discovery/` and follows processes as they start and stop.
  - A Processes window; a Records window that shows any record (JSON tree, arrays of objects as sortable tables, refresh on request or every N
    ms); a Trace window that merges every process's events in time order, with filters by process, category, type and text, pause, follow, and
    an event's fields.
- **`ngen-introspect`, command line**, the same library without a window, for agents and scripts:
  - `ngen-introspect list` — processes, and each one's records
  - `ngen-introspect get <process> <record>` — a record as JSON on stdout
  - `ngen-introspect trace [filters] [--until-exit <process>] [--output FILE]` — events as JSONL, from the processes' history on, until the
    named process exits or the command is stopped
  - `ngen-introspect describe <process> [method]` and `ngen-introspect call <process> <method> [params]` — `ngen-rpc`'s commands, moved
- **CPU-side dumps become records in the trace.** A script verb `record <name>` puts a record into the trace as one event at that frame; the
  `--dump-render-debug`, `--dump-memory`, `--dump-profile` flags and the `dump-render-debug`, `dump-memory`, `dump-profile`, `dump-counters` verbs
  go. GPU-native dumps (`dump-frame`, `dump-gpuscene`, `dump-texture`, captures) stay script verbs in the view.
- **`ngen-rpc` folds into `ngen-introspect`**: `src/apps/rpc.cpp` goes; `ngen-cli rpc` becomes `ngen-cli introspect`.
- **Agents verify through it**: the run-headless skill and `AGENTS.md` change from `--obs-output` to `ngen-introspect trace` and
  `ngen-introspect get`.

**Out**

- **Moving view windows into the tool.** They stay in the view.
- **GPU-native data in the tool**: GPU tables, buffers, textures, captures, the frame debugger. `introspect.gpuscene`, `introspect.frame` and
  `capture.request` stay RPC methods, but aren't records.
- **The profiler's zones on the trace stream.** They are high-frequency timing data with their own windows; see Deferred.
- **Record schemas**: the records reuse today's JSON.
- **Session recording** in the windowed tool (save a trace, load it later). See Deferred.
- **Remote processes and authentication**: loopback only, as every endpoint.

## Decisions

Locked with Henrik:

1. **The view keeps its windows; the tool is for data across processes and for traces.**
2. **No GPU-native data in the tool.**
3. **One trace system replaces the observation bus** (option A of the discussion): one tool for events instead of two that overlap.
4. **Agents verify with the introspection tool**, not with JSONL files the view writes.
5. **`ngen-rpc` folds into the tool, and everything stays on RPC.** The tool is an RPC client like any other; records, traces and commands are
   methods on the processes. No side channel.
6. **The tool draws with SDL's renderer** (`imgui_impl_sdlrenderer3`) and Dear ImGui, nothing else: simple and minimal. No Vulkan device, no
   dependency on the renderer.
7. **The trace ring is always on** (Decision 12 has the details).

8. **The JSONL line stays the same shape** as the observation bus's (`ts_ns`, `thread`, `category`, `type`, `name`, `fields`), with `process`
    added, so the `jq` habits for today's files carry over.
9. **Frame-exact data goes through the trace.** `get` from outside the view can't pick a frame; a script's `record <name>` can, and its event
   lands in the same `ngen-introspect trace` output as everything else. One path for CPU-side data instead of `get` plus dump files. GPU-native
   dumps stay files written by the view, as Decision 2 keeps them out of the tool.
10. **Generic records instead of a window per data type.** One `introspect.list` / `introspect.get` pair on every endpoint, and a tool that draws
   any JSON. New data and new processes appear in the tool without tool changes.
11. **The tool connects to the processes**, through discovery in its working directory, as `AssetClient` finds the asset server. Nothing in the
   processes depends on the tool. The umbrella had views connect to an open tool; that is reversed.
12. **The ring holds 65,536 events per process**, so the tool can attach at any time and see what happened before.
   - This replaces the observation bus's "off unless asked" with "on, in memory". The cost, building and queueing events every frame, is measured
     (Verification).
   - A command-line `trace` started before the view gets everything from the view's first event: it connects when the view's discovery file
     appears, and the history covers the gap. A run longer than the ring before the tool connects loses its oldest events, reported as a count.
13. **A process flushes its trace before it exits.** On shutdown it sends the last batch to its subscribers and waits up to a second for them to
   take it, so `trace --until-exit` sees the final events (`quit`, the screenshot written).
14. **Streaming never stalls a frame.** Events go out in batches (every 50 ms, or every 1,024 events). Each subscriber has a capped queue; when
    it falls behind, the oldest events are dropped and the next batch says how many.
15. **One clock.** Events carry `CLOCK_MONOTONIC` nanoseconds, which every process on a machine shares, so the tool merges by timestamp. (The
    observation bus counted from each process's start.)

## Steps

### Phase A: records and the tool

1. **Records on the endpoints**: a record table in each process (name, description, producer); `introspect.list`, `introspect.get`.
   - ngen-view: wrap the existing writers (`render`, `memory`, `profile`, `counters`, `status`), and add `scene`, `assets`, `culling` first,
     since the status bar and the windows already gather that data.
   - Asset server: `status`, `rules`, `cache`, `requests`, `clients`.
2. **`src/introspect/`** (library: endpoint manager polling discovery, one `RpcServer` client connection per process, reconnect, record
   fetching) and **`src/apps/introspect.cpp`** (main: windowed by default; command-line subcommands `list`, `get`, `describe`, `call`).
   `ngen-rpc`'s code moves here and `src/apps/rpc.cpp` is removed.
3. **The windowed tool**: SDL window, imgui with the SDL renderer backend, Processes and Records windows.
4. **`build.cpp`**: an `ngen-introspect` program replacing `ngen-rpc`; `ngen-cli introspect` replacing `ngen-cli rpc`; the CI build line.

**Phase A as built.** Beyond the steps above:
- `RpcResponder` moved into the RPC core, next to the new `RpcRecords` (`src/rpc/core/rpcrecords.*`), so the asset server serves records
  from its own dispatch (`serveRecordsCall`) and the view through its registry (`registerRpcRecords`).
- The `status` records replace the `view.status` and `server.status` methods, so the same data has one way in.
- The CPU-side `introspect.render`, `.memory`, `.counters` and `.profile` methods stay until step 7 removes the verbs that use them.
- The view's `profile` record is the frame history (CPU and GPU ms per frame); the Chrome trace stays a file (`introspect.profile`).
- The window takes `--select=<target>/<record>`, `--frames` and `--screenshot`, so it can be looked at headless.
- Gap: the asset server has no `rpc.describe`.

### Phase B: traces

5. **`src/trace/`**: the event type, `TRACE_EVENT` (the builder `OBS_EVENT` has today), the ring with a cursor per subscriber, batching,
   `trace.subscribe` on the RPC engine layer and the asset server.
6. **Migrate**: the 41 `OBS_EVENT` sites and the direct builder uses to `TRACE_EVENT`; the asset server's trace lines to events; remove
   `src/obs/` and the `--obs-*` flags; flush on exit.
7. **`record <name>` script verb** in `src/view/viewcommands.cpp`: emits a `Record` event (`name`, `fields` = the record's JSON, `frame`). Remove
   the CPU-side `--dump-*` flags from `src/apps/view.cpp` and their verbs and writers from `src/view/viewdumps.*`, keeping the GPU-native ones.
8. **The tool**: the Trace window; the `trace` subcommand with filters, `--until-exit` and `--output`.
9. **Docs**: `src/trace/README.md` (replaces `obs.md`) and `src/introspect/README.md`; `src/rpc/README.md` (records, `trace.subscribe`),
   `src/asset/README.md`, the root README; `AGENTS.md` and the run-headless skill move verification to `ngen-introspect`.

**Phase B as built.** Beyond the steps above:
- `BusStarted` became `ProcessStarted`; `ProcessExiting` is the view's last event, sent before its endpoint stops.
- The asset server's log lines are `Asset`/`Message` events with the line in `text`; nothing more structured yet.
- **The trace became flow and messages, not data** (decided after the first runs, which showed a constant stream of `PassExecuted`,
  `FrameEnd`, `RenderStats` and the like: the migration had carried the observation bus's per-frame narration into an always-on ring).
  Events gained a level (info, warning, error) and a `text` message; `TRACE_WARNING` and `TRACE_ERROR` sit next to `TRACE_EVENT`, and an error
  is also printed on stderr. Per-frame and data events were removed where a record holds the data (`render`, `counters`, `culling`,
  `memory`, `profile`; `status` gained anti-aliasing and the sampler). Flow events were added (`SceneOpenStarted`, `SceneOpened`,
  `SceneUploaded`, `EditApplied` for committed edits). Engine diagnostics on stderr became warnings and errors, the RHI's through a new
  `RhiDeviceOptions::onMessage` handler; the asset server's failed requests are warnings. `RpcCall` became `RpcCallFailed`.
- Records don't go into the trace: the `record` verb is `record <name> <file>` (method `introspect.record`), appending
  `{record, requested_frame, frame, ts_ns, value}` to a file. `introspect.trace` and the `Record` category are gone. `trace` takes
  `--level=`, and the Trace tab shows level and text.
- **The console is the trace.** Every event is printed with the local time, info on stdout and warnings and errors on stderr, and engine code
  prints nothing else, so a process's console and its trace hold the same lines. The asset server's own timestamped printing moved into the
  trace; the RHI's handler gained `Info` for what it used to print on stdout; ResourcePool's per-allocation line went (the `memory` record
  has every texture).
- **Start-up reads as a timeline.** Events for each step up to the first frame: asset server connected, shaders requested and ready (with the
  time waited), resolver registered, scene open requested (command line or editor), meshes and materials built and the render world
  extracted (counts, ms), window created, job system, GPU and device features (RHI info), renderer initialised, main loop started and first
  frame presented (ms since the process started), the session script, real window resizes. OpenUSD's diagnostics go into the trace through a
  `TfDiagnosticMgr` delegate. Sponza: 5.4 s to the first frame, 2.6 s of it opening the stage and 1.3 s building meshes and materials.
- `--dump-profile` went, but the `dump-profile` verb and `introspect.profile` stay: the Chrome trace has the profiler zones, which neither a
  record nor the trace carries.
- `trace --output` sorts the whole file at the end: a process the tool connects to late brings history older than lines already merged. On
  stdout lines go out live, a quarter of a second after arrival.
- Without `--history`, `trace` takes events from its own start, so a long-running asset server's earlier events stay out.

**Results** (the first paragraph is the migration as planned, before the trace became flow and messages). three_cubes, 200 frames: the
view's 3,636 events match the pre-migration `--obs-output` run in type, name and field keys (three neighbouring pairs swapped by cross-thread
timing), plus `ProcessStarted`, `ProcessExiting`, the tool's own `RpcConnected`/`RpcCall` and the
asset server's messages; the file is sorted by time. `record` at frames 120 and 150 put `culling`, `render` and `memory` into the trace. A
tool stopped for 8 s and 40 s left the view's frame time at 1.14 ms; after the 40 s stop it reported 447,377 dropped events. Sponza, GPU-bound
in debug: 8.24–8.27 ms before, 8.24–8.39 ms after. A CPU-bound before/after comparison wasn't run. The Trace tab shows the view's and the
asset server's events live (headless screenshot).

After the change: three_cubes, 200 frames, gives the view 11 events (start-up, `SceneOpenStarted`, `SceneOpened`, `SceneUploaded`, render
thread start and stop, RPC, exit), plus two `ScriptCommandFailed` errors for a deliberately bad script line and an unknown record, which also
print on stderr. `record render` at frame 100 and `record status` at 120 wrote two lines (render ready at frame 111). Sponza: 9 view events;
the `render` record holds 25 textures at 4096×4096 and 3 at 1×1. A missing scene gives the asset server's `failed: no such file` as a warning
(`trace --level=warning` shows only it) and the view's `AssetOpenFailed` and `SceneOpenFailed` on stderr. The debug view with `--validation`
on three_cubes reports no validation messages; the triangle, compute and bindless examples pass `--check --validation` with the default
stderr handler.

## Verification

**Phase A**
- With ngen-view and the asset server running in a folder, the windowed tool started there lists both within a second, and drops a process
  within a second of it stopping.
- `ngen-introspect list` prints the same processes and records as each process's `introspect.list`.
- Every `ngen-rpc` command in the run-headless skill and `src/rpc/README.md` works as the same `ngen-introspect` command with the same output.
- `ngen-introspect get view render` has the same values as `dump-render-debug` for the same frame.
- The asset server's `cache` record has one entry per asset in `.ngen-assets/<platform>/<config>/`, with the same versions.
- `ngen-introspect` links no GPU table, capture or frame-debug code (link graph).

**Phase B**
- **Same events as before:** a headless three_cubes run recorded with `ngen-introspect trace --until-exit view --output t.jsonl` has the same
  events (type, name, fields, order within a process) as `--obs-output` gave before the migration, plus the asset server's.
- **Records at a frame:** a script with `record render` and `record memory` at frame 60, traced through the tool, gives the same values as
  `--dump-render-debug` and `--dump-memory` gave for frame 60 before the migration.
- **Nothing lost at the edges:** the view's first event (start-up) and last (`quit`) are in the file.
- **Merged:** during a Sponza load the Trace window shows the asset server's packing events and the view's events interleaved in time order.
- **No stalls:** a stopped tool (`SIGSTOP`) doesn't change the view's frame time; after `SIGCONT` the next batch reports the dropped count.
- **Cost:** with the ring always on and no tool connected, Sponza's mean frame time over 300 frames stays within noise of today's without
  `--obs-output`.
- **The skill works:** the run-headless skill's verification steps, rewritten for `ngen-introspect`, run end to end on three_cubes and Sponza.

## Deferred / follow-ups

- **Profiler zones on the trace stream**, so the tool shows CPU and GPU timing next to events. Trigger: a problem that needs both on one timeline.
- **Session recording in the windowed tool** (`.ngentrace`). Trigger: a problem that has to be looked at after the run ended.
- **Specialised record views** (plots over time, the frame graph drawn as a graph). Trigger: a record a table can't make readable.
- **Remote processes.** Trigger: a view on another machine.
