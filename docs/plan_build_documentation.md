# Plan: Document `build/` for a Cold Reader

The build system has reached the shape it's going to keep for a while — runner-driven, three responsibility-split directories (`framework/`, `ir/`, `run/`),
no ninja. Now the goal is making every file legible to someone who has never seen the code before. A reader should be able to open any file in `build/`
and, from the comments alone, know what problem the file solves, what it owns, how it fits into the pipeline, and which file to read next to keep following
the flow.

This document is not implementation work — it's documentation work. Adds prose, doesn't touch behavior.

---

## 1. Goal

Three things a reader should walk away with after reading any file's header comment:

1. **Why it exists.** The problem it solves, stated in concrete terms. Not "this file contains the Target class" — that's obvious from the file's contents.
   Something like "every node in the build graph is a `Target`; this file defines the language-agnostic identity carried by every node."
2. **What it owns.** The types it exports, the invariants those types maintain, the API surface that callers reach for.
3. **Where this fits.** One paragraph orienting the file in the overall pipeline. Who constructs it. Who consumes it. What state it carries between
   construction and consumption.
4. **What to read next.** Two or three pointers to the files most likely to be opened next when chasing the flow.

After reading the headers across all files in order of the pipeline (bootstrap → framework → ir/emit → ir/writer → run/main → run/execute → scheduler →
edge execution → log writeback), the reader should be able to narrate the entire build process end to end without opening any function bodies.

---

## 2. Documentation style

**Prose, not annotations.** A 10–25 line comment at the top of each file in natural English, matching the voice of `build_system.md`. No
"@brief"/"@param"/"@returns" tags, no per-method docstrings, no rigid templates. The header is a paragraph or three of orientation, optionally followed by
a list of cross-references.

Rough shape (not a template — adapt per file):

```cpp
// <one-line headline, e.g. "build::Target — the node type for every build edge">
//
// <one paragraph: the problem this file solves and why a separate file exists for it>
//
// <one paragraph: what the file exports — the types and their job; key invariants>
//
// <one paragraph: where this fits in the pipeline — who constructs it, who reads it,
//  when it lives, when it dies>
//
// See also:
//   - <pathA>: <one phrase about why you'd open this next>
//   - <pathB>: ...
```

In-code comments stay sparse. CLAUDE.md's rule still holds — only write a comment when the WHY is non-obvious. The file header documents the file. Inline
comments document surprising lines.

**Length budget.** Aim for 10–25 lines per file header; up to 40 for the central files (`emit.hpp`, `execute.hpp`, `scheduler.hpp`). If a header would
exceed 40 lines, that probably means the file is doing too much; flag it for follow-up rather than padding.

---

## 3. What NOT to document

- **Method-by-method docstrings.** If `add_edge(Edge)` is named well, no doc needed. The class header explains the class; method names explain the methods.
- **WHAT the code does.** "This loop iterates over `edges` and prints each one" repeats the code. Don't.
- **History / changelog.** Git carries that. Don't write "added in plan_custom_build_backend phase 3" — it rots.
- **Implementation curiosities that don't bear on use.** "Uses `std::unordered_map` because it's faster than `std::map` here" is a perf note, fine in
  context; but mostly let the code speak.
- **Cross-references to files that don't add information.** "See also: `path.hpp` for the `Path` type" is noise. Only cross-ref when the next file is part
  of the *flow* a reader is likely chasing.

---

## 4. Per-file scope

What each file's header should specifically cover. This is not the exact wording — it's the angle the comment should take. I'll write the actual prose
during execution.

### 4.1 Top-level (1 file)

- **`build/bootstrap.cpp`** — The user-facing entry point. Explain the orchestration: self-build via `ngen::run::execute()`, then graph subprocess, then
  runner subprocess. Explain the `self_build_ir()` two-edge construction (graph + runner). Note the seed compile that produces this binary in the first
  place is documented in `CLAUDE.md`.

`build/build.cpp` is intentionally excluded — see §8.

### 4.2 Framework core (12 files)

- **`framework/path.hpp`** — Why a wrapper exists around `std::filesystem::path` (consistent `generic_string()` use, `operator/`, comparison). Short header.
- **`framework/command.hpp`** — `Command` is an argv list (vector of strings). Emit code builds these via `cxx::cmd::*` helpers; later they get baked into
  IR via `bake_command` in `ir/emit.hpp`. Short header.
- **`framework/variant.hpp`** — `BuildVariant` is `(platform, config, out_dir)` — the identity of "which build is this." Constructed in the emitter and
  passed everywhere as a read-only descriptor. Short header.
- **`framework/glob.hpp`** — Globbing + a few free utilities (`shell_quote`, `capture_tokens`, `write_if_changed`, `repo_root`, `split_ws`) and the
  framework-wide `Error` type. Explain why `std::regex` is avoided (exceptions are forbidden) and that `Error` lives here because every other framework
  header includes glob.hpp transitively.
- **`framework/extensionmap.hpp`** — Type-erased extension map keyed by `std::type_index`. Explain the two attachment modes (`add` owns + idempotent;
  `attach` is non-owning replace) and why nullable `get` returns `nullptr` instead of throwing.
- **`framework/target.hpp`** — `build::Target` is the node. Identity + deps + variant gating + `ExtensionMap`. Language-agnostic. Carries no language
  vocabulary; that lives in extensions. Point at `cxx/target.hpp`, `tool.hpp`, `alias.hpp` as the things that attach to it.
- **`framework/project.hpp`** — `Project` is the registry of entry targets, platforms, and configurations. Explain `build_all()` returns post-order, which
  is what the emitter needs. Note the project does not own its targets — those live in user code (typically `main()` locals in `build.cpp`).
- **`framework/platform.hpp`** — `build::Platform` is the language-agnostic platform identity (name, os, graphics_api, exe_suffix) + an `ExtensionMap`.
  Cross-ref to `cxx/platform.hpp` for the language-specific wrapper.
- **`framework/configuration.hpp`** — Same shape as `Platform`: identity (name, out_dir) + `ExtensionMap`. Cross-ref to `cxx/configuration.hpp`.
- **`framework/alias.hpp`** — `Alias` is a wrapper attached to a `Target` as an extension. Resolves to another target based on `(platform, config)`
  selectors. Used so a graph node like `rhi-backend` can mean different things on different platforms. Note that the emitter walks alias chains via
  `resolve_alias` in `ir/emit.hpp`.
- **`framework/tool.hpp`** — `Tool` is a wrapper attached to a `Target` as an extension. Runs an opaque shell command (`glslc`, `rm`, `clang-format`).
  Explain the `is_global` flag (one edge across all variants vs per-variant), the `for_each` form (one output per input), and the `$in`/`$out`/`$out_dir`
  substitution. Cross-ref to `ir/emit.hpp::emit_tool` / `emit_global_tool`.
- **`framework/inspect.hpp`** — `list_roots` helper used by `--list`. Tiny file. Brief header.

### 4.3 Framework cxx language module (6 files)

- **`framework/cxx/toolchain.hpp`** — Just the tools: `compiler`, `archiver`, `linker`, `default_std`. Composed inside `cxx::Platform`, not its own
  extension on `build::Platform`.
- **`framework/cxx/platform.hpp`** — Per-platform compile flags / link flags / defines / system libs + a `Toolchain`. Attached to `build::Platform` via
  `ExtensionMap`. Wrapper invariant: re-attaches on move/copy.
- **`framework/cxx/configuration.hpp`** — Per-config compile flags / link flags / defines. Attached to `build::Configuration`. Same wrapper invariant.
- **`framework/cxx/target.hpp`** — `cxx::Target` is the user-facing surface for libraries and programs. Wraps a `build::Target`. Explain the `Kind` enum
  (static_library / shared_library / program), the `sources(...)` call that materializes `cxx::ObjectFile` children, the include/link/flag fluent API, and
  the `OptLevel` sugar.
- **`framework/cxx/objectfile.hpp`** — One node per translation unit. Held behind `std::shared_ptr` because it's referenced from both the parent `cxx::Target`
  and as a `build::Target` dep. Non-copyable, non-movable. Per-TU compile flags / defines / std / warning suppressions. Explain why ObjectFile names are
  composite (`<parent>/<source>`).
- **`framework/cxx/commands.hpp`** — The argv builders. `compile_command` / `archive_command` / `link_command` produce `Command` values that the emitter
  bakes into IR edge command strings. Pure data, no I/O.

### 4.4 IR transport (5 files; xxhash excluded)

- **`ir/schema.hpp`** — The in-memory IR types (`Edge`, `Pool`, `IR`) plus the wire-format constants (`kHeaderSize`, `kEdgeRecordSize`, etc.) and pool
  indices (`kPoolDefault`, `kPoolConsole`). Explain the field layout so a reader can match it to writer/reader byte offsets.
- **`ir/writer.hpp`** — Serializes IR to a binary file. Walks the IR twice (intern strings, then layout records). Explain the string-table-at-end layout
  and the `serialize(IR&) -> string` core.
- **`ir/reader.hpp`** — The inverse of writer. Validates magic and version, deserializes records. Will eventually mmap for zero-copy reads; currently does
  a full file read. Returns `std::expected<IR, Error>`.
- **`ir/json.hpp`** — JSON dump for `--dump-graph`. Not a parse target; strictly for human inspection. Note this is the only path that turns IR back into
  text.
- **`ir/emit.hpp`** — The big one. `ir::Emitter` walks a `Project` and produces one `IR` per `(platform, config)` variant. Explain the traversal order
  (post-order via `Project::build_all`), the dispatch by extension type (`Tool` / `cxx::ObjectFile` / `cxx::Target`), the helpers (`resolve_alias`,
  `collect_includes`, `object_path`), and the command-baking step that turns a `Command` argv list into an `Edge.command` string. Probably the longest
  header of any file.

### 4.5 Runner (8 files)

- **`run/main.cpp`** — Thin CLI wrapper. Parses args, calls `read()` then `execute()`. Useful as the "what does this binary do" entry point.
- **`run/execute.hpp`** — The runner's top-level entry. Explain the lifecycle: load IR + load build log → resolve targets + reachability walk → local dirty
  check per edge → propagate dirty along DAG → build Plan for scheduler → invoke scheduler → update log on each completion → atomic log save. This is the
  second-longest header.
- **`run/scheduler.hpp`** — The `-j N` worker pool. Explain: ready queue, pending-count decrement, console pool serialization, dead-branch poisoning on
  failure, SIGINT cancellation. Explain why the scheduler is a separate class from `execute.hpp` (testability, isolation of concurrency code).
- **`run/process.hpp`** — Subprocess primitives. POSIX-only. Explain `Process::spawn` (returns a handle with pid + read pipe), `wait` (drains pipe + reaps
  pid), `signal` (forwards SIGINT). Note the convenience `run()` wrapper that does spawn + wait synchronously.
- **`run/hash.hpp`** — xxh3-64 file hashing + mtime fast-path. Explain `StatTuple`, `stat_matches`, `hash_file`, `hash_string`. Reference the buildlog
  fast-path logic that uses these.
- **`run/buildlog.hpp`** — Persistent edge state. Single binary file per variant at `_out/<plat>/<cfg>/.ngen-buildlog`. Explain the `TrackedFile`
  `(path, stat, content_hash)` triple, the `LogEntry` fields (inputs/outputs/discovered_headers/command_hash/last_run_ns), atomic-rewrite-on-save
  semantics, and the `refresh()` helper that pairs with the mtime fast-path.
- **`run/depfile.hpp`** — Parse Make-format `.d` files (the output of `-MMD -MF $out.d`). Explain the supported syntax (line continuations, `\space`
  escapes, `#` comments) and why we don't reuse a make implementation (we'd inherit too much).
- **`run/progress.hpp`** — Display logic. Explain the three verbosity modes (`Default`, `NonTty`, `FullCommand`), the `\r`-overwrite single-line mode on a
  tty, `NO_COLOR` handling, and the failure-block format.

---

## 5. Flow tracing

A reader following the flow should be able to walk the docs in this order without ever feeling lost:

1. `bootstrap.cpp` → "OK, this is the entry point; it builds graph + runner via execute(), then shells out to them."
2. `run/execute.hpp` (from the self-build path) → "execute takes an IR and runs it."
3. `run/scheduler.hpp` → "scheduler runs edges in parallel respecting the DAG."
4. `run/process.hpp` → "each edge becomes a subprocess."
5. (Back to bootstrap.cpp) → "now invoke ngen-build-graph."
6. `build.cpp` → "the graph stage describes the project using framework/ types."
7. `framework/project.hpp` → "this holds the registry."
8. `framework/target.hpp` → "every node is a Target with extensions."
9. `framework/cxx/target.hpp` → "library/program-shaped targets come from this extension."
10. `framework/cxx/objectfile.hpp` → "and each `.cpp` is its own node via this extension."
11. `ir/emit.hpp` → "this is what turns the Project into an IR."
12. `ir/schema.hpp` → "and these are the types it produces."
13. `ir/writer.hpp` → "which get serialized to disk."
14. (Back to bootstrap.cpp) → "then ngen-build-run subprocess runs the IR."
15. `run/main.cpp` → "loads the IR..."
16. `ir/reader.hpp` → "...via this..."
17. (Back to execute.hpp) → "and the lifecycle continues with dirty detection and log writeback."
18. `run/buildlog.hpp` + `run/hash.hpp` + `run/depfile.hpp` → "these are the bricks of dirty detection."

The "See also" cross-references on each file should support this walk. They don't need to be exhaustive — pick the next two or three files a reader is most
likely to want.

---

## 6. Phases

Five phases, each a self-contained PR. Order is bottom-up by responsibility — start with the leaf utilities, then the configuration API, then the
transport, then the runner, then the entry points. This way every cross-reference points at a file whose header has already been written.

### Phase 1 — Framework core (12 files)

`path.hpp`, `command.hpp`, `variant.hpp`, `glob.hpp`, `extensionmap.hpp`, `target.hpp`, `project.hpp`, `platform.hpp`, `configuration.hpp`, `alias.hpp`,
`tool.hpp`, `inspect.hpp`.

Most are 10–15 line headers. `extensionmap.hpp`, `target.hpp`, `tool.hpp` get a bit more — 20–25 lines.

### Phase 2 — Framework cxx language module (6 files)

`toolchain.hpp`, `platform.hpp`, `configuration.hpp`, `target.hpp`, `objectfile.hpp`, `commands.hpp`.

Most files here can defer to "the framework core file with the same name" for the wrapper pattern, then explain the cxx-specific surface. 15–25 lines each.

### Phase 3 — IR transport (5 files)

`schema.hpp`, `writer.hpp`, `reader.hpp`, `json.hpp`, `emit.hpp`.

`emit.hpp` is the heavy one (30–40 lines) because it's the central translation step. The others are shorter — they're all serialization mechanics.

### Phase 4 — Runner (8 files)

`main.cpp`, `execute.hpp`, `scheduler.hpp`, `process.hpp`, `hash.hpp`, `buildlog.hpp`, `depfile.hpp`, `progress.hpp`.

`execute.hpp` and `scheduler.hpp` are the heavy ones (30–40 lines each). `hash.hpp` and `depfile.hpp` are tiny.

### Phase 5 — Top-level (1 file)

`bootstrap.cpp`. The entry point benefits most from a longer narrative header (25–40 lines) since a new reader is most likely to open this file first.
`build.cpp` is excluded (see §8).

---

## 7. Quality bar — review checklist

Before each phase merges, walk through these:

- [ ] Header opens with a one-line headline that names the file's primary export, not the file's filename.
- [ ] The "why this exists" paragraph answers in concrete terms; it does not say "this file contains X" (the file's contents say that).
- [ ] No method-by-method commentary, no `@param`, no per-line obvious-WHAT comments.
- [ ] The "See also" list has at most three entries, each with a phrase explaining *why* a reader would open that file next.
- [ ] No history, no dates, no plan-doc references that will rot.
- [ ] Reading the headers of an adjacent set of files in pipeline order tells a coherent story; no dead-end pointers, no doubling back.
- [ ] If a file would need a 50-line header to fairly explain it, treat that as a smell — flag for a future split, don't pad.

---

## 8. Out of scope

- Any behavior change. This plan is comments only.
- Renaming files or namespaces. The previous reorgs settled the names; this plan documents what's there.
- Editing `build_system.md`, `CLAUDE.md`, or `docs/*.md`. Those are already documented at their own level; the per-file comments are a different layer.
- `build/ir/xxhash.h`. Vendored upstream; not our prose to write.
- `build/build.cpp`. This is the project's own graph description — a per-project artifact, not part of the build system itself. A header comment here
  would either restate what the user already wrote (the `Project p; p.target(view);` shape is self-describing in fluent C++) or document the *project*
  rather than the *build system*, and that's the wrong layer. If a new contributor needs orientation on how this file is structured, the answer is
  `build_system.md` plus reading the fluent API.

---

## 9. What "done" looks like

After all five phases:

- Every file in `build/` (excluding `xxhash.h` and `build.cpp` — see §8) opens with a prose header that orients a cold reader.
- The "See also" trails knit into a tour that follows the actual flow of a build.
- Inline comments remain rare, surgical, and only present where the WHY is non-obvious.
- `build_system.md` and the per-file headers do not duplicate each other — `build_system.md` is the architecture overview, the file headers are the
  embedded "you are here" pins. A reader who only has the file open can orient; a reader who wants the bird's-eye view goes to `build_system.md`.
