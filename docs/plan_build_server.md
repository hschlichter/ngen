# Build server

**Status. Superseded by [plan_asset_server.md](plan_asset_server.md).**

Part of step 2 of [plan_tool_architecture.md](plan_tool_architecture.md). It needs:
- the build system under `src/build/` ([plan_build_into_src.md](plan_build_into_src.md))
- the RPC core from step 1 ([plan_rpc.md](plan_rpc.md), landed)
- pack rules in the IR, and pack jobs as edges ([plan_pack_rules.md](plan_pack_rules.md))

It provides what [plan_async_shaders.md](plan_async_shaders.md) and the scene packing need: `pack.request`, and packed data streamed back over the
connection.

**Packing is request-driven.** `build.cpp` declares which file extensions can be packed, and the packer binary that packs each. Which assets get
packed is decided at runtime, by requests: a user drops an asset of a supported type into the project, something requests it (in the end, mostly a
USD scene referencing it), and it is packed, with no change to `build.cpp`. Nothing is ever packed without a request. The engine's own start-up
shaders go the same way: ngen-view requests them and waits for the results before it goes on.

**All packed data is streamed.** The server sends a packed asset's bytes over the RPC connection. A client never reads the server's files, so a view
could in principle run on another machine and load everything from `ngen-build`.

## Current state

- **`ngen-build` is one-shot, in three stages** (`src/build/bootstrap.cpp`):
  - it self-builds `_out/ngen-build-graph` and `_out/ngen-build-run`
  - it runs the graph stage as a subprocess, which writes `_out/<platform>/<config>/build.ngenir`
  - it runs the runner as a subprocess against that IR
  - every invocation re-reads the IR and the build log from disk, and exits
- **The runner is a library.** `ngen::run::execute(IR&, RunOptions&)` in `src/build/run/execute.hpp` covers dirty detection with content hashing and
  a stat fast path, depfiles, the parallel scheduler and the build log. `ngen-build-run` is a thin CLI over it.
- **Nothing guards the build log.** Two concurrent `ngen-build` calls for the same variant would both write `.ngen-buildlog`.
- **The framework is std-only and self-contained** ([plan_build_into_src.md](plan_build_into_src.md), Decision 1).
- **The RPC core exists** (`src/rpc/core/`), standard library plus nlohmann/json:
  - frames carry a JSON text and a binary attachment, up to 256 MiB per frame
  - `RpcServer` has an I/O thread, and drops a client whose outgoing queue passes 256 MiB
  - `RpcClient` is blocking, for command-line tools: it answers anything the peer sends it with `methodNotFound`
  - the engine layer on top (`src/rpc/`: registry, endpoint) is ngen-view's
- **`ngen-cli build` forwards to `ngen-build`** with `execv`, and fills in `-p`/`-c` from `_out/set` (`src/apps/cli.cpp`).
- **After [plan_pack_rules.md](plan_pack_rules.md)**, the IR carries pack rules and the packs root. Each asset a static pack target lists is a pack
  job edge in the build log, writing `<out_dir>/packs/<asset id>`, and the runner rewrites the reverse index `packs/.ngen-packdeps` after every build.
  Pack jobs exist only for assets a pack target in `build.cpp` lists (`pack("core")`, every shader). ngen-view reads the packed files from disk.

## Scope

**In**

- **Server mode is the default for builds.** Every build goes through a build server, so the request flow is always exercised.
  - `ngen-build -p … -c … [targets]` makes sure a server is running for the project, starting one if needed. It sends the build as a request, prints
    the server's progress, and exits with the build's exit code. The server keeps running.
  - `ngen-build --no-server -p … -c … [targets]` is today's in-process one-shot, with no server. It's for bootstrapping problems, CI that wants no
    background process, and debugging the build system itself.
  - `ngen-build --serve` runs the server in the foreground. Clients start it this way, detached; it's also how you debug the server.
  - `ngen-build --shutdown` stops the running server for this project. `ngen-cli build --shutdown` is the same flag passed through, since `build`
    passes every argument on.
- **Only the build path starts a server.** `ngen-build` and `ngen-cli build` do (through `src/build/client/`); no other program does.
  `ngen-cli build` sends `build.run` itself, instead of exec'ing `ngen-build`.
- **The server holds, per variant in use:** the loaded IR, the build log in memory, pack jobs included, and the pack reverse index. It registers in
  discovery (`_out/run/ngen-build-<pid>.json`: project root, port, pid, helper version).
- **Builds over RPC.**
  - `build.variants` and `build.targets`.
  - `build.run` (variant, targets, flags) runs the in-process runner. The server calls back on the same connection: `build.output` for each output
    line, and `build.done` with the counts and exit status.
  - `build.clean` does the same as `--clean`.
- **Pack rules by extension; pack targets go.**
  - A rule names the extensions it packs and its packer: `pack_rule("shader").extensions({".vert", ".frag", ".comp", ".geom"}).packer(packerShader)`.
    An extension belongs to one rule; two rules claiming the same one is an emit-time error.
  - `pack(...)` targets, the `Pack` wrapper and the emitter's pack jobs are removed, and so is `pack("core")`. The graph has no pack jobs until a
    request adds them.
- **Pack requests: any asset a rule's extensions cover, with no build rule per asset.**
  - `pack.request` (variant, a list of asset ids, and optionally the version the client already holds of each) is answered at once with a request id.
  - For each id, the server finds the rule for its extension and, if its graph has no job for the id yet, creates one. All of a request's jobs run in
    one runner call, so a bulk request packs in parallel on the job pool.
  - A job's name, `pack:<id>`, and its command come only from the rule and the id, so they are the same for every request and every server. The build
    log entry is reused: a request for an asset already packed, by an earlier request or a previous server, runs no job.
  - Each asset is answered on its own as it finishes: its bytes as `pack.data`, then `pack.ready`, or `pack.failed` (id, errors). An asset the client
    already holds in the current version gets `pack.ready` with no data.
  - An id no rule covers, or whose source file doesn't exist, gets `pack.failed` naming it.
  - Identical requests from several clients share one job.
- **Nothing packs without a request.** No watcher, no repack after a `build.cpp` change, and no repack when a client connects.
- **Graph and helper reload.**
  - When `build.cpp` changes, the server re-runs the graph stage, swaps in the new IR, and keeps serving.
  - Requested jobs are created again from the new rules. A job whose command changed (a rule's parameters or version) is dirty, and the next
    request for the asset repacks it.
  - When the build system's own sources change (`src/build/**`), the next client's self-build produces new helpers. The client sees that the running
    server's helper version differs and asks it to restart. The server finishes its running work and re-executes itself.
- **ngen-view needs a build server.** At start-up it connects to the project's build server, and exits with an error naming the problem when none is
  running. It requests its start-up shaders in one `pack.request`, waits for all of them, and then initialises the renderer from the streamed
  bytes. It reads nothing under `packs/`.
- **A lock per variant.** `.ngen-buildlog.lock` is held by whoever runs the runner, server or `--no-server`. A second runner on the same variant waits
  for it.
- **`server.status`**: variants loaded, queue, running jobs, clients, and pack bytes queued per client. **`server.shutdown`**, called by
  `ngen-build --shutdown`.

**Out**

- **ngen-view, ngen-editor or `ngen-cli view` starting a server.** A runtime process doesn't spawn build infrastructure; a view with no server
  doesn't start.
- **Remote connections.** The design needs no shared files, but the transport stays loopback only, without authentication, as every endpoint in the
  umbrella.
- **A client-side cache.** A view holds what it received in memory; every start streams its data again (no packing, since the server's log is warm).
- Remote builders, a distributed or shared cache, and build farms.
- Priority between clients, or cancelling a running pack job. Requests queue in arrival order.
- **Assets discovered by a packer.** A packer that finds further assets, such as a USD scene's references, will send them as new requests: requests,
  not extra rounds inside one build. The mechanism comes with the first packer that needs it, the scene pack plan; the request path here is what
  those requests use.
- **Source edits reaching a running view** (hot reload). See Deferred.

## Decisions

Proposed unless marked locked; pushback welcome.

1. **The RPC core lives in `src/rpc/`, and `src/build/serve/` depends on it.** The framework itself (`framework/`, `ir/`, `run/`) stays std-only with
   no dependency on `src/rpc/`; only the server and the client code in `ngen-build` use it. `src/rpc/` keeps its split:
   - the core, used by everything: transport, framing, JSON-RPC, discovery, calls in both directions
   - the engine layer, used by ngen-view, the editor and the introspection tool: the method registry with parameter schemas, and dispatch onto the
     main and render threads

   The framework can still be lifted into another project together with `src/rpc/`'s core.
2. **Serve is a mode, implemented as a third self-built helper.** `ngen-build --serve` self-builds `_out/ngen-build-serve` next to `ngen-build-graph`
   and `ngen-build-run`, then execs it. The bootstrap stays one hand-compiled file, and the server links the runner library and the RPC core like any
   helper.
3. **Starting a server.**
   - A client starts it detached: `setsid`, with stdout and stderr to `_out/run/ngen-build-serve.log`.
   - The client then waits up to five seconds for the discovery file and a successful `server.status`.
   - If two clients start at once, one server wins, by a lock on `_out/run/ngen-build.lock`, and the other connects to it.
   - A stale discovery file whose pid is gone is removed by the next client.
4. **The server stops after an idle hour.** No clients and no running work for 60 minutes, then it exits and removes its discovery file. A connected
   view is a client. `--idle-timeout=<minutes>` changes it, and `0` means never. An always-running server that nobody stops would linger across
   sessions and hold stale helpers; a short timeout would restart it all the time.
5. **Output is identical to a one-shot build.** The client renders the server's `build.output` lines with the same progress formatting (`[done/total]`,
   the tty `\r` redraw, `-v`, `-vv`), and returns the same exit code. The terminal logic lives in the client, in one place, shared by `ngen-build` and
   `ngen-cli`.
6. **The runner runs in-process, one run per variant at a time.** Queued `build.run`s and pack requests for the same variant are merged into the next
   run. Requests for different variants run in parallel, each with its own IR, log and lock. There's one job pool for the whole server, sized to the
   machine, so two variants don't oversubscribe it.
7. **Packed data is streamed over the connection** (locked with Henrik). The server reads a finished asset's packed file and sends it; a client
   never opens a file of the server's.
   - The bytes go as `pack.data` notifications: `{request, id, version, offset, total}` with up to 1 MiB of the file as the frame's attachment, in
     order, then `pack.ready` `{request, id, version, size}`.
   - Chunks of different assets interleave, so a large texture doesn't hold back the small assets requested with it.
   - **Flow control:** the server keeps at most 8 MiB of pack data queued per connection, and sends the next chunk as the queue drains. A slow client
     then never reaches the RPC server's 256 MiB limit, which would drop it.
   - The packed file on disk stays the cache the build log tracks; streaming is how it reaches clients.
8. **Progress and results are messages on the same connection**, not subscriptions: `build.output`, `build.done`, `pack.data`, `pack.ready`,
   `pack.failed`.
9. **A view without a build server doesn't start** (locked with Henrik). No program other than the build path starts a server, and the view doesn't
   fall back to files on disk. The message names the fix: `./ngen-cli build` starts a server.
10. **An asset's version is the content hash of its packed file** (locked). It is the output hash the build log already records for its job.
    `pack.data` and `pack.ready` carry it, and a request's held versions are compared with it.
11. **No pack targets: start-up shaders are requested too** (locked with Henrik). `build.cpp` lists extensions and packers, never assets. There is
    one path for every asset, engine shaders and user assets alike. Keeping `pack("core")` for start-up, and a target that packs every matching file
    in the tree, were the alternatives.
12. **The one-shot path takes `pack:<asset id>` targets** (locked). `ngen-build [--no-server] … pack:shaders/gbuffer.vert` resolves the rule the way
    a request does, with the same `pack_job_edge` call, and writes the packed file. A CI run or an offline agent can pack an asset without a server.
13. **Until the pipeline registry exists, the start-up shaders are a list.** `startupShaderIds()` in `src/renderer/shaderloader.h` names the 17
    shaders the passes load in `init()`, so the view can request them in one batch before the renderer initialises. `loadShaderModule` fails, naming
    the id, for a shader that isn't on the list, which keeps it honest. [plan_async_shaders.md](plan_async_shaders.md) replaces the list with the
    shader ids of the registered pipelines. The alternative was landing the registry first; this keeps the plans independent.

## Steps

1. **Rules by extension** (`src/build/framework/packrule.hpp`, `src/build/ir/`):
   - `PackRule::extensions(std::vector<std::string>)` replaces `match`; `ir::PackRule::patterns` becomes `extensions`. The IR format version goes
     up, so the bootstrap is rebuilt.
   - `Pack`, `pack()`, `emit_pack` and `pack("core")` in `build.cpp` are removed. ngen-view's target no longer depends on a pack; it depends on
     the packer programs, so they are built with it.
   - `emit_pack_rules` reports an extension claimed by two rules.
2. **Pack jobs on demand** (`src/build/ir/packjob.hpp`): the one place a pack job edge is built, used by the server's requests and by `pack:<id>`
   targets on the one-shot path, so both give the same edge for an id.

   ```cpp
   // The rule whose extensions include the asset id's extension; nullptr when none does.
   auto find_pack_rule(const IR& ir, const std::string& asset_id) -> const PackRule*;

   // The job that packs `asset_id` with `rule`: named pack:<asset id>, writing <packs_root>/<asset id> and its depfile.
   auto pack_job_edge(const PackRule& rule, const std::string& asset_id, const std::string& packs_root) -> Edge;
   ```

   `ngen-build-run` appends the job for a `pack:<id>` target that isn't in the IR before it calls `execute`.
3. **RPC core additions** (`src/rpc/core/`):
   - `RpcConnection`: an outgoing connection with its own I/O thread, for long-running clients such as the view. It sends requests with a response
     handler, and hands incoming notifications to a handler, instead of answering them with `methodNotFound` as `RpcClient` does.
   - `RpcServer::queuedBytes(connection)`, and a handler called when a connection's queue drains below a threshold, for the pack stream's flow
     control.
4. **`src/build/serve/`:**
   - the server loop, with per-variant state, loaded on first use: the IR, with the requested jobs added to it; the build log; the pack reverse index
   - `pack.request`: `find_pack_rule` and `pack_job_edge` per id with no job yet, then one run with the request's jobs as its targets
   - the pack stream: per connection, a queue of finished assets sent as `pack.data` chunks within the 8 MiB window, then `pack.ready`
   - the job pool, with one runner call per variant at a time
   - the methods: `build.*`, `pack.*`, `server.*`
   - the idle timeout
   - `src/build/serve/main.cpp` is the `ngen-build-serve` entry point; it compiles `src/rpc/core/` into the helper like its own sources
5. **`src/build/client/`:**
   - ensure-server (discovery, the start lock, the detached start, the wait, the helper-version check and restart)
   - `build.run` with the output renderer
   - used by `ngen-build`'s default path and by `ngen-cli build`
6. **Bootstrap:**
   - `self_build_ir()` gains the `ngen-build-serve` edge
   - the default path becomes: self-build helpers → ensure server → send the build
   - `--no-server` keeps today's path
   - `--serve` execs the server helper
7. **Runner:**
   - the variant lock around `execute`
   - an output sink in `RunOptions`, so the server forwards progress lines instead of printing them
   - no change for requested jobs: they are ordinary edges in the IR the server passes to `execute`, so dirty checks, the build log and the reverse
     index cover them like any edge
8. **The pack client** (`src/pack/packclient.h/.cpp`, a `packclient` library on the RPC core, no pxr), the engine side of the stream:

   ```cpp
   struct PackedAsset {
       std::string id;
       std::string version;
       std::vector<std::byte> bytes;
   };

   class PackClient {
   public:
       // Finds the project's build server through discovery and connects; an error when none is running.
       auto connect(const std::string& variant) -> std::expected<void, std::string>;

       // Sends one pack.request for all of `ids`; returns at once.
       auto request(std::span<const std::string> ids) -> void;

       // Blocks until every id in `ids` has arrived or failed.
       auto wait(std::span<const std::string> ids) -> void;

       // The asset once it has arrived; nullptr before that, or when it failed.
       auto find(const std::string& id) const -> const PackedAsset*;

       // The packer's errors for an asset that failed; empty otherwise.
       auto errors(const std::string& id) const -> std::vector<std::string>;
   };
   ```

   `pack.data` chunks are assembled on the connection's I/O thread; an asset becomes visible to `find` when its `pack.ready` arrives.
9. **ngen-view start-up** (`src/apps/view.cpp`, `src/renderer/shaderloader.*`):
   - `PackClient::connect` for the variant the binary was built for; on failure, print the error and `./ngen-cli build` as the fix, and exit 1
     before a window is created
   - `request(startupShaderIds())`, then `wait`
   - `setShaderPackRoot` becomes `setShaderSource(const PackClient*)`; `loadShaderModule` takes the bytes from `find(id)`, and logs the packer's
     errors and returns nullptr for an asset that failed
10. **`ngen-cli`:**
    - `build` uses the client library instead of exec'ing `ngen-build`
    - `view` runs the view and nothing else
    - no commands of its own for the server: `ngen-cli build` starts it, and `ngen-cli build --shutdown` stops it
11. **Docs:**
    - `src/build/README.md`: server mode as the default, `--no-server`, `--serve`, the methods, starting and stopping, the idle timeout, locking, and
      `pack:<id>` targets
    - `src/pack/README.md`: rules by extension, packing by request, the stream and its flow control, `PackClient`; the pack target sections go
    - `src/rpc/README.md`: `RpcConnection` and the queue-drain handler
    - `src/renderer/README.md`: shaders are requested at start-up
    - `AGENTS.md` and the run-headless skill: agents build through `ngen-cli build`, which starts the server, before any view run; a view run with no
      server fails. `ngen-build --no-server` is only for debugging the build system

## Verification

- **Server is the default.**
  - With no server running, `./_out/ngen-build -p linux-vulkan -c debug ngen-view` starts one (a discovery file appears), builds through it and exits
    0. The server process is still running afterwards.
  - A second call reuses it: same pid in `server.status`.
- **Same result both ways.** A build through the server and a `--no-server` build after `--clean` produce byte-identical linked binaries, and
  build-log entries with the same hashes.
- **Output is identical:**
  - the printed lines of a server build match a `--no-server` build of the same targets, in `-v` mode, compared line by line
  - with a compile error, the exit code and the compiler message are the same both ways
- **`--no-server`** runs with no server contact (no discovery file created), and waits on the variant lock while a server is building that variant.
- **Concurrent starts:** two clients started at once from an empty state end with one server process and both builds done.
- **Helper restart:** after touching a file under `src/build/`, the next `ngen-build` self-builds and the server restarts. `server.status` shows a new
  pid and the new helper version, and the build succeeds.
- **Idle timeout:** with `--idle-timeout=1`, the server exits a minute after the last client, and its discovery file is gone. It doesn't while a view
  is connected.
- **`ngen-cli`:** `./ngen-cli build` on an empty state starts the server and builds. `./ngen-cli build --shutdown` stops it (its discovery file is
  gone), and the next `./ngen-cli build` starts a new one. `./ngen-cli view` with no server leaves no discovery file behind.
- **Pack requests:**
  - `pack.request` for `shaders/debugview.frag` after `--clean` gives `pack.data` chunks whose bytes, concatenated, equal the server's
    `packs/shaders/debugview.frag`, then `pack.ready` whose version equals the build log's output hash for `pack:shaders/debugview.frag`
  - a second request streams the same bytes and runs no job; a request that holds that version gets `pack.ready` with no `pack.data`
  - an id no rule covers gives `pack.failed` naming the id, and so does `shaders/missing.frag`, which a rule covers but doesn't exist
- **Nothing packs without a request:**
  - after editing `shaders/lighting.frag` or adding a parameter to the shader rule in `build.cpp`, no job runs, however long the server runs
  - the next `pack.request` for it repacks it and answers with a new version; one more request runs no job
- **A new asset:** `shaders/requesttest.frag`, created while the server runs, with `build.cpp` unchanged:
  - `pack.request` streams it, and the bytes are identical to `glslc` run by hand with the rule's flags
  - there is no graph reload, and `build.targets` is unchanged
  - `packs/.ngen-packdeps` lists it
  - after `ngen-build --shutdown` and a new server, the same request runs no job
- **Bulk and streaming:**
  - one `pack.request` for all 17 shaders after `--clean` runs their jobs in one runner call, in parallel (the `-v` output of that run lists all 17)
  - a client that reads slowly receives a 300 MiB stream of assets complete and in order, `server.status` never shows more than 8 MiB queued for it,
    and the server doesn't drop it
- **Start-up:**
  - with the server running and `packs/` deleted, the view on three_cubes sends one `pack.request` with the 17 start-up ids, waits, and renders a
    screenshot byte-identical to the baseline. The six headless screenshots are byte-identical to the baseline this way.
  - `strace -e trace=openat` on the view shows no file opened under `packs/`
  - with no server, the view exits 1 before creating a window, and its message names `./ngen-cli build`
- **One-shot:** `ngen-build --no-server … pack:shaders/gbuffer.vert` after `--clean` writes the same file a request does, and a request afterwards
  runs no job.
- **Graph reload:** adding a target to `build.cpp` makes it appear in `build.targets` without restarting the server.

## Gaps

- The graph is reloaded in full after a `build.cpp` change; there's no incremental re-emit.
- Only one server per project root, on one machine: no remote or shared builds.
- No cancellation or priority; a long pack job delays later requests on the same variant.
- A server whose `build.cpp` fails to re-emit keeps serving the last good IR and reports the error. It doesn't stop.
- Which assets were requested is held only in the server. A new server starts with none, and a client asks again; the build log makes that cheap.
- A view's first start on a clean cache waits for its start-up shaders to pack, in one parallel run.
- Every view start streams its data again; there is no client-side cache.
- The view can't run without a build server: not offline, and not in a shipping build.
- A background process is now a normal part of building. Its log is `_out/run/ngen-build-serve.log`, and `ngen-build --shutdown` (or `ngen-cli build
  --shutdown`) or the idle timeout ends it.

## Deferred / follow-ups

- **Priority and cancellation.** Trigger: interactive requests (a shader being edited) wait behind bulk packing.
- **Incremental graph reload.** Trigger: re-emit time after `build.cpp` edits is noticeable.
- **A client-side cache** keyed by asset id and version, so a view sends the versions it holds and receives only what changed. Trigger: start-up
  transfer time matters, or a view on another machine.
- **Remote connections:** binding beyond loopback, and authentication. Trigger: a view on another machine or a devkit.
- **Remote builders and a shared cache.** Trigger: more than one machine builds the project.
- **Source edits reaching a running view** (hot reload): whether the server watches sources and tells clients what is stale, or clients ask again
  when told to. Nothing packs without a request either way. Trigger: the architecture is up and running.
- **Requests from packers for the assets they discover.** Trigger: the scene pack plan's USD packer, the first packer that references other assets.
