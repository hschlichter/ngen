# Asset server

**Status. Landed.**

Part of step 2 of [plan_tool_architecture.md](plan_tool_architecture.md). It supersedes [plan_build_server.md](plan_build_server.md): packing moves out
of the build system into its own program, `ngen-asset-server`, so builds and packing are separate concerns. A unified build and asset server is
still the likely end state (Deferred). It needs the RPC core from step 1 ([plan_rpc.md](plan_rpc.md), landed) and the shader packer from
[plan_pack_rules.md](plan_pack_rules.md). It provides what [plan_async_shaders.md](plan_async_shaders.md) and the scene packing need.

**Packing is request-driven.** The root `pack.cpp` declares which file extensions can be packed, and the packer program that packs each. Which assets
get packed is decided at runtime, by requests: a user drops an asset of a supported type into the project, something requests it (in the end, mostly
a USD scene referencing it), and it is packed. Nothing is ever packed without a request. The engine's own start-up shaders go the same way.

**All packed data is streamed.** The asset server sends a packed asset's bytes over the RPC connection. A client never reads the server's files, so a
view could in principle run on another machine.

## Current state

- **Packing lives in the build system** ([plan_pack_rules.md](plan_pack_rules.md)):
  - `build.cpp` has `pack_rule("shader")` with glob patterns and per-config parameters, and `pack("core")` listing every shader
  - the IR (format version 2) carries the rules and the packs root; each listed asset is a `pack:<id>` edge with `kEdgeFlagPack`
  - the runner writes `packs/.ngen-packdeps` after every build
  - ngen-view depends on `core` and reads `_out/<platform>/<config>/packs/<id>` from disk (`src/renderer/shaderloader.*`)
- **Packers are standalone programs** on `src/pack/packer.h`: `ngen-packer-shader` runs `glslc` and writes a depfile.
- **The RPC core exists** (`src/rpc/core/`), standard library plus nlohmann/json:
  - frames carry a JSON text and a binary attachment, up to 256 MiB per frame
  - `RpcServer` has an I/O thread, and drops a client whose outgoing queue passes 256 MiB
  - `RpcClient` is blocking, for command-line tools: it answers anything the peer sends it with `methodNotFound`
- **ngen-build is one-shot**, and stays that way.

## Scope

**In**

- **Packing leaves the build system.** `packrule.hpp`, `pack(...)` targets, the IR's pack rules and packs root, `kEdgeFlagPack` and the pack index
  go. `build.cpp` builds the asset server and the packers as ordinary programs, and nothing else about packing.
- **`pack.cpp` at the project root** holds the pack rules, as `build.cpp` holds the build. It is compiled into `ngen-asset-server`.
- **`ngen-asset-server`**, a program target in `build.cpp`, one binary per variant (`_out/<platform>/<config>/ngen-asset-server`). It uses nothing
  from `src/build/`: its own cache, its own job runner, the RPC core.
  - started by hand, and runs until it is stopped (Ctrl-C or SIGTERM), removing its discovery file on exit
  - registers in discovery as kind `asset`, with its variant as the label
- **Pack requests.**
  - `pack.request` (a list of asset ids, and optionally the version the client holds of each) is answered at once with a request id.
  - For each id, the server finds the rule for its extension. It runs the packer when its cache says the packed asset is out of date, several jobs
    in parallel, so a bulk request packs in parallel.
  - Each asset is answered on its own as it finishes: its bytes as `pack.data`, then `pack.ready`, or `pack.failed` (id, errors). An asset the client
    already holds in the current version gets `pack.ready` with no data.
  - An id no rule covers, or whose source file doesn't exist, gets `pack.failed` naming it.
  - Identical ids requested at once, by one client or several, share one job.
- **Nothing packs without a request.** No watching, and nothing packed at start-up.
- **The cache** (`<out_dir>/packs/`): the packed files at `packs/<asset id>`, and one record per asset saying what they were packed from.
- **ngen-view needs an asset server.** At start-up it connects to its variant's asset server, and exits with an error when none is running. It
  requests its start-up shaders in one `pack.request`, waits for all of them, and initialises the renderer from the streamed bytes. It reads nothing
  under `packs/`.
- **`server.status`**: pid, variant, clients, jobs running and queued, and pack bytes queued per client.

**Out**

- **Starting the server automatically.** No program starts it; `ngen-view` without one exits with an error that says how to start it.
- **Building packers.** `ngen-build` builds them, and the asset server runs what it finds next to itself.
- **Remote connections.** The design needs no shared files, but the transport stays loopback only, without authentication.
- **A client-side cache.** A view holds what it received in memory; every start streams its data again, without packing.
- **Assets discovered by a packer**, such as a USD scene's references. They will be sent as new requests; the mechanism comes with the scene pack plan.
- **Source edits reaching a running view** (hot reload). See Deferred.
- Priority between clients, and cancelling a running job. Requests queue in arrival order.

## Decisions

Proposed unless marked locked; pushback welcome.

1. **A separate asset server, using nothing from the build system** (locked with Henrik). Builds and packing are separate concerns until a unified
   server is worth it. The cost is some duplication: a job runner, a content hash and a depfile parser exist in both.
2. **`pack.cpp` is compiled into the server** (locked with Henrik that it exists at the root, as `build.cpp` does). It includes `src/pack/packrule.h`
   and defines the rules as data:

   ```cpp
   auto packRules(const std::string& config) -> std::vector<PackRule> {
       std::vector<PackRule> rules;
       rules.push_back(PackRule{
           .name = "shader",
           .extensions = {".vert", ".frag", ".comp", ".geom"},
           .packer = "ngen-packer-shader",
           .params = {
               {"optimize", config == "debug" ? "0" : "1"},
               {"debug_info", config == "gamerelease" ? "0" : "1"},
           },
           .version = 1,
       });
       return rules;
   }
   ```

   `build.cpp` lists `pack.cpp` as one of the server's sources, so a rule change is a rebuild of the server. An extension belongs to one rule; two
   rules claiming the same one stop the server at start-up. The config comes from the server's location, `_out/<platform>/<config>/`, as the
   project root does for discovery (`rpcProjectRoot`). I lean this over the server compiling or loading `pack.cpp` at runtime, which would repeat what
   `ngen-build-graph` does for `build.cpp`.
3. **One server per variant** (from being a program target). The view connects to the asset server whose label is its own variant and whose project
   root is its own.
4. **Started by hand** (locked with Henrik). `./_out/linux-vulkan/debug/ngen-asset-server`. A view with no asset server exits with an error naming
   that command.
5. **The packer contract is unchanged.** A job runs `<dir of the server>/<packer> --rule <name> --rule-version <n> --asset <id> --source <id>
   --out packs/<id> --depfile packs/<id>.d [--param key=value]...`, as the pack rules plan defined it, so the SPIR-V stays byte-identical. Jobs run
   with `posix_spawn`, no shell, up to the machine's hardware concurrency at once.
6. **The cache record says when a packed asset is up to date.** Per asset, `packs/.ngen-assetcache` records:
   - the job key: rule name, version, parameters and packer name
   - the content hash of the packer binary
   - the content hash of the source and of every file in the depfile
   - the content hash of the packed file, which is the asset's version

   An asset is up to date when its packed file exists and every recorded hash matches. A stat fast path (size and modification time) skips
   re-hashing unchanged files. The hash is FNV-1a 64 (`src/pack/hash.h`).
7. **An asset's version is the content hash of its packed file** (locked). `pack.data` and `pack.ready` carry it, and a request's held versions are
   compared with it.
8. **Packed data is streamed over the connection** (locked with Henrik).
   - The bytes go as `pack.data` notifications, `{request, id, version, offset, total}` with up to 1 MiB of the file as the frame's attachment, in
     order, then `pack.ready` `{request, id, version, size}`.
   - Chunks of different assets interleave, so a large texture doesn't hold back the small assets requested with it.
   - **Flow control:** at most 8 MiB of pack data is queued per connection; the next chunk is sent as the queue drains. A slow client never reaches the
     RPC server's 256 MiB limit, which would drop it.
9. **Client connections reuse `RpcServer`'s I/O loop.** `RpcServer::connect(port)` opens an outgoing connection that the same I/O thread serves, and
   `startWithoutListening()` runs the loop with no listening socket. The view's `PackClient` and any later long-running client use it. A separate
   `RpcConnection` class was the alternative; it would duplicate the poll loop, framing and call matching.
10. **Until the pipeline registry exists, the start-up shaders are a list.** `startupShaderIds()` in `src/renderer/shaderloader.h` names the 17
    shaders the passes load in `init()`, so the view can request them in one batch before the renderer initialises. `loadShaderModule` fails, naming
    the id, for a shader not on the list. [plan_async_shaders.md](plan_async_shaders.md) replaces the list with the shader ids of the registered
    pipelines.

## Steps

1. **Take packing out of the build system:**
   - remove `src/build/framework/packrule.hpp`, `Project::pack_rule`, `Pack` and `pack()`, `emit_pack` and `emit_pack_rules`
   - remove `IR::pack_rules`, `IR::packs_root` and `kEdgeFlagPack` from the schema, writer, reader and JSON dump; the IR format version becomes 3,
     so the bootstrap is rebuilt
   - remove `write_pack_index` from `src/build/run/execute.hpp`
   - `build.cpp`: remove `shaderRule` and `corePack`; ngen-view depends on the asset server and the packers instead of `core`
   - `src/build/README.md`: the Packing section goes
2. **Rules** (`src/pack/packrule.h`, `pack.cpp`):

   ```cpp
   struct PackRule {
       std::string name;
       std::vector<std::string> extensions;
       std::string packer;
       std::vector<std::pair<std::string, std::string>> params;
       uint32_t version = 0;
   };

   // Defined in the project's pack.cpp.
   auto packRules(const std::string& config) -> std::vector<PackRule>;
   ```
3. **The cache** (`src/pack/server/assetcache.h/.cpp`, `src/pack/hash.h`): load and save `packs/.ngen-assetcache` through a temporary file and a
   rename; `isUpToDate(id, rule)`; `record(id, rule, depfile)` after a successful job; a Make depfile parser.
4. **Jobs** (`src/pack/server/packjobs.h/.cpp`): a queue of jobs run with `posix_spawn` on a fixed number of worker threads, capturing each packer's
   output for `pack.failed`. A job for an id already queued or running is joined instead of added.
5. **RPC core** (`src/rpc/core/rpcserver.*`): `connect(port)`, `startWithoutListening()`, `queuedBytes(connection)`, and a handler called when a
   connection's queue drains below a threshold.
6. **The server** (`src/pack/server/assetserver.h/.cpp`, `src/apps/assetserver.cpp`):
   - start-up: the variant from the executable's location, `packRules(config)`, the extension check, the cache, the RPC server, the discovery file
   - `pack.request` and `server.status`
   - the stream: per connection, finished assets sent as interleaved `pack.data` chunks within the 8 MiB window, each followed by `pack.ready`
   - SIGINT and SIGTERM: stop, and remove the discovery file
   - `build.cpp`: an `ngen-asset-server` program over `src/apps/assetserver.cpp`, `src/pack/server/*.cpp` and `pack.cpp`, linking `rpccore`
7. **The pack client** (`src/pack/packclient.h/.cpp`, a `packclient` library on `rpccore`, no pxr):

   ```cpp
   struct PackedAsset {
       std::string id;
       std::string version;
       std::vector<std::byte> bytes;
   };

   class PackClient {
   public:
       // Finds the asset server of this variant and project through discovery and connects; an error when none is running.
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
8. **ngen-view start-up** (`src/apps/view.cpp`, `src/renderer/shaderloader.*`):
   - `PackClient::connect` for the variant the binary lives in; on failure, print the error and the command that starts the server, and exit 1
     before a window is created
   - `request(startupShaderIds())`, then `wait`
   - `setShaderPackRoot` becomes `setShaderSource(const PackClient*)`; `loadShaderModule` takes the bytes from `find(id)`, and logs the packer's
     errors and returns nullptr for an asset that failed
9. **Docs:**
   - `src/pack/README.md`: `pack.cpp` and rules by extension, the asset server, the cache, requests, the stream and its flow control, `PackClient`
   - `src/rpc/README.md`: `connect`, `startWithoutListening` and the queue-drain handler
   - `src/renderer/README.md`: shaders are requested at start-up
   - `AGENTS.md` and the run-headless skill: start `ngen-asset-server` in the background before any view run; a view run without one fails
   - root `README.md`: the asset server in the build-and-run section

## Verification

- **The build system is free of packing:** `rg -i pack src/build` finds nothing about pack rules or jobs, and a build of every target in all three
  configurations succeeds.
- **Byte-identical SPIR-V.** Every shader streamed from the asset server, in `debug`, `release` and `gamerelease`, is byte-identical to the `.spv` the
  old `shaders` tool produced (51 of 51).
- **Start-up:**
  - with the asset server running and `packs/` deleted, the view on three_cubes sends one `pack.request` with the 17 start-up ids, waits, and renders
    a screenshot byte-identical to the baseline. The six headless screenshots are byte-identical to the baseline this way.
  - `strace -e trace=openat` on the view shows no file opened under `packs/`
  - with no asset server, the view exits 1 before creating a window, and its message names the command that starts one
- **Requests:**
  - `pack.request` for `shaders/debugview.frag` after deleting `packs/` gives `pack.data` chunks whose bytes, concatenated, equal
    `packs/shaders/debugview.frag`, then `pack.ready` whose version is that file's hash
  - a second request streams the same bytes and runs no job; a request holding that version gets `pack.ready` with no `pack.data`
  - an id no rule covers gives `pack.failed` naming the id, and so does `shaders/missing.frag`, which a rule covers but doesn't exist
  - two clients requesting the same id at once cause one job
- **A new asset:** `shaders/requesttest.frag`, created while the server runs, with nothing else changed: `pack.request` streams it, and the bytes are
  identical to `glslc` run by hand with the rule's flags.
- **What repacks**, each checked by the jobs the next request runs:
  - nothing changed: nothing, also after restarting the server
  - one shader edited: that shader only
  - a file included by two shaders edited: exactly those two
  - a rule parameter changed in `pack.cpp`, and the server rebuilt: every shader, on request
  - the shader packer's code changed and rebuilt: every shader, on request
- **Nothing packs without a request:** after editing `shaders/lighting.frag`, the server runs no job until a request names it.
- **Bulk and streaming:**
  - one `pack.request` for all 17 shaders after deleting `packs/` runs jobs in parallel (`server.status` during the run shows more than one running)
  - a client that reads slowly receives a 300 MiB set of assets complete and in order, `server.status` never shows more than 8 MiB queued for it,
    and the server doesn't drop it. The test uses a copy rule for `.bin` added to `pack.cpp` for the check only.
- **Stopping:** Ctrl-C on the server removes its discovery file, and a connected view is told the connection closed.

## Gaps

- Packers aren't rebuilt by the asset server; a packer changed in the source is used after the next `ngen-build`.
- A rule change in `pack.cpp` takes effect after the server is rebuilt and restarted.
- A view's first start on an empty cache waits for its start-up shaders to pack, in parallel.
- Every view start streams its data again; there is no client-side cache.
- The view can't run without an asset server: not offline, and not in a shipping build.
- A job runner, a content hash and a depfile parser exist in both the build system and the asset server.

## Deferred / follow-ups

- **A unified build and asset server.** Trigger: the two keep needing the same state, for example packers rebuilt on demand, or rules that depend
  on build targets.
- **Source edits reaching a running view** (hot reload). Nothing packs without a request either way. Trigger: the architecture is up and running.
- **Requests from packers for the assets they discover.** Trigger: the scene pack plan's USD packer.
- **A client-side cache** keyed by asset id and version. Trigger: start-up transfer time matters, or a view on another machine.
- **Remote connections:** binding beyond loopback, and authentication. Trigger: a view on another machine or a devkit.
- **Priority and cancellation.** Trigger: interactive requests wait behind bulk packing.

## Results

- **The build system is free of packing.** `packrule.hpp`, the IR's pack rules, packs root and pack flag, and the pack index are gone; the IR is
  format version 3. All three configurations and the examples build.
- **Byte-identical SPIR-V.** All 17 shaders streamed by each variant's server match the old `shaders` tool's `.spv` files, in `debug`, `release`
  and `gamerelease` (51 of 51).
- **Start-up.** With `packs/` deleted and the server running, the six headless screenshots are byte-identical to the baseline, with 18 shader
  loads. `strace -f -e trace=openat` on the view shows none of its 599 opens under `packs/`. With no server, the view exits 1 before loading the
  scene or creating a window, and prints `start one with ./_out/linux-vulkan/debug/ngen-asset-server`.
- **Requests**, checked with a test client over the socket:
  - streamed bytes equal `packs/shaders/debugview.frag`, and the version equals that file's FNV-1a 64
  - a second request streams the same bytes and runs no packer; a request holding the version gets `pack.ready` with `sent: false` and no data
  - `.xyz`, `shaders/missing.frag` and `../etc/passwd` each get `pack.failed` naming the reason
  - two clients requesting `shaders/lighting.frag` at once cause one packer run
  - `shaders/requesttest.frag`, created while the server ran, streams bytes identical to `glslc` run by hand
- **What repacks**, counted by `packerRuns` and the server's `PACK` lines: nothing changed, also after a restart: 0. One shader edited: that one.
  Two shaders made to include a new file, then the file edited: exactly those two, both times. The packer's code changed so its binary changed:
  all 17, on request (a change that compiles to a byte-identical binary repacks nothing, as it should). A parameter added in `pack.cpp` and the
  server rebuilt: all 17. Restored: 17 again, then 0.
- **Nothing packs without a request:** two seconds after editing `shaders/lighting.frag`, no packer ran; the next request ran one.
- **Bulk:** one request for all 17 shaders on an empty cache ran 17 packers, with 17 tasks running at the peak.
- **Streaming:** with a temporary copy rule for `.bin`, a client with a 64 KiB receive buffer reading slowly got 31 assets, 300 MiB, complete and
  identical. The queue never passed 7.98 MiB (sampled, and the server's recorded peak), and the 3 KB asset's data arrived as chunk 34 of 301,
  between the large ones.
- **Stopping:** SIGINT removes the discovery file, and a connected client sees the connection close.

Deviations from the plan:

- **The view requests its shaders before loading the scene** and waits just before the renderer is created, so packing overlaps the USD load.
- **A chunk is sent only when it fits in the window**, so the 8 MiB bound holds with the chunk counted.
- **Asset ids are checked**: an absolute path or one with `..` is refused.
- **Additions for observability:** the server prints a `PACK <id>` line per packer run, and `server.status` reports `packerRuns` and each client's
  peak queued bytes. `pack.ready` carries `sent`.
- **Not observed directly:** that the view sends exactly one `pack.request` (it's one call in the code, but nothing counts requests), and a
  running *view* seeing the server stop (checked with the test client).

Renamed after landing (assets are the system, packing is the process): `src/pack/` became `src/asset/`, with the packing code in
`src/asset/pack/`; `PackClient` became `AssetClient` (`assetclient.*`, library `assetclient`), `hash.h` became `assethash.h`, and the `pack`
library became `packer`. The methods are `asset.request`, `asset.data`, `asset.ready` and `asset.failed`, and the cache is
`_out/<platform>/<config>/assets/`.

Changed after landing (2026-10-02): there is no project root. The asset server's working directory is the root asset ids are relative to; its
cache is `.ngen-assets/<platform>/<config>/` there, and every tool's discovery file is in `.ngen-discovery/` in its working directory, so a view
finds the asset server it runs next to. The view's scene argument is an asset id as given. Binaries can be copied anywhere: a copy of the debug
binaries in an unrelated folder, run against a folder holding only `shaders/` and a scene, rendered the baseline image.
