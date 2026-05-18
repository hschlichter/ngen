# Build System

ngen's build system is a small header-only C++ framework you write your project graph against (in `build.cpp` at the project root), plus a runner that
executes that graph in parallel. Three top-level directories under `build/`, each with a distinct responsibility:

- **`framework/`** — the configuration API. `build::Target`, `build::Project`, `build::Platform`, `build::Configuration`, the `build::cxx` language module,
  plus the auxiliary `Tool` and `Alias` wrappers.
- **`ir/`** — the binary IR that carries a frozen build graph from the configuration layer to the executor, plus `ir::Emitter` which walks a `Project` to
  produce one.
- **`run/`** — the executor library (`ngen::run::execute()`) and its standalone CLI (`ngen-build-run`). Dirty detection, scheduler, depfile parsing, build
  log.

Every file under `build/` carries a prose header explaining what it solves, what it exports, and how it fits in. **This document is the high-level
architecture and usage; the file headers are the details.** When you want to know how a specific type works or what a class's invariants are, open the
corresponding `.hpp`.

---

## Fresh-clone bootstrap

One C++ compile produces `_out/ngen-build`:

```sh
mkdir -p _out && c++ -std=c++23 -O0 -g -pthread -o _out/ngen-build build/bootstrap.cpp
```

From then on, `ngen-build` is the only entry point. It rebuilds `ngen-build-graph` and `ngen-build-run` on demand using the runner library, then drives them
as subprocesses.

If `bootstrap.cpp` itself changes, re-run the seed command above. The runner can rebuild every other binary but not itself.

---

## Directory map

```text
build.cpp           # The project graph at the project root. Defines platforms, configs, targets. The one file most users edit.
build/
  bootstrap.cpp     # ngen-build orchestrator. Self-builds graph + runner via the runner library, then runs them.
  build_system.md   # This document.
  framework/        # Configuration API. Target / Project / Platform / Configuration / cxx language module / Tool / Alias.
  ir/               # IR schema, writer, reader, JSON dump, Emitter, compile-commands extractor, graph-stage main, vendored xxhash.
  run/              # Runner: execute, scheduler, process, hash, buildlog, depfile, progress.
```

`build.cpp` lives at the project root because it's *project-specific* configuration, not part of the build system. Everything under `build/` is the build
system itself — header-only framework, IR, runner, and the orchestrator. To bring the build system to a different project, copy `build/` and write a new
`build.cpp`.

Dependency direction: `ir/` depends on `framework/`, never the reverse. `run/` depends on `ir/` and `framework/`, never the reverse. `bootstrap.cpp` depends
on `ir/` (to construct the self-build IR) and on `run/` (to execute it). `build.cpp` depends on `framework/` and `ir/emit.hpp`.

For per-file documentation — what each header is for, what it exports, how it fits — open the file. Every `.hpp` in `build/` carries a prose header at the top.

---

## Mental model

Three layers, each built from a small set of types.

### Core (language-agnostic — `build/framework/`)

```text
build::Target         → graph node (name, deps, platform/config gating, ExtensionMap)
build::Project        → entry targets + registered platforms + registered configs
build::Platform       → environment identity (name, os, graphics_api, exe_suffix) + ExtensionMap
build::Configuration  → variant identity (name, out_dir) + ExtensionMap
build::ExtensionMap   → type-erased attachment point that everything language-specific hangs from
```

The core types carry zero language vocabulary. They model "what to build", "where", and "how identities relate". Anything language-specific lives in
extensions attached through `ExtensionMap`. See `build/framework/extensionmap.hpp`.

### C++ language module (`build::cxx` — `build/framework/cxx/`)

A parallel namespace mirroring the core, with each type as a fluent wrapper that owns its corresponding core type via `shared_ptr` and registers itself as
the cxx extension:

```text
build::cxx::Toolchain      → compiler / archiver / linker / default_std (composed inside cxx::Platform)
build::cxx::Platform       → wraps build::Platform; per-platform compile_flags / link_flags / defines / system_libs
build::cxx::Configuration  → wraps build::Configuration; per-config compile_flags / link_flags / defines
build::cxx::Target         → wraps build::Target; one node per library/program
build::cxx::ObjectFile     → wraps build::Target; one node per translation unit
```

A library or program is `cxx::Target`; each `.cpp` it owns is a `cxx::ObjectFile` child whose base is dep-edged to the parent's base. Compile edges are
emitted from the ObjectFile node, archive/link edges from the parent. The framework graph is one node per TU plus one node per library/program — sources
are first-class, not an opaque list inside the parent.

### Generic auxiliary (`Tool`, `Alias`)

Both follow the cxx wrapper pattern: own a `shared_ptr<Target>`, attach themselves to the base's `ExtensionMap`, expose a fluent builder.

- **`Tool`** runs an opaque shell command (`glslc`, `rm`, `clang-format`, `clang-tidy`, …). Per-variant by default; `global()` makes it variant-independent.
  See `build/framework/tool.hpp`.
- **`Alias`** resolves to another target based on `(platform, config)` selectors — used for graph-level
  indirection like a `gpu-backend` alias that resolves to different backend libraries per platform. See
  `build/framework/alias.hpp`.

### IR transport (`build/ir/`)

The graph stage walks the `Project` and produces one `ir::IR` per `(platform, config)` variant. Commands are **fully baked** into shell strings at emit
time — no `$cflags` templating, no rule expansion, no variables. The runner just executes them. The IR is a flat binary format with a fixed header, fixed-size
record arrays, and a string table at the end. See `build/ir/schema.hpp` for the in-memory types and the byte layout, and `build/ir/emit.hpp` for the walk.

### Runner (`build/run/`)

A single entry point `ngen::run::execute(IR&, RunOptions&)` does the whole lifecycle: load the build log, compute the dirty set with content hashing and a
stat fast-path, propagate dirty along the DAG, hand a Plan to the parallel scheduler, update the log on each success, save atomically at the end. See
`build/run/execute.hpp`.

---

## Bootstrap and execution flow

```text
fresh-clone seed (documented one-line `c++` invocation in CLAUDE.md):
  $ c++ -std=c++23 -O0 -g -pthread -o _out/ngen-build build/bootstrap.cpp
  → produces _out/ngen-build

every subsequent invocation:
ngen-build
  → build a small in-memory IR (`self_build_ir()`) with two edges (graph, runner)
  → ngen::run::execute(self_build_ir, opts)   ← runner library, in-process
      → if stale: c++ ... build.cpp           → _out/ngen-build-graph
      → if stale: c++ ... build/run/main.cpp  → _out/ngen-build-run
  → ngen-build-graph                          → _out/<plat>/<cfg>/build.ngenir per variant
  → ngen-build-run --ir <variant>/build.ngenir <target>   ← subprocess, executes the project IR
```

There is one execution engine — the runner library at `build/run/` — and it does both build-system self-build (in-process inside `ngen-build`) and project
build (as a subprocess invocation of `_out/ngen-build-run`). Both use the same dirty detection, content hashing, scheduler, and build log format. They differ
only in which IR they execute and where its build log lives:

- **Self-build IR.** Constructed in memory on every `ngen-build` invocation by `bootstrap.cpp::self_build_ir()`. Two edges. Never written to disk. Build log
  at `_out/.system/.ngen-buildlog`.
- **Project IR.** Emitted by `ngen-build-graph` to `_out/<plat>/<cfg>/build.ngenir`. Build log at `_out/<plat>/<cfg>/.ngen-buildlog`.

Header tracking uses `-MMD -MF $out.d` for both paths: any new header under `build/framework/`, `build/ir/`, or `build/run/` automatically becomes a build
dependency on the next invocation, picked up by the runner's depfile parser. No heredoc updates, no manifest changes.

---

## CLI

```text
./_out/ngen-build (--platform|-p) <name> (--config|-c) <name> [--clean|--rebuild] [--compile-commands] [(--list|-l)] [--dump-graph] [-v|-vv] [target]
./_out/ngen-build (--help|-h)        — usage + available platforms / configs / targets
```

`--platform` / `-p` and `--config` / `-c` are required — the build system carries no project-specific defaults.
A bare invocation (or any invocation missing either flag) prints the same panel `--help` shows, then exits with
an error pointing at the missing flag. The project-only listing (no flag reference) is available via `--list`.

Targets are positional and may repeat. Each query goes through `build::ir::resolve_target` (in
`build/ir/resolve.hpp`) before reaching the runner. The resolution rule:

1. **Exact match** on any edge name → that edge.
2. Else **fuzzy substring** on ObjectFile source stems (case-insensitive) → every matching `.cpp` is built.
3. Else **fuzzy substring** on non-ObjectFile edge names (libraries, programs, tools, aliases) → every match
   is built.
4. Else the original query is forwarded; the runner produces an "unknown target" error.

So `ngen-build -p X -c Y render` builds every source whose stem contains `render`; `ngen-build -p X -c Y
renderer` (exact) builds the library; `ngen-build -p X -c Y rhivu` falls through to tier 3 and resolves to the
`rhivulkan` library. Stem matches always expand to every hit — there's no cap. Substring is case-insensitive.
Resolution is silent; the runner's normal `[done/total] description` output makes which edges actually ran
self-evident.

When no positional target is given, the runner falls back to the project's `default_target()`. Internal
targets that aren't registered as roots are not top-level invokable but are reached via traversal from
registered entry points.

Special flags:

- `--help` / `-h` — print bootstrap-side usage + flag list, then the `--list` panel from the graph stage, then exit.
- `--list` / `-l` — print platforms, configs, and top-level targets, then exit (handled by `ngen-build-graph`).
- `--dump-graph` — dump the IR as JSON (one object per variant) to stdout and exit.
- `--clean` — remove `_out/<plat>/<cfg>/` for the chosen variant, then exit. Requires `-p` and `-c`.
  Bootstrap-level — no graph stage, no runner, just `std::filesystem::remove_all` on the variant dir.
- `--rebuild` — same wipe as `--clean`, then fall through to the normal pipeline. Graph re-emits the IR;
  the runner sees no build log and rebuilds every reachable edge. Requires `-p` and `-c`.
- `--compile-commands` — derive `compile_commands.json` from the on-disk IR for the chosen variant and write
  it to `_out/<plat>/<cfg>/compile_commands.json`, plus an updated merged top-level `_out/compile_commands.json`
  (union of every variant's IR currently on disk). Opt-in; the runner and emitter are unaware of this file.
  Implemented by `build::ir::compile_command_entries` over an `IR` value loaded via `ir::read`. Requires `-p`
  and `-c`. Exits without running the build.

Verbosity:

- default — `[done/total] description` with `\r`-overwrite on a tty.
- `-v` — one line per edge, no `\r` tricks. Same effect as `TERM=dumb`; suitable for scripts and log capture.
- `-vv` — also echo each `$ command` before running.

`NO_COLOR=1` (or any non-empty value) suppresses ANSI escapes regardless of tty.

The runner is also directly invokable: `./_out/ngen-build-run --ir <path> [-j N] [-k N] [-v|-vv] [target ...]`. `-j` defaults to `nproc`; `-k` defaults to 1
(fail fast). See `build/run/main.cpp`.

---

## Adding a platform or configuration

Use the cxx factories and register with the project:

```cpp
auto clang = cxx::toolchain()
    .compiler("clang++")
    .archiver("ar")
    .default_std("c++23");

auto my_platform = cxx::platform("my-platform")
    .os("linux")
    .toolchain(clang)
    .compile_flag("-fPIC")
    .define("MY_PLATFORM")
    .system_lib("m");

auto debug = cxx::configuration("debug")
    .out_dir("_out")
    .compile_flag("-O0")
    .compile_flag("-g")
    .define("DEBUG=1");

Project p;
p.platform(my_platform);
p.config(debug);
```

To add a new platform, construct another `cxx::platform("name")` chain and register it. Same for configurations. For the full fluent surface (per-target
overrides, link inputs, includes, etc.), see `build/framework/cxx/target.hpp` and `build/framework/cxx/platform.hpp`.

---

## Adding another language

The framework is designed so that adding a new language module is purely additive:

1. Create `build/framework/<lang>/{toolchain,platform,configuration,target,commands}.hpp`.
2. Each follows the cxx pattern: a wrapper that owns a `shared_ptr` to the corresponding `build::*` base, attaches itself as an extension, exposes a fluent
   surface. (See the existing cxx files as the reference shape.)
3. Add a free factory `<lang>::<lang>(name)` for the language target equivalent (the analogue of `cxx::program` / `cxx::static_library`).
4. Add a backend dispatch branch in `build/ir/emit.hpp`'s `emit_target`:

   ```cpp
   else if (auto* x = target->extension<lang::Target>()) { /* push edges into ir_ */ }
   ```

   The branch builds a `Command` (argv list) via `<lang>::cmd::*` builders, bakes it into the edge's `command` string via `bake_command`, and pushes an
   `ir::Edge` into the variant emitter. No language-specific knowledge bleeds into the runner — it just executes `/bin/sh -c <baked-command>`.

No edits to `build::Target`, `build::Project`, `build::Platform`, or `build::Configuration` are required.

---

## Things to know

System-level invariants worth knowing before changing the code. Implementation details belong with the relevant `.hpp`.

- **No exceptions.** The framework uses `std::expected<T, build::Error>` at every boundary. `<stdexcept>` is not included anywhere in `build/`.
- **Wrapper move/copy invariant.** Every cxx wrapper (and `Tool`, `Alias`) re-attaches itself to the base's `ExtensionMap` in both move and copy
  constructors. If a future field is added to one of these wrappers, both constructors must be updated. `cxx::ObjectFile` is the deliberate exception — it
  lives behind `shared_ptr` from construction, never gets copied or moved by user code, and is `=delete`d for both.
- **Per-TU graph nodes.** Each `.cpp` is its own `build::Target` (with a `cxx::ObjectFile` extension). ObjectFile names like
  `renderer/src/renderer/foo.cpp` are unique build edges in the graph but are not surfaced via `Project::roots()`. Code that walks `Project::build_all()`
  looking only for "real" libraries should filter on `extension<cxx::ObjectFile>()` first.
- **Archive non-determinism.** `ar rcs` embeds mtimes. A modify-then-revert cycle takes one extra build to converge to a no-op — the round trip flushes new
  mtimes into the archive, the runner re-records the archive's hash, and the next run sees inputs unchanged. Switch to `ar rcsD` if byte-identical archives
  matter.
- **Synthetic outputs.** Global tools (`format`, `tidy`) have no real file outputs; the IR emitter gives them the target name as a virtual output so they're
  addressable. The runner's dirty rule naturally treats the missing virtual output as dirty, so global tools always run when invoked — matching user
  expectation.
- **`compile_commands.json`** is opt-in via the orchestrator's `--compile-commands` flag. The emitter and runner
  carry zero knowledge of it; the file is derived from the on-disk IR by `build::ir::compile_command_entries`
  (in `build/ir/compile_commands.hpp`). Per-variant file lives under `_out/<platform>/<config>/`; the merged
  top-level file at `_out/compile_commands.json` is the union of every variant's IR currently on disk. Naive
  concatenation; entries are not de-duplicated across variants.
- **`-Wl,--start-group` / `--end-group`** wraps every program's archives at link time so over-linking works without curating transitive link order.
- **System build log location.** Self-build state lives at `_out/.system/.ngen-buildlog`. Same format as project-build logs. Deleting it forces a clean
  rebuild of `ngen-build-graph` and `ngen-build-run` on the next invocation.

---

## Where to read next

Every `.hpp` and `.cpp` in `build/` carries a prose header. Open the ones whose role you need to understand. Suggested entry points for a top-down read:

- [`build/bootstrap.cpp`](bootstrap.cpp) — the orchestrator. Start here.
- [`build/framework/target.hpp`](framework/target.hpp) and [`build/framework/extensionmap.hpp`](framework/extensionmap.hpp) — the seam between the language-agnostic core and language-specific extensions.
- [`build/framework/cxx/target.hpp`](framework/cxx/target.hpp) — the user-facing cxx surface for libraries and programs.
- [`build/ir/schema.hpp`](ir/schema.hpp) — the IR types and binary wire format.
- [`build/ir/emit.hpp`](ir/emit.hpp) — how a `Project` becomes an `IR`. The central translation step.
- [`build/run/execute.hpp`](run/execute.hpp) — how the runner consumes an IR. Dirty detection, scheduling, log writeback.
- [`build/run/scheduler.hpp`](run/scheduler.hpp) — the parallel execution model.
