# Build server

**Status. Draft.**

Part of step 2 of [plan_tool_architecture.md](plan_tool_architecture.md). It needs:
- the build system under `src/build/` ([plan_build_into_src.md](plan_build_into_src.md))
- the RPC core from step 1
- pack rules in the IR, and pack jobs as edges ([plan_pack_rules.md](plan_pack_rules.md))

It provides what Phase B of [plan_async_shaders.md](plan_async_shaders.md) and the scene packing need: `pack.request` and `pack.ready`, and file
watching with repack.

## Current state

- **`ngen-build` is one-shot, in three stages** (`src/build/bootstrap.cpp` after the move):
  - it self-builds `_out/ngen-build-graph` and `_out/ngen-build-run`
  - it runs the graph stage as a subprocess, which writes `_out/<platform>/<config>/build.ngenir`
  - it runs the runner as a subprocess against that IR
  - every invocation re-reads the IR and the build log from disk, and exits
- **The runner is a library.** `ngen::run::execute(IR&, RunOptions&)` in `src/build/run/execute.hpp` covers dirty detection with content hashing and
  a stat fast path, depfiles, the parallel scheduler and the build log. `ngen-build-run` is a thin CLI over it.
- **Nothing guards the build log.** Two concurrent `ngen-build` calls for the same variant would both write `.ngen-buildlog`.
- **The framework is std-only and self-contained**, and stays so after the move ([plan_build_into_src.md](plan_build_into_src.md), Decision 1).
- **`ngen-cli build` forwards to `ngen-build`** with `execv`, and fills in `-p`/`-c` from `_out/set` (`src/apps/cli.cpp`).
- **After [plan_pack_rules.md](plan_pack_rules.md)**, the IR carries pack rules and the packs root. Each asset a static pack target lists is a pack
  job edge in the build log, writing `<out_dir>/packs/<asset id>`, and the runner rewrites the reverse index `packs/.ngen-packdeps` after every build.
  Nothing takes a request at runtime.

## Scope

**In**

- **Server mode is the default.** Every build goes through a build server, so the request flow is always exercised.
  - `ngen-build -p … -c … [targets]` makes sure a server is running for the project, starting one if needed. It sends the build as a request, prints
    the server's progress, and exits with the build's exit code. The server keeps running.
  - `ngen-build --no-server -p … -c … [targets]` is today's in-process one-shot, with no server. It's for bootstrapping problems, CI that wants no
    background process, and debugging the build system itself.
  - `ngen-build --serve` runs the server in the foreground. Clients start it this way, detached; it's also how you debug the server.
  - `ngen-build --shutdown` stops the running server for this project. `ngen-cli build --shutdown` is the same flag passed through, since `build`
    passes every argument on.
- **`ngen-cli` is the everyday entry point.** For a command that needs the server (`build`, and `view` once views request packs), it checks
  discovery, starts `ngen-build --serve` detached if no server is running, and then talks to it over RPC. `ngen-cli build` sends `build.run` itself,
  instead of exec'ing `ngen-build`.
- **The server holds, per variant in use:** the loaded IR, the build log in memory, pack jobs included, and the pack reverse index. It registers in
  discovery (`_out/run/ngen-build-<pid>.json`: project root, port, pid, helper version).
- **Builds over RPC.**
  - `build.variants` and `build.targets`.
  - `build.run` (variant, targets, flags) runs the in-process runner. The server calls back on the same connection: `build.output` for each output
    line, and `build.done` with the counts and exit status.
  - `build.clean` does the same as `--clean`.
- **Pack requests.**
  - `pack.request` (variant, asset id) is answered at once with a request id and the version already cached, if any.
  - The server resolves the rule from the IR, runs the job and the jobs it requests, and calls back per asset with `pack.ready` (id, version, pack
    path) or `pack.failed` (id, errors).
  - Identical requests from several clients share one job.
- **Interest and watching.**
  - The server remembers which client asked for which asset ids, and drops that interest when the client disconnects.
  - A file watcher covers the sources and dependency paths of those assets. On a change, the reverse index gives the affected ids; the server repacks
    them and sends `pack.ready` with the new version to every client with an interest.
- **Graph and helper reload.**
  - When `build.cpp` changes, the server re-runs the graph stage, swaps in the new IR, and keeps serving.
  - When the build system's own sources change (`src/build/**`), the next client's self-build produces new helpers. The client sees that the running
    server's helper version differs and asks it to restart. The server finishes its running work and re-executes itself.
- **A lock per variant.** `.ngen-buildlog.lock` is held by whoever runs the runner, server or `--no-server`. A second runner on the same variant waits
  for it.
- **`server.status`**: variants loaded, queue, running jobs, clients and interests. **`server.shutdown`**, called by `ngen-build --shutdown`.

**Out**

- **ngen-view and ngen-editor starting a server themselves.**
  - A runtime process shouldn't spawn build infrastructure. `ngen-cli view` makes sure a server is running before it starts the view, and the view
    connects to it.
  - A view started directly with no server uses static packs.
- Remote builders, a distributed or shared cache, and build farms.
- Watchers other than Linux's `inotify`. There's a watcher interface, so others can be added.
- Authentication. Loopback only, as every endpoint in the umbrella.
- Priority between clients, or cancelling a running pack job. Requests queue in arrival order.

## Decisions

Proposed; pushback welcome.

1. **The RPC core lives in `src/rpc/`, and `src/build/serve/` depends on it.** With the build system under `src/` there is no longer a
   reason to put the core inside the framework. The framework itself (`framework/`, `ir/`, `run/`) stays std-only with no dependency on `src/rpc/`;
   only the server and the client code in `ngen-build` use it. `src/rpc/` keeps a clear split:
   - the core, used by everything: transport, framing, JSON-RPC, discovery, calls in both directions; the standard library plus header-only
     nlohmann/json (`external/json`, [plan_rpc.md](plan_rpc.md))
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
4. **The server stops after an idle hour.** No clients and no running work for 60 minutes, then it exits and removes its discovery file.
   `--idle-timeout=<minutes>` changes it, and `0` means never. An always-running server that nobody stops would linger across sessions and hold stale
   helpers; a short timeout would restart it all the time.
5. **Output is identical to a one-shot build.** The client renders the server's `build.output` lines with the same progress formatting (`[done/total]`,
   the tty `\r` redraw, `-v`, `-vv`), and returns the same exit code. The terminal logic lives in the client, in one place, shared by `ngen-build` and
   `ngen-cli`.
6. **The runner runs in-process, one run per variant at a time.** Queued `build.run`s and pack jobs for the same variant are merged into the next run.
   Requests for different variants run in parallel, each with its own IR, log and lock. There's one job pool for the whole server, sized to the
   machine, so two variants don't oversubscribe it.
7. **Pack results are one file per asset**, at `_out/<platform>/<config>/packs/<asset id>`, the same path whether a static target or a request packed
   it. A client reads the asset's file on `pack.ready`, so a repack of one texture rewrites one file.
8. **Progress and results are calls back on the same connection**, not subscriptions: `build.output`, `build.done`, `pack.ready`, `pack.failed`.
   Step 1's calls in both directions suffice.
9. **The watcher debounces.** Changes are collected for 50 ms before repacking, so an editor's save burst or a checkout produces one repack per asset.

## Steps

1. **`src/rpc/` core** (shared with step 1 of the umbrella): TCP on loopback, length-prefixed frames, JSON-RPC 2.0, discovery files in `_out/run/`, a
   dispatcher, and calls in both directions.
2. **`src/build/serve/`:**
   - the server loop, with per-variant state (IR, build log and pack reverse index, all loaded on first use)
   - the job pool, with one runner call per variant at a time
   - the methods: `build.*`, `pack.*`, `server.*`
   - the idle timeout
   - `src/build/serve/main.cpp` is the `ngen-build-serve` entry point
3. **`src/build/client/`:**
   - ensure-server (discovery, the start lock, the detached start, the wait, the helper-version check and restart)
   - `build.run` with the output renderer
   - used by `ngen-build`'s default path and by `ngen-cli`
4. **Bootstrap:**
   - `self_build_ir()` gains the `ngen-build-serve` edge
   - the default path becomes: self-build helpers → ensure server → send the build
   - `--no-server` keeps today's path
   - `--serve` execs the server helper
5. **Runner:**
   - the variant lock around `execute`
   - an output sink in `RunOptions`, so the server forwards progress lines instead of printing them
   - pack job edges created at request time, for an asset that no static target covers ([plan_pack_rules.md](plan_pack_rules.md) emits them for
     static targets only)
6. **Watcher** (`src/build/serve/watcher.hpp`, an interface with an `inotify` implementation):
   - it watches the sources and recorded dependencies of assets with an interest, plus `build.cpp`
   - debounce, then reverse-index lookup, then a repack, then `pack.ready` to the interested clients
7. **`ngen-cli`:**
   - `build` uses the client library instead of exec'ing `ngen-build`
   - `view` ensures the server before starting the view
   - no commands of its own for the server: it starts on demand, and `ngen-cli build --shutdown` stops it
8. **Docs:**
   - `src/build/README.md`: server mode as the default, `--no-server`, `--serve`, the methods, starting and stopping, the idle timeout, locking and
     the watcher
   - `AGENTS.md` and the run-headless skill: agents build through `ngen-cli build`, which goes through the server like a human's build, and use
     `ngen-build --no-server` only when debugging the build system

## Verification

- **Server is the default.**
  - With no server running, `./_out/ngen-build -p linux-vulkan -c debug ngen-view` starts one (a discovery file appears), builds through it and exits
    0. The server process is still running afterwards.
  - A second call reuses it: same pid in `server.status`.
- **Same result both ways.** A build through the server and a `--no-server` build after `--clean` produce byte-identical linked binaries and
  packed files, and build-log entries with the same hashes.
- **Output is identical:**
  - the printed lines of a server build match a `--no-server` build of the same targets, in `-v` mode, compared line by line
  - with a compile error, the exit code and the compiler message are the same both ways
- **`--no-server`** runs with no server contact (no discovery file created), and waits on the variant lock while a server is building that variant.
- **Concurrent starts:** two clients started at once from an empty state end with one server process and both builds done.
- **Helper restart:** after touching a file under `src/build/`, the next `ngen-build` self-builds and the server restarts. `server.status` shows a new
  pid and the new helper version, and the build succeeds.
- **Idle timeout:** with `--idle-timeout=1`, the server exits a minute after the last client, and its discovery file is gone.
- **`ngen-cli`:** `./ngen-cli build` on an empty state starts the server and builds. `./ngen-cli build --shutdown` stops it (its discovery file is
  gone), and the next `./ngen-cli build` starts a new one.
- **Pack requests:**
  - `pack.request` for `shaders/debugview.frag` on a clean cache gives `pack.ready` with a version and an existing packed file, `packs/shaders/debugview.frag`
  - a second request answers at once from the cache, with no job run
  - an id with no matching rule gives `pack.failed` naming the id
- **Watching:**
  - with a client interested in `shaders/lighting.frag`, saving an edit gives one `pack.ready` with a new version within one second
  - editing a file it includes gives `pack.ready` for every shader that includes it, and only those
  - after the client disconnects, a further edit repacks nothing
- **Graph reload:** adding a target to `build.cpp` makes it appear in `build.targets` without restarting the server.

## Gaps

- The graph is reloaded in full after a `build.cpp` change; there's no incremental re-emit.
- Only one server per project root, on one machine: no remote or shared builds.
- Linux watcher only, until another backend is written.
- No cancellation or priority; a long pack job delays later requests on the same variant.
- A server whose `build.cpp` fails to re-emit keeps serving the last good IR and reports the error. It doesn't stop.
- A background process is now a normal part of building. Its log is `_out/run/ngen-build-serve.log`, and `ngen-build --shutdown` (or `ngen-cli build
  --shutdown`) or the idle timeout ends it.

## Deferred / follow-ups

- **Priority and cancellation.** Trigger: interactive requests (a shader being edited) wait behind bulk packing.
- **Incremental graph reload.** Trigger: re-emit time after `build.cpp` edits is noticeable.
- **Remote builders and a shared cache.** Trigger: more than one machine builds the project.
- **Watchers for macOS and Windows.** Trigger: a second host platform.
