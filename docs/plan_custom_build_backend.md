# Plan: Custom Build Backend (`ngen-build-run`)

This plan replaces ninja as the project's build executor with a custom backend `ngen-build-run`. The graph stage emits a bespoke binary IR; the runner reads
that IR and executes it. The boundary between graph and execution becomes a real, on-disk file rather than a ninja syntax stream.

Companion to `build_system.md`. Read that first for context on the existing four-stage chain, the framework, and how the graph is currently produced.

---

## 1. Goals and non-goals

**Goals.**
- Replace ninja with our own runner so the build system is fully self-hosted past the bootstrap seed.
- Sharpen the boundary between "produce the graph" (stage 3) and "execute the graph" (stage 4): the IR is a versioned binary file written by stage 3 and read by
  stage 4.
- Keep the runner small enough that one person can hold it in their head: dumb (cmd, inputs, outputs) executor, no template engine, no expression language.
- Hash-based dirty detection (xxhash) with an mtime fast-path so no-op builds stay stat-only.
- `-j N` parallelism with a single `console` pool for serialized tools; `-k N` continue-past-failure with default 1.
- Ninja-style progress display (`[done/total] description`), `-v` / `-vv` verbosity matching today.
- Preserve `compile_commands.json` emission and the existing CLI shape of `ngen-build`.

**Non-goals (this plan).**
- Watching / daemon mode. Designed-around but explicitly deferred.
- Windows/macOS execution. The thin process abstraction lands now; only the POSIX implementation is wired up.
- Fancy scheduling: job weights, priority by critical path, distributed builds.
- Replacing ninja in the build-system *self-bootstrap* chain. `bootstrap.ninja`, `_out/ngen-build-pre.ninja`, `_out/ngen-build-graph.ninja`, and the new
  `_out/ngen-build-run.ninja` continue to use ninja. Only the *project* graph (the user's libraries/programs/tools) moves to the new runner.
- Optimizing for build speed beyond what falls out of the design. Once the runner is correct we measure, then tune.

---

## 2. Architecture at a glance

Bootstrap chain after this lands:

```text
build/bootstrap.ninja
  → _out/ngen-build                  (stage 1, from build/bootstrap.cpp)
  → _out/ngen-build-pre              (stage 2, from build/prebuild.cpp)
  → _out/ngen-build-graph            (stage 3, from build/build.cpp + framework headers)
  → _out/ngen-build-run              (stage 4, from build/run/*.cpp)             ← NEW
  → _out/<platform>/<config>/build.ngenir   (emitted by ngen-build-graph)        ← REPLACES build.ninja
  → ngen-build-run executes the IR                                                ← REPLACES final ninja invocation
```

Stage 4 (`ngen-build-run`) is built in parallel with stage 3 — they have no dependency between them. `prebuild.cpp` emits two ninja manifests
(`_out/ngen-build-graph.ninja` and `_out/ngen-build-run.ninja`); `bootstrap.cpp` runs them concurrently when both need rebuilding.

Process boundary: the orchestrator (`ngen-build`) shells out to `ngen-build-graph` to produce IR, then to `ngen-build-run` to execute. Strict separation, two
processes. A future single-process mode is possible but explicitly out of scope for now.

---

## 3. File layout

```text
build/
  bootstrap.ninja                # unchanged
  bootstrap.cpp                  # updated: invokes ngen-build-run instead of `ninja` for the project build
  prebuild.cpp                   # updated: emits ngen-build-graph.ninja AND ngen-build-run.ninja
  build.cpp                      # updated: graph emits IR via the new IR backend
  run/                           # NEW — runner binary lives here, multi-TU
    main.cpp                     # entry point + CLI parsing
    buildlog.hpp                 # persistent edge state (single binary file at _out/.ngen-buildlog)
    buildlog.cpp
    hash.hpp                     # xxhash3 file hashing with (path,size,mtime,ctime) fast-path
    hash.cpp
    process.hpp                  # thin spawn/wait abstraction; POSIX impl
    process.cpp
    scheduler.hpp                # ready-queue driven worker pool, console pool, -k N, Ctrl+C
    scheduler.cpp
    depfile.hpp                  # parse Make-style .d files
    depfile.cpp
    progress.hpp                 # ninja-style progress line + verbosity modes
    progress.cpp
  framework/
    ... (existing headers unchanged)
    ir/                          # NEW — shared IR schema, used by stage 3 (writer) and stage 4 (reader)
      schema.hpp                 # POD value types: Header, Edge, Pool, StringRef, etc.
      writer.hpp                 # build::ir::write(IR&, Path) — header-only
      reader.hpp                 # build::ir::read(Path) -> std::expected<IR, Error> — header-only
      json.hpp                   # build::ir::dump_json(IR&, std::ostream&) — for --dump-graph
      xxhash.h                   # vendored single-header xxhash; placed here, NOT under external/
    backendir.hpp                # NEW — assembles IR from Project; replaces backendninja.hpp's role
```

`framework/backendninja.hpp` is deleted at the end of phase 5. During phases 1-4 both backends coexist behind `--backend ir|ninja` so we can validate.

The runner is the first part of the build system that is multi-TU. Framework stays header-only as it has been; the runner under `build/run/` uses regular
`.hpp/.cpp` split because the implementations are real (subprocess management, file hashing) and there's no benefit to forcing them into one TU.

---

## 4. IR design

### 4.1 What goes in the IR

The IR represents one fully-resolved build variant: every `(platform, config)` pair gets its own file at `_out/<platform>/<config>/build.ngenir`. There is no
runtime variant filtering; the runner just executes what it's handed.

A `build.ngenir` file contains:

- **Header.** Magic (`NGIR`), format version, generated-at timestamp (ns), variant string (`"linux-vulkan/debug"`), project root absolute path, offsets into the
  rest of the file.
- **String table.** All strings in the file (paths, command lines, descriptions) are stored once at the end and referenced by `(offset, length)`. Edges hold
  `StringRef` indices, not inline strings.
- **Pools.** Index 0 is always `default` (depth 0 = unbounded, capped by `-j N`). Index 1 is `console` (depth 1, runs serialized, output goes straight to TTY).
  Future pools just append.
- **Edges.** A flat array. Each edge:
  - `name` — string ref (used as the build log key; uniqueness guaranteed by the graph stage).
  - `command` — fully baked shell command string. No `$cflags`, no rule templates. The command is exactly what gets passed to `/bin/sh -c`.
  - `inputs` — array of string refs (paths, relative to project root).
  - `outputs` — array of string refs.
  - `implicit_deps` — array of string refs. Runner treats them like inputs for hashing but doesn't pass them to the command.
  - `order_only_deps` — array of string refs. Force build ordering but don't trigger rebuild on change.
  - `pool` — pool index.
  - `depfile` — string ref (output path containing Make-format depfile after the edge runs); empty if none.
  - `description` — string ref; what the progress line shows (`"CXX src/foo.cpp.o"`).
  - `flags` — bitfield, mostly reserved. One bit flags "no-op edge" for phony/alias passthrough.
- **Default targets.** Array of edge indices. Used when the runner is invoked without a target argument.
- **Compile-commands hint section.** Per-edge optional `(directory, file)` pair so `compile_commands.json` can be regenerated from the IR alone if we ever
  want to. (Optional in v1; nice to have.)

Variables, rules, `$in`/`$out` substitution: **none of this exists in the IR**. The graph stage bakes the full command string. This makes the runner trivial
and the IR diff-readable in JSON form.

### 4.2 Binary layout

Little-endian, packed structs, no compression. Layout is "header → fixed-size record arrays → string table at the end". `mmap` the whole file and read in
place; no per-edge allocation needed.

```text
+--------------------------------------+
| Header (fixed size)                  |
+--------------------------------------+
| Pool[]   (count from header)         |
+--------------------------------------+
| Edge[]   (count from header)         |
+--------------------------------------+
| u32[]    default_targets             |
+--------------------------------------+
| String table (concatenated, no NULs) |
+--------------------------------------+
```

`StringRef` is `{ uint32_t offset; uint32_t length; }` into the string table. Arrays referenced from edges (inputs, outputs, etc.) are stored in dedicated
side-arrays; the edge struct holds `(offset, length)` into them. Concrete struct sizes settle during phase 1 implementation.

### 4.3 JSON dump (`--dump-graph`)

A debugging affordance. `ngen-build --dump-graph` (or `ngen-build-graph --dump-graph-json`) re-emits the same IR as JSON to stdout, one object per edge, in
deterministic order. Not a parse target — strictly human-readable. Round-trip not required.

### 4.4 Versioning

Header carries a `format_version` u32. The runner refuses to execute IR with a version it doesn't recognize and asks for a graph regen. We bump the version on
any breaking change to layout or semantics; additive struct fields use reserved bits.

---

## 5. Graph stage changes

The change is local to one place: replace `NinjaBackend` with `IrBackend` in `build/framework/backendir.hpp`. The traversal logic
(`detail::Emitter::emit_target`, dispatch by extension type, alias resolution, dep-walk filtering of `cxx::ObjectFile` from `order_only`, etc.) carries over
nearly unchanged. What was previously "format a ninja string" becomes "append an `Edge` to the in-memory IR and write the file at the end".

Concrete changes:

- The compile-flag composition order, defines precedence, and `-Wl,--start-group` wrapping all happen at IR-emission time, baked into the command string.
- Per-variant `compile_commands.json` and the merged top-level one are still emitted by the graph stage, identical to today. They do not flow through the IR.
- Output directories are still pre-created by the graph stage at emit time. The IR carries no `mkdir` edges.
- `Tool::global(true)` edges (`clean`, `format`, `tidy`) become a single edge in each variant's IR with the same command. The orchestrator's existing rule for
  routing bare `clean`/`format`/`tidy` (without `:platform:config`) stays — it just now invokes `ngen-build-run` with that target name.

---

## 6. The runner

### 6.1 CLI

```text
ngen-build-run --ir <path>  [-j N]  [-k N]  [-v|-vv]  [--dump-graph]  [target ...]
```

- `--ir <path>` — required; path to a `build.ngenir`. The orchestrator passes
  `_out/<platform>/<config>/build.ngenir` based on user-selected variant.
- `-j N` — concurrent jobs, defaults to `nproc`.
- `-k N` — continue past failures up to N total; default 1 (stop on first error).
- `-v` — `TERM=dumb` style: forces non-tty progress (matches today's `--vv` behavior on `ngen-build`? — clarify when wiring up; goal is parity with current
  semantics).
- `-vv` — print every command before running it.
- `target ...` — edges to build (matched against edge names or output paths). Empty = default targets from IR header.

The user-facing `ngen-build` orchestrator is unchanged. It still owns `--platform`, `--config`, target routing, and the special bare-target list
(`clean`/`format`/`tidy`). It just shells out to `ngen-build-run` instead of `ninja`.

### 6.2 Lifecycle of one invocation

1. **Load IR.** `mmap(_out/<plat>/<cfg>/build.ngenir)`. Validate magic + version. Bail with a clear error if either fails.
2. **Load build log.** `_out/.ngen-buildlog` if present. Missing log = treat every edge as dirty (clean build).
3. **Resolve targets.** Map CLI targets to edge indices. `nullopt` = default targets.
4. **Reachability walk.** From requested edges, walk dep edges to compute the active edge set. Inactive edges are ignored entirely.
5. **Dirty pass.** For each active edge in topological order, ask: are inputs + command unchanged since the last successful run? See §8. Mark dirty if not.
6. **Schedule.** Hand the dirty set to the scheduler; it executes them respecting deps + pool constraints. See §9.
7. **Persist.** On every successful edge, update its build log entry. On full-build success, atomically replace `_out/.ngen-buildlog`. On failure, persist
   what succeeded so far so the next run resumes correctly.
8. **Exit.** Status code: 0 success, non-zero on any edge failure (count of failures if `-k > 1`).

Memory budget: the IR mmap is the only large allocation. Edges, hashes, etc. are POD vectors sized by edge count.

---

## 7. Build log

`_out/.ngen-buildlog`. Single binary file. Format mirrors the IR's "fixed-size record + string table" pattern for consistency.

Per-edge entry:

- `edge_name` (string ref) — primary key.
- `command_hash` (xxh64) — hash of the baked command string.
- `inputs[]` — for each input + implicit dep + discovered header: `(path, size, mtime_ns, ctime_ns, content_hash)`.
- `outputs[]` — for each output: `(path, size, mtime_ns, content_hash)`.
- `discovered_headers[]` — additional paths parsed out of the depfile after the last successful run; semantically just more inputs but tracked separately so
  we can re-parse on depfile change without re-scanning everything.
- `last_run_ns` — wall-clock timestamp of the last successful execution.

Persistence strategy: read at startup, mutate in memory, **rewrite the entire file atomically (`write` to `.ngen-buildlog.tmp`, `rename`) at the end of every
successful build**. For a project of this scale (~100 edges) the log is tens of KB; full rewrite is microseconds. No append log, no compaction. If we ever
outgrow this, switch to append + periodic compaction; not before.

Crash safety: if the rename never happens (process killed mid-build), the log on disk reflects the previous successful build. We may redo work but never get
into an inconsistent "thinks it's done but isn't" state. Per-edge mid-build success is *not* persisted to disk; only the final atomic rewrite is durable.

---

## 8. Hashing and dirty detection

### 8.1 Hash

xxhash3 (xxh3_64bits). Single-header `xxhash.h` lives at `build/framework/ir/xxhash.h`, dropped in directly (not vendored as a submodule). License attribution
goes in the file header — we don't edit the upstream copy.

### 8.2 Mtime fast-path

For each tracked file the build log stores `(size, mtime_ns, ctime_ns, content_hash)`. On a subsequent build:

- `stat(path)`. If it fails, file is missing → edge is dirty.
- If `(size, mtime_ns, ctime_ns)` matches the build log → reuse the stored `content_hash`. **No file read.**
- Else → read the file, xxh3 it, store new tuple.

This collapses a no-op build to one `stat` per tracked path, which matches ninja's no-op cost.

### 8.3 Dirty rule

An edge is dirty if any of:

1. The edge has no entry in the build log (first build, or new edge).
2. `command_hash` differs from the log entry (command string changed → flags, defines, etc. changed).
3. Any input / implicit dep / discovered header has a different `content_hash` than recorded.
4. Any declared output is missing from disk, or its `(size, mtime, content_hash)` differs from the log entry.

Order-only deps **do not** participate in dirty detection — they only constrain scheduling order. This matches ninja's semantics.

If the edge is up to date, skip execution but still propagate "build log entry is current" so downstream edges see consistent input hashes.

### 8.4 Depfile ingestion

After a compile edge runs successfully, parse `<output>.d` (Make format), extract the dependency list, and record those paths in the edge's
`discovered_headers`. They are hashed and tracked just like declared inputs on the next run. The depfile parser handles `\`-line continuations,
`\space` escapes, and `#` comments; surface a clear error on malformed input.

If the depfile is missing after a successful run (some edges legitimately don't produce one), record an empty `discovered_headers` list — that is the steady
state for those edges.

---

## 9. Scheduler

### 9.1 Model

- One ready queue per pool. The default pool's queue feeds a worker pool of size `-j N` (default `nproc`). The console pool has depth 1: at most one console
  edge runs at a time, and it runs on the main thread with stdin/stdout/stderr inherited so interactive output works correctly.
- An edge is "ready" when all its dependency edges (regular + order-only) are complete and successful.
- When a worker finishes an edge, it decrements the pending-dep count of every dependent. Edges whose count hits zero get pushed onto their pool's queue.
- Failure handling: when an edge fails, decrement `failures_remaining` (initialized to `-k N`, default 1). If it hits zero, stop scheduling new work but let
  in-flight workers finish. If non-zero, mark dependents as "skipped due to upstream failure" and continue.
- Ctrl+C: install a SIGINT handler that (a) stops scheduling new work, (b) sends SIGINT to all active children, (c) waits for them to drain, (d) exits with a
  non-zero status. No half-rewritten outputs left behind because the build log isn't rewritten until clean exit.

### 9.2 Workers

Each worker is an `std::thread` running a loop: pop ready edge → spawn child via `Process::launch` → wait → capture stdout/stderr → on success update build
log entry → on failure record diagnostic → push notification to main thread.

stdout/stderr buffering: child output is captured into per-edge buffers and flushed *after* the edge completes, in completion order. On failure the buffer is
written verbatim before the error summary. This avoids interleaved output from concurrent edges, which is the same approach ninja uses.

The console pool bypasses capture: child inherits the main process's std streams.

---

## 10. Process abstraction

`build/run/process.hpp`:

```cpp
namespace ngen::run {

struct Process {
    // launch and wait; captures stdout+stderr into out (interleaved, like 2>&1).
    static std::expected<int, Error> run(std::string_view command,
                                         std::string& out_combined,
                                         bool inherit_stdio = false);

    // for the scheduler: launch async, return a handle.
    struct Handle;
    static std::expected<Handle, Error> spawn(std::string_view command, bool inherit_stdio);
    static std::expected<int, Error>    wait(Handle&, std::string& out_combined);
    static void                          signal(Handle&, int sig);  // SIGINT/SIGTERM
};

}
```

POSIX implementation: `posix_spawn` + `pipe(2)` for capture + `waitpid`. A small reactor thread does `poll` on the live read-fds to drain output without
blocking. Single Linux implementation lands now. Header carries the abstraction so a Windows `CreateProcess` impl can drop in later without touching
schedulers or callers.

Commands run with `/bin/sh -c <command>` so shell features (pipes, redirection) work — the graph stage already produces commands that assume a shell. cwd is
the project root (passed by the orchestrator).

---

## 11. Output and progress

Modes:

- **Default.** One progress line per completed edge: `[done/total] description`. Keeps the cursor on a single TTY line when stdout is a tty. When not a tty,
  prints one line per edge (no carriage-return tricks).
- **`-v`** (forwarded from `ngen-build` `-v`). Forces non-tty progress: full `[done/total] description` lines, one per edge, no carriage-return tricks.
  Equivalent to today's `TERM=dumb ninja`.
- **`-vv`**. Echoes the full command before running it, then the captured output. Equivalent to `ninja -v`.

On failure, the runner prints:

```text
FAILED: <output paths>
$ <command>
<captured output>
```

Then the summary `N edges, M failed`.

Color: respect `NO_COLOR` env var; default to colored output when stdout is a tty.

---

## 12. Bootstrap orchestrator changes

`bootstrap.cpp` (the `ngen-build` binary) needs three things adjusted:

1. **Stage 4 build.** After running `_out/ngen-build-pre.ninja` (which now emits both `_out/ngen-build-graph.ninja` and `_out/ngen-build-run.ninja`), invoke
   ninja to build the graph and runner *concurrently*. Both produce binaries, neither depends on the other. A single `ninja -f .../ngen-build-graph.ninja
   ngen-build-run.ninja` style invocation, or two parallel `std::system` calls — whichever is easier with the `// NOLINT(bugprone-command-processor)` boundary
   we already use.
2. **Run the graph.** `_out/ngen-build-graph --variant <plat>/<cfg> --out _out/<plat>/<cfg>/build.ngenir`. Existing dump options (`--dump-graph` JSON) are
   added at the same time.
3. **Run the runner.** Replace the current final `ninja -f _out/build.ninja` invocation with `_out/ngen-build-run --ir _out/<plat>/<cfg>/build.ngenir [target
   ...]`. Forward `-j` / `-k` / `-v` / `-vv` flags as today.

Special-target routing (`clean` / `format` / `tidy` without a `:platform:config` suffix) stays in `bootstrap.cpp` but now resolves to a runner invocation against
the appropriate variant's IR.

`prebuild.cpp` (`ngen-build-pre`) is updated to emit `_out/ngen-build-run.ninja` in addition to the existing `_out/ngen-build-graph.ninja`. Both manifests use
`-MMD -MF $out.d` so framework-header / runner-header changes are picked up automatically.

---

## 13. Phased rollout

Each phase is independently demoable and reversible. Both backends coexist until phase 5.

### Phase 1 — IR types and writer

- Add `framework/ir/{schema,writer,json}.hpp` with the format from §4.
- Add `framework/backendir.hpp`, modelled on `backendninja.hpp`. Same traversal, same dispatch by extension type; output is an `IR` struct that gets written
  via `ir::write`.
- `build/build.cpp` learns `--backend ir`. With `--backend ninja` (default), behavior is unchanged. With `--backend ir`, write `build.ngenir` per variant.
- `--dump-graph` (or `--dump-graph-json`) on `ngen-build-graph` prints JSON.
- **Validation gate**: write a small comparison tool (or one-off script) that takes a `build.ngenir` and the equivalent `build.ninja` and asserts the set of
  `(output, command)` tuples matches. Run it across all three configs. Phase doesn't merge until they match.
- Ship: nothing user-visible. Both backends produce equivalent graphs.

### Phase 2 — Runner skeleton (serial, no caching)

- Add `build/run/main.cpp` + minimal `process.{hpp,cpp}` (synchronous `Process::run`). No build log, no parallelism, no progress display, no depfile parsing.
- `prebuild.cpp` learns to emit `_out/ngen-build-run.ninja`. `bootstrap.cpp` builds it alongside the graph.
- `ngen-build-run --ir <path> [target]` walks the IR in topological order and runs every edge. Always rebuilds everything.
- `ngen-build` learns `--backend run` flag (still defaulting to ninja). With `--backend run`, the orchestrator uses the new runner end to end.
- Ship: `--backend run` works for clean builds across all three configs. Slow because no caching, but functionally complete.

### Phase 3 — Caching and parallelism

- `build/run/{buildlog,hash,depfile,scheduler}.{hpp,cpp}`.
- Async `Process::spawn` / `wait` / `signal`.
- Build log read at start, written atomically at end. Hashing with mtime fast-path. Depfile parsing after each compile edge.
- Scheduler with `-j N`, console pool, `-k N`, Ctrl+C handling.
- Ship: `--backend run` is now competitive with ninja on clean and incremental builds.

### Phase 4 — UX polish

- `progress.{hpp,cpp}` — ninja-style line, `-v` / `-vv` modes, color/NO_COLOR.
- Failure formatting (`FAILED: ... $ command ... <output>`).
- `--dump-graph` JSON wired up end to end.
- Error messages on missing IR, version mismatch, malformed depfile, missing inputs.
- Ship: `--backend run` is feature-parity with the current ninja flow for the workflows in `build_system.md` §11.

### Phase 5 — Cutover

- Make `--backend run` the default in `bootstrap.cpp`.
- Remove `--backend ninja` from the orchestrator CLI; remove ninja-emit code.
- Delete `framework/backendninja.hpp` and any helpers used only by it.
- Update `build_system.md` — replace section 8 (Ninja backend behavior) with the runner equivalent; update §11 (CLI) and §13 (things to know) accordingly.
- Update `CLAUDE.md` build section if any commands changed.
- Ninja remains a build-time dependency only for the bootstrap chain (stages 1–4 ninja manifests). The user-facing experience requires no ninja knowledge.

### Phase 6 — Cleanup and measurement

- Profile the runner. Now we have something to measure. Tighten anything that shows up: hash batching, scheduler contention, build log layout, depfile parser
  hot path.
- Sanity-check no-op build wall time (target: under 50ms for the current ngen graph), clean-build wall time (target: ≥ ninja parity within 5%).
- Document lessons in `build_system.md` §8 if anything surprising came up.

---

## 14. Open questions / future work

- **Removing ninja from the build-system self-bootstrap.** Tracked separately in `plan_remove_ninja_from_bootstrap.md`. Replaces `bootstrap.ninja` and the
  prebuild stage with a three-line shell seed plus a stat-and-compile loop inside `ngen-build`. Starts after this plan's phase 5 lands.
- **Unified runner (end state).** Tracked separately in `plan_unified_runner.md`. Replaces the stat-and-compile loop above with the runner itself, exposing
  `ngen::run::execute()` as a library entry point so `ngen-build` can hand the runner an in-memory IR describing its own dependencies. Starts after
  `plan_remove_ninja_from_bootstrap.md` lands.
- **Single-process mode for the project build.** A `--in-process` flag could collapse stages 3 and 4: graph emit → in-memory IR → execute, no file in
  between. Saves one mmap and a process boundary on no-op rebuilds. Largely subsumed by `plan_unified_runner.md` if we ever want it for the project path too.
- **Watch mode.** Long-running `ngen-build --watch` that holds the IR + build log + recursive inotify watches in memory. Order-of-magnitude inner-loop win for
  iterative dev. Defer until the runner has shipped and we have measurements.
- **Windows.** The `Process` abstraction is in place. `process_win.cpp` is the only file that needs to land; `posix_spawn` calls don't leak into schedulers
  or main.
- **Distributed / sandboxed execution.** Way out of scope. Mention because the IR happens to be a clean unit-of-work boundary that would make this tractable.
- **`compile_commands.json` from IR.** Currently the graph stage writes it. With the per-edge `(directory, file)` hint in the IR, we *could* let the runner
  write it instead, deduplicating across variants. Optional polish.
- **Hash skip on empty diffs.** If an edge's command + inputs hash matches the previous successful run, but the previous run was *interrupted* mid-build,
  re-execution may be needed. Mitigation: the build log only records edges after they completed successfully, so this can't happen — but worth a comment in
  the buildlog code so a future maintainer doesn't change the invariant accidentally.

---

## 15. What to read first when implementing

- `build_system.md` §2 (mental model), §4 (ExtensionMap), §8 (current ninja backend behavior — the IR backend mirrors its traversal exactly).
- `build/framework/backendninja.hpp` — `detail::Emitter::emit_target` is the template for `IrBackend`.
- `build/framework/cxx/backendninja.hpp` — the helpers `compile_command` / `archive_command` / `link_command` already produce baked command strings; the IR
  backend reuses them verbatim.
- `build/prebuild.cpp` — pattern for emitting an additional ninja manifest for `ngen-build-run`.
- `build/bootstrap.cpp` — for the orchestrator changes in §12.

The traversal logic does not need to be rewritten. The change is entirely in what the leaf "emit" calls do: append to an `IR` struct instead of writing
ninja syntax.
