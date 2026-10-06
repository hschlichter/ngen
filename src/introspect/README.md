# Introspection

`ngen-introspect` is the one tool for seeing into the running processes — ngen-view, ngen-asset-server, later the editor — and for calling them: their
records, their traces merged by time, and their methods. It has a window for humans and a command line for agents and scripts; both are RPC clients
and nothing else. The processes don't know about the tool: it finds them through `.ngen-discovery/` in its working directory and connects to each.

ngen-view keeps its own windows. The tool shows what any process gives as records, and doesn't look into GPU buffers, textures or captures; those
stay in the view (`introspect.frame`, `introspect.gpuscene`, `capture.request`).

## Records

A record is one named piece of a process's data as JSON. Every endpoint serves its records through two methods:

- **`introspect.list`** → `{records: [{name, description}]}`
- **`introspect.get {name}`** → the record

The table behind them is `RpcRecords` in the RPC core (`src/rpc/core/rpcrecords.*`). A record's producer answers through an `RpcResponder`, at
once or frames later, so a record that needs GPU readback (`render`, `memory`, `counters`) completes when the data is in. ngen-view registers its
records with `registerRpcRecords` (engine layer); the asset server answers the two methods from its own dispatch with `serveRecordsCall`.

| Process | Record | What it holds |
|---|---|---|
| view | `status` | frame counter, scene, selection, camera, anti-aliasing, material sampler |
| view | `scene` | layers, prim count, up axis, mesh instances, lights |
| view | `assets` | what the view asked the asset server for and received |
| view | `culling` | GPU culling's latest readback: instances and triangles drawn and culled, camera and per cascade |
| view | `profile` | the profiler's frame history, CPU and GPU time per frame |
| view | `render` | the render debug snapshot (next frames) |
| view | `memory` | every allocation and heap (next frames) |
| view | `counters` | one frame's GPU zones and pipeline statistics per pass (next frames) |
| asset | `status` | pid, variant, directory, clients, pack tasks running and queued, packer runs |
| asset | `rules` | the pack rules from `pack.cpp` |
| asset | `cache` | every packed asset: version, packer, inputs, packed file |
| asset | `requests` | requests in flight and the 64 most recent finished ones |
| asset | `clients` | connected clients, bytes queued, assets waiting to stream |

Records are data and the trace is flow and messages (`src/trace/README.md`): a record never goes into the trace.

**A record at a frame:** the script verb `record <name> <file>` (method `introspect.record {name, path}`) appends the record to a file as one
JSON line: `{record, requested_frame, frame, ts_ns, value}`, `frame` being the frame it was ready (GPU readbacks take a few). The view takes the
sample itself, so the frame is exact and nothing has to be connected at that frame; a script run collects its values in one file, for example
`120 record render /tmp/records.jsonl` and `150 record memory /tmp/records.jsonl`.

**Adding a record:** `records.add(name, description, producer)` in the process — `registerViewRecords` (`src/view/viewcommands.cpp`) for
ngen-view, `AssetServer::addRecords` for the asset server. The tool shows it without changes. A record is for reading; anything that changes
state is a method.

## Command line

```sh
./ngen-cli introspect list                              # live processes and their records
./ngen-cli introspect get view culling                  # one record as JSON
./ngen-cli introspect get asset:12345 cache
./ngen-cli introspect describe view                     # methods with parameter schemas
./ngen-cli introspect call view view.camera.set '{"x": 5.3, "y": 11.3, "z": 1.2, "yaw": -169.8, "pitch": -0.2}'
./ngen-cli introspect trace --until-exit=view --output=/tmp/trace.jsonl   # every process's events until the view exits
```

**`trace`** subscribes to every process (`trace.subscribe`, `src/trace/README.md`), including ones that start later, and writes their events as
JSON lines: `ts_ns, process, thread, level, category, type, name, text, fields`. It takes events from its own start; `--history` takes
everything the processes' rings still hold. `--until-exit=<target>` stops once a matching process has connected and gone, its last events
included; without it the command runs until Ctrl-C. `--process=`, `--category=` and `--type=` filter (comma lists); `--level=warning` keeps
warnings and errors, `--level=error` errors only. With `--output` the file is written at the end,
sorted by time across processes; on stdout lines go out live, about a quarter of a second after they arrive. Dropped events (overwritten in a
process's ring before they were sent) are reported on stderr.

A target is a kind (`view`), `kind:pid`, or a pid; a kind with several live processes fails and lists them. Output is JSON on stdout. Exit codes:
0 success, 1 the call returned an error (printed as JSON), 2 no process or it can't be reached, 3 usage.

## Window

`./ngen-cli introspect` with no command opens the window with two tabs. Records has the processes and their records on the left and the selected
record on the right; Trace has every process's events, the full width, with any process the tool can't reach named in its toolbar. Objects show as a tree, arrays of objects as tables (click a header to sort, again to reverse, a third time for the record's own
order). Refresh fetches the record again; Auto refreshes it on an interval. The Trace tab holds the last 200,000 events of every process, merged by
time, with a level column (warnings yellow, errors red), filters by level, process, category, type and name, text or fields, Pause, Follow, and
the selected event below. It is drawn with SDL's
renderer and Dear ImGui — no Vulkan device, nothing of the renderer — and keeps no `imgui.ini`.

For a look without a display: `SDL_VIDEODRIVER=offscreen ./ngen-cli introspect --select=view/culling --frames=60 --screenshot=out.png`.
`--select=<target>/<record>` shows a record as soon as its process is there; `--trace` opens the Trace tab.

## Layout

| Part | Files |
|---|---|
| Targets | `introspecttarget.*` — process names (`kind:pid`) and resolving a target |
| Command line | `introspectcommands.*` — `list`, `get`, `describe`, `call` over a blocking `RpcClient`; `introspecttrace.*` — `trace` |
| Session | `introspectsession.*` — the connections: polls discovery, connects to new processes, fetches records and subscribes to traces on an `RpcServer` that only connects out |
| Window | `introspectwindow.*` — the ImGui layout and the generic JSON view |
| Program | `src/apps/introspect.cpp` — arguments, SDL and the frame loop |

## Known gaps

- Table cells that hold objects or arrays show compact JSON; they can't be expanded in place.
- The asset server has no `rpc.describe`, so `describe asset` fails; its methods are listed in `src/asset/README.md`.
- A process is connected to once; if the tool's connection to it drops, it isn't reconnected (a restarted process has a new pid and is).
- Profiler zones aren't on the trace stream; the `profile` record has frame times and `dump-profile` writes the full Chrome trace.
