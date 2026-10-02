# RPC core, ngen-rpc, and ngen-view as an endpoint

**Status. Landed.**

Step 1 of [plan_tool_architecture.md](plan_tool_architecture.md). Everything after it talks through what this plan builds:
- the build server ([plan_build_server.md](plan_build_server.md))
- pack requests and deltas
- the editor
- the introspection tool

## Current state

- **No networking anywhere.** A running ngen-view can't be reached from outside.
- **Agents drive ngen-view offline.**
  - Flags, and `--script` files of `<frame> <verb> [args]` lines (`src/session/sessionscript.*`).
  - The verbs are one `applyCommand` lambda in `src/main.cpp`: `camera`, `camera-frame`, `select`, `translate`, `view`, `window`, `debugview`,
    `overlay`, `cull`, `sampler`, `inspect`, `dump-texture`, `shadow`, `prepass`, `screenshot`, `dump-render-debug`, `renderdoc-capture`, `capture`,
    `dump-frame`, `dump-gpuscene`, `dump-counters`, `dump-memory`, `dump-profile`, `quit`.
  - Verbs take a string, report problems with `std::println(stderr, …)`, and return nothing.
  - Some finish frames later: a screenshot after its fence, captures and dumps when their results arrive.
- **Dump writers only write files.** `writeRenderDebugJson`, `writeMemoryJson`, `writeFrameDebugJson`, `writeGpuCountersJson`,
  `writeGpuSceneJoinedJson` and `writeCaptureFiles` all take a path and use `fprintf`.
- **There's no JSON reader in the tree.** The only JSON code is writers; `src/build/ir/json.hpp` (after the move) is explicitly not a parse target.
- **Discovery, ports and endpoints don't exist yet.** `ngen-cli` has a tool table ready for an `rpc` row.

## Scope

**In**

- **`src/rpc/core/`**, standard library plus the header-only nlohmann/json, shared with the build server:
  - JSON through nlohmann/json, vendored as a submodule in `external/json`
  - frames: a length-prefixed header plus an attachment slot; the core reads and writes attachments, and step 1's view methods don't use them yet
  - JSON-RPC 2.0 messages
  - a peer that sends and receives calls in both directions on one connection
  - a TCP transport on loopback, and discovery files
- **`src/rpc/`, the engine layer:**
  - `RpcMethodRegistry`: a method has a name, a summary, a parameter schema, a result schema, and the thread it runs on
  - `RpcResponder`, which can be completed later
  - `RpcEndpoint`: listen, the I/O thread, dispatch to the main thread
  - `rpc.describe`, `rpc.ping`, `rpc.version`
- **ngen-view registers methods:**
  - every session verb as a method
  - the dumps as `introspect.*` methods, returning JSON inline
  - `capture.request`, returning the capture inline
  - `view.screenshot`, returning a PNG path, or the bytes as base64 on request
- **`--script` runs through the registry.** Each script verb maps to its method, with a small parser from the script's string to the method's
  parameters. There's one implementation per command, used both live and offline.
- **`ngen-rpc`:** `list`, `describe <target>`, `call <target> <method> [params-json]`, printing JSON.
- **`ngen-cli`:** the tool table gains `rpc`, running `ngen-rpc`.
- **Observations:** `RpcListening`, `RpcConnected`, `RpcDisconnected`, and `RpcCall` (method, ms, ok or error code) in the Engine category.

**Out**

- Subscriptions and streams (step 4). A method returns once; nothing pushes events to a client yet.
- Binary attachments in view methods. The core supports them, but step 1's view methods send bulk data as base64 (a gap).
- Methods for scene editing beyond today's verbs (`select`, `translate`). Scene methods belong to the editor (step 3).
- Authentication, and anything but loopback.
- An RPC endpoint in `gamerelease`: the endpoint is compiled in with `NGEN_INTROSPECTION`, like the other tooling.

## Decisions

Proposed; pushback welcome.

1. **JSON through nlohmann/json, vendored as a submodule in `external/json`.** Locked with Henrik.
   - It is header-only and needs nothing beyond the standard library, so the build server can use the RPC core with no other dependencies.
   - Only `src/rpc/core/*.cpp` includes `nlohmann/json.hpp`. Headers use `nlohmann/json_fwd.hpp`, so the compile-time cost stays in a few files.
   - Writing our own JSON (about 500 lines) was the alternative; picojson, RapidJSON, glaze and jsmn were considered.
   - The repository is large because of its test data; a shallow submodule keeps the checkout reasonable. Only `single_include/` is used.
2. **The frame format is fixed now, attachments included.** A frame is `u32 json_length`, `u32 attachment_length`, the JSON, then the attachment
   bytes, all little-endian.
   - The core reads and writes attachments from the start; a message refers to its attachment by offset and length.
   - Step 1's view methods don't use attachments yet. Deltas (step 3) and the streams (step 4) do.
3. **Methods run on the thread that owns their data, one frame at a time.**
   - The I/O thread reads requests and queues those for the main thread.
   - The main thread drains the queue at the point where session commands run today (after `session.takeDue`).
   - A handler either responds at once, or keeps its `RpcResponder` and completes it later, for example a screenshot after its fence, or a capture
     when the result arrives.
   - Responses go back through the I/O thread, woken with an `eventfd`.
   - Latency is at most one frame for immediate methods.
4. **Parameters are typed and checked before the handler runs.**
   - A schema field has a name, a type (`bool`, `int`, `float`, `string`, `vec3`, `enum` with values, `array`, `object`), required or optional, and
     a one-line description.
   - A mismatch answers JSON-RPC `-32602` with the field named.
   - Unknown methods answer `-32601`.
   - Engine failures ("prim not found", "not built") use application codes from `-32000`, with a message.
   - `rpc.describe` returns every schema, so agents discover the surface without reading source.
5. **Method names: `area.verb`, with areas for today's surface.**
   - `view.*`: camera, frame, select, translate, view mode, debug view, overlay, windows, sampler, shadow, prepass, screenshot, quit
   - `introspect.*`: `render`, `memory`, `gpuscene`, `counters`, `frame`, `profile`
   - `capture.*`: request
   - `renderdoc.*`: capture

   Script verbs keep their names, and the table maps each verb to its method.
6. **Dumps return JSON inline by writing into memory.** The writers already take a `FILE*` internally. They gain an overload on a `FILE*`, and the RPC
   path passes `open_memstream`. The file-path versions stay for the verbs. The dump code isn't rewritten now; step 4 replaces the writers with the
   one records model.
7. **Discovery.**
   - Each endpoint writes `_out/run/<kind>-<pid>.json` (kind, pid, port, project root, protocol version, start time, and a label such as the scene
     path) once it is listening, and removes it on exit.
   - Readers drop files whose pid is gone.
   - The port is chosen by the OS (bind to `127.0.0.1:0`).
8. **Targets in `ngen-rpc`.** A target is a kind (`view`), a kind and pid (`view:12345`) or a pid. A kind with several live endpoints fails and lists
   them. `ngen-rpc list` prints every live endpoint.
9. **The endpoint is on by default in debug and release**, and `--no-rpc` turns it off. Headless runs in parallel each get their own port and
   discovery file.

## Steps

1. **nlohmann/json:**
   - `external/json` as a shallow submodule (Henrik adds it; it's a git operation)
   - `external/json/single_include` on the include path of the RPC core, the build server helper (`self_build_ir()`) and `ngen-rpc`
2. **`src/rpc/core/`:**
   - `frame.h/.cpp`: frame read and write
   - `peer.h/.cpp`: JSON-RPC ids per direction, pending calls, dispatch of incoming calls to a handler
   - `transport.h/.cpp`: TCP listen, accept, connect, and a `poll` loop
   - `discovery.h/.cpp`: write, remove, list, prune
   - no engine includes; nlohmann/json only in the `.cpp` files
3. **`src/rpc/`:**
   - `rpcregistry.h/.cpp`: methods, schemas, parameter checking, `rpc.describe`
   - `rpcresponder.h`
   - `rpcendpoint.h/.cpp`: the I/O thread, the main-thread queue, and `drain(frame)`
4. **Commands out of `main.cpp`:**
   - `applyCommand`'s branches move into `src/session/sessioncommands.h/.cpp`, one function per command with typed parameters, registered as methods
     with their script verbs and parsers
   - `main.cpp` calls `endpoint.drain(frameCounter)` and runs due script commands through the same registry
   - errors that went to stderr become responder failures; the script runner prints them as before
5. **Deferred completions:** screenshot, `capture.request`, `introspect.frame` (the frame debug capture), `introspect.gpuscene` and
   `introspect.counters` complete their responders where today's code writes their files.
6. **Writers:** `FILE*` overloads for the six dump writers, and `open_memstream` on the RPC path.
7. **`ngen-rpc`** (main in `src/apps/rpc.cpp`, [plan_apps_folder.md](plan_apps_folder.md); a program target in `build.cpp`): `list`, `describe`, `call`. Exit code 0 on success, 1 on an RPC
   error, 2 when there's no endpoint or it can't connect.
8. **`ngen-cli`:** an `{ "rpc", "ngen-rpc" }` row in the tool table.
9. **Docs:**
   - `src/rpc/README.md`: the protocol (frames, JSON-RPC, codes), discovery, threading and responders, and how to add a method
   - `AGENTS.md` and the run-headless skill: `./ngen-cli rpc` for live investigation next to the offline script loop
   - the root README's ngen-cli section gains `rpc`

## Verification

- **Script parity.** The existing headless runs through the registry match today's:
  - the six screenshots are byte-identical
  - the dump files of `dump-gpuscene`, `dump-counters` and `dump-memory` are byte-identical for the same frame
  - a script with a bad verb argument prints the same message
- **Live queries on a running Sponza:**
  - `ngen-rpc call view introspect.gpuscene` returns JSON equal to `dump-gpuscene`'s `instances_joined.json`
  - `ngen-rpc call view view.camera.set '{…}'` then `view.screenshot` gives a PNG byte-identical to the `--camera` and `--screenshot` flags for
    the same pose and frame count
  - `capture.request` for `gbuffer.normal` returns the same channel ranges as `capture GeometryPass gbuffer.normal`
- **Discovery:**
  - `ngen-rpc list` shows the running view with its scene label
  - after a normal quit its file is gone
  - after `kill -9`, the next `ngen-rpc list` prunes it
  - two views at once get two entries; `call view …` fails and lists both, and `call view:<pid> …` reaches the chosen one
- **Errors:** an unknown method gives `-32601`; a wrong parameter type gives `-32602` naming the field; `view.select` on a missing prim gives an
  application error with the path.
- **Frame safety:** 1,000 `rpc.ping` calls in a loop while Sponza renders don't move the frame time (Performance window, compared with no client).
  A client that stops reading doesn't block the main thread.
- **`rpc.describe`** lists every registered method with its schema, one per script verb plus the `introspect.*`, `capture.*` and `rpc.*` methods.
- **`--no-rpc`:** no port opened, and no discovery file.

## Gaps

- Request/response only: no subscriptions, so an agent polls for anything that changes over time.
- Bulk data (screenshots, captures) is base64 in JSON, about a third larger than the binary.
- Loopback only, with no authentication; any local process can drive a running view.
- Methods run one frame at a time on the main thread, so a slow handler delays the frame it runs in. Handlers must hand heavy work to the existing
  asynchronous paths, as captures already do.
- The dumps are still the six writers. Their shapes change in step 4.

## Deferred / follow-ups

- **Screenshots and captures as attachments** instead of base64. Trigger: step 4, or base64 cost showing up in a profile.
- **Subscriptions and streams.** Trigger: step 4.
- **A Python or other-language client**, for agents that prefer it over `ngen-rpc`. The protocol is plain JSON-RPC over TCP, so any client works;
  trigger: an agent workflow wanting one.

## Results

Built in all three configs; `ngen-rpc` and the `rpc` row in `ngen-cli` exist. What was checked:

- **Script parity.**
  - The six headless screenshots are byte-identical to the baseline.
  - `dump-gpuscene` on Sponza (courtyard camera, frame 150) writes ten files byte-identical to the stage 4 dump from the introspection work.
  - Every other dump verb writes its files with the old console messages: render debug, memory, counters, frame with per-pass captures, capture,
    profile.
  - Bad arguments print the old messages (`camera: expected x,y,z,yaw,pitch, got '1,2'`, `overlay: expected name=on|off, got 'grid'`,
    `select: prim '/nope' not found`, `unknown session command 'bogus-verb'`).
- **Live, on a running Sponza:**
  - `ngen-rpc list` shows the view with its scene label, and `describe` lists 29 methods.
  - `introspect.gpuscene` returns 407 rows equal to the stage 4 dump.
  - `capture.request` for `gbuffer.normal` returns `R16G16B16A16_SFLOAT` 2560×1440 with channel ranges.
  - `view.camera.set` plus `view.screenshot` gives a PNG byte-identical to the flag-driven screenshot of the same pose. With `inline`, it also
    returns the base64 PNG.
- **Errors:** an unknown method gives `-32601`; `x: "one"` gives `-32602` "parameter 'x' must be float"; a missing prim gives `-32000` with the path.
- **Discovery:**
  - two views are listed, and `call view …` fails listing both, while `view:<pid>` and a bare pid reach the chosen one
  - the file is removed on quit
  - after `kill -9` the file is left, and the next `list` prunes it
  - `--no-rpc` writes none, and a `gamerelease` view writes none
- **Frame safety** (three_cubes, uncapped):
  - baseline 873 fps
  - a client that sends 500 `rpc.describe` calls and never reads: 871 fps
  - a client flooding `rpc.ping` as fast as it can: 826 fps
  - after both closed: 880 fps
  - A client whose unread replies pass 256 MB is dropped.

Deviations from the plan:

- **The view's commands and dumps live in `src/view/`** (`viewcommands.*`, `viewdumps.*`, compiled into `ngen-view`), not
  `src/session/sessioncommands.*`. They need the renderer, the editor UI and the USD scene, which the `session` library doesn't link. `sessionscript`
  stays where it is.
- **`drain()` has a time budget** (2 ms per frame, at least one call), and the queue is a deque. The first flood test, with everything drained at
  once, dropped the view to four frames in eight seconds.
- **`rpcclient.h` includes the full JSON header.** Its response carries a JSON value, and its users are command-line tools. Every other header uses
  `json_fwd.hpp`.
- **`overlay`, `sampler` and `shadow` reject the whole line on a bad item.** They used to apply the good items and warn about the bad one. The message
  is unchanged.
- **Methods beyond the verbs:** `view.camera.get` and `view.status`. `view.screenshot` without a path writes
  `_out/run/screenshot-<pid>-<frame>.png`.
- **Screenshots now report completion.** The renderer queues a `ScreenshotResult` per written file, delivered through `RenderThread`, so a
  screenshot call answers when its file exists.
- **The dump writers are split** into a `FILE*` body and a path wrapper (`writeRenderDebugJson`, `writeMemoryJson`, `writeFrameDebugJson`,
  `writeGpuCountersJson`, `writeGpuSceneJoinedJson`). `writeCaptureJson` is the JSON part of `writeCaptureFiles`.

Changed after landing (2026-10-02): discovery files moved from `<project root>/_out/run/` to `.ngen-discovery/` in each tool's working
directory, and the project root (`rpcProjectRoot`) is gone; tools find each other when they run in the same directory
([plan_asset_server.md](plan_asset_server.md)).
