# Plan: Remove Ninja From the Build-System Self-Bootstrap

**Status. Superseded — not implemented.** After `plan_custom_build_backend.md` phase 5 landed, the runner had hardened enough that the risk-staging argument
for doing this plan as a stepping stone collapsed. We went straight to `plan_unified_runner.md` instead, which replaces ninja with the runner library
directly. This document is kept as a record of the path-not-taken; the analysis of what `Process::run` + depfile + buildlog primitives are needed remained
correct, the choice to insert a separate stat-and-compile loop did not.

This plan removes ninja from the build-system's self-bootstrap chain. After it lands, the repo contains zero ninja files (no `bootstrap.ninja`, no generated
`.ninja` manifests). The user-facing binary `ngen-build` orchestrates its own dependencies — it compiles `ngen-build-graph` and `ngen-build-run` on demand
when their sources change, then runs them.

The project runner (`ngen-build-run`) is unchanged by this plan. It already executes the project graph. This plan only touches stages 1-2 and the manifests
that drove stages 3-4.

---

## 1. Why this is a separate plan

The main plan deliberately scoped out the build-system's self-bootstrap to keep risk on the critical path (the project runner) low. Once the runner is shipped
and stable, removing ninja from self-bootstrap is a small, contained change:

- The primitives needed (`Process::run`, depfile parser, file hashing with the mtime fast-path) all already exist in `build/run/`.
- The bootstrap chain has only four binaries to manage, with simple dependency relationships.
- Failure mode is isolated: if it breaks, `bootstrap.sh` plus a manual `clang++` invocation always gets you back.

Doing it in a separate pass also gives us measurements. After the main plan lands we'll know whether the current chain (`bootstrap.ninja` plus prebuild plus
two more ninja runs) is actually noticeable on no-op rebuilds. The answer informs how aggressive we want to be here.

---

## 2. End state

Bootstrap chain after this lands:

```text
one-time bootstrap (documented `c++` invocation, run by hand on a fresh clone)
  $ c++ -std=c++23 -O2 -o _out/ngen-build build/bootstrap.cpp
  → produces _out/ngen-build

every subsequent invocation:
ngen-build
  → stat-and-compile loop inside ngen-build:
      → recompile _out/ngen-build-graph if any source/header changed
      → recompile _out/ngen-build-run     if any source/header changed
  → _out/<plat>/<cfg>/build.ngenir          (emitted by ngen-build-graph)
  → ngen-build-run executes the IR
```

What changes:

- `build/bootstrap.ninja` — deleted.
- `build/prebuild.cpp` and `_out/ngen-build-pre` — deleted. Their job (emit ninja manifests for stages 3-4) no longer exists. Header dependency tracking moves
  to direct `-MMD` consumption inside `ngen-build`'s stat-and-compile loop.
- `_out/ngen-build-graph.ninja`, `_out/ngen-build-run.ninja` — no longer emitted; nothing reads them.
- `build/bootstrap.cpp` — gains an internal "build the build-system if stale" pass that runs before any project work. Uses `Process::run` and the depfile
  parser from `build/run/`.
- Bootstrap is no longer a checked-in file. It becomes a one-line `c++` invocation that contributors run once on a fresh clone, documented in `CLAUDE.md`'s
  build section. No `bootstrap.sh`, no `Makefile`, no wrapper — the command is short enough to print, paste, and read.

What stays the same:

- `build/build.cpp` (graph stage) — unchanged.
- `build/run/*` (runner) — unchanged.
- `framework/` — unchanged.
- The project-level user experience (`./_out/ngen-build`, `--config release`, `clean` / `format` / `tidy`) — unchanged.

The four-stage mental model collapses to three: bootstrap (one shell line), orchestrate-and-self-build (`ngen-build`), execute (`ngen-build-run`). The graph
stage remains a separate process because it still emits the project IR.

---

## 3. The new bootstrap step

Bootstrap becomes a single documented `c++` invocation — not a checked-in script:

```sh
mkdir -p _out && c++ -std=c++23 -O2 -o _out/ngen-build build/bootstrap.cpp
```

Run once after a fresh clone, never again unless `bootstrap.cpp` itself changes. The command lives in `CLAUDE.md`'s build section as the documented entry
point. Everything else flows through `./_out/ngen-build`.

Rationale for "documented command, not script":

- The invocation is short enough to print, paste, and read. There is no `bootstrap.sh` to maintain, no `set -eu` wrapping, no shell-portability layer.
- Anyone copying the command knows exactly what's happening — no indirection through a file.
- We're Linux-only by `plan_custom_build_backend.md` §1, so we don't need cross-platform script handling.
- One fewer file in the repo. The build system has consciously avoided checked-in tooling so far (bootstrap.ninja being the sole exception); this preserves
  that discipline.

---

## 4. The stat-and-compile loop inside `ngen-build`

The orchestrator's startup gains a "make sure the build-system binaries are current" pass before any project work. Pseudocode:

```cpp
// build-system self-build graph, hand-built (small and stable).
struct BinarySpec {
    Path output;             // _out/ngen-build-graph
    std::vector<Path> sources;   // build/build.cpp
    std::vector<std::string> flags;  // -std=c++23 -O2 -MMD -MF $out.d -Ibuild/framework
};

for (auto& spec : self_build_specs()) {
    if (is_stale(spec)) {                  // see §4.1
        compile(spec);                     // shell out via Process::run
    }
}
```

`self_build_specs()` returns two entries: graph and runner. Each spec is a handful of lines of plain C++ — sources, output path, flags. Updates land here when
we add a framework header that needs special flags or split the runner into more TUs. Both rare events.

### 4.1 Staleness check

For each spec, the output binary is stale if any of:

1. The binary doesn't exist.
2. Any listed source file is newer (mtime) than the binary.
3. Any header listed in the binary's `.d` file is newer than the binary.

This is intentionally simpler than the project runner's dirty rule. The build-system has four .cpp files and a couple dozen headers; xxhashing them all would
be measurable noise relative to compiling them. mtime-only is correct enough for code we control.

If the `.d` file is missing, the binary is considered stale (forces a full recompile, which regenerates the depfile).

### 4.2 Compile invocation

Shell out via `Process::run`:

```cpp
std::string cmd = compose_command(spec);  // "c++ -std=c++23 ... build/build.cpp -o _out/ngen-build-graph"
std::string output;
auto rc = ngen::run::Process::run(cmd, output);
if (!rc || *rc != 0) {
    std::cerr << output;
    return Error::SelfBuildFailed;
}
```

Output is suppressed on success, printed on failure. Same convention as the project runner uses for non-console-pool edges.

### 4.3 Concurrency

Graph and runner have no dependency between them. The stat-and-compile pass can compile them in parallel using two `Process::spawn` calls and a `wait_all`. At
two binaries the win is one-shot; not worth tuning further. If we ever split the runner into many TUs, revisit (and probably reach for plan D anyway).

---

## 5. Reuse from the project runner

This plan adds no new primitives. Everything it needs already exists in `build/run/` after the main plan lands:

- `Process::run` / `Process::spawn` — `build/run/process.hpp`.
- Depfile parser — `build/run/depfile.hpp`. Used identically to how the runner consumes compile-edge depfiles.
- Path utilities — `build/framework/path.hpp` / `glob.hpp`. Header-only, already used by `bootstrap.cpp`.

Because the runner is header-only (see `plan_custom_build_backend.md` §3), `bootstrap.cpp` simply `#include`s the runner headers it needs and gets `inline`
definitions of `Process::run`, `parse_depfile`, etc. There is no link-time sharing to worry about; each binary's TU instantiates its own copies and the
linker drops duplicates. Compile-time cost is two TUs' worth — bounded and accepted.

---

## 6. Phased rollout

Two phases. Each is a self-contained PR.

### Phase 1 — Add the stat-and-compile loop alongside ninja

- Implement `self_build_specs()`, `is_stale()`, and the parallel compile pass in `bootstrap.cpp`.
- Gate it behind `--self-build=inline` (default: keep the existing ninja-based flow). Both paths work; we validate that the new path produces byte-identical
  binaries on a clean tree.
- Ship: `ngen-build --self-build=inline` runs end-to-end. Default is still ninja.

### Phase 2 — Cutover and delete

- Flip the default to inline. Smoke-test all configs.
- Delete `build/bootstrap.ninja`, `build/prebuild.cpp`. Remove `_out/ngen-build-pre` from the chain.
- Remove the `--self-build` flag (only one path now).
- Document the one-shot bootstrap `c++` invocation in `CLAUDE.md`'s build section. It replaces `ninja -f build/bootstrap.ninja` as the entry point for a
  fresh clone. No file is added to the repo for this — the command stays in docs.
- Update `build_system.md` — rewrite §3 (Bootstrap chain), trim §1 file list.

After phase 2, ninja is no longer required to build the engine. Contributors clone, run `./bootstrap.sh`, and have a working `ngen-build` with zero ninja
involvement from then on.

---

## 7. Risks and mitigations

- **First-run experience regresses if the documented bootstrap command is wrong.** It's one line; we test it on a clean container before merging and keep the
  command verbatim in `CLAUDE.md`.
- **Header dependency tracking subtly differs from ninja's.** Both use `-MMD`. Same compiler output, same parser semantics, so behavior matches. If we
  discover a corner case (multi-output edges, generated headers), it's the same corner case the project runner already handles.
- **Two compile paths for build-system vs project.** Plan D removes this duplication. Until then, it's a known wart and explicitly documented in
  `build_system.md` §13.
- **CI assumes `ninja` on PATH.** None right now, but worth a grep at merge time.

---

## 8. What lands in `build_system.md`

After phase 2, the following sections need rewrites:

- §1 (Files on disk) — remove `bootstrap.ninja` and `prebuild.cpp`; add `bootstrap.sh`.
- §3 (Bootstrap chain) — replace the four-stage diagram with the three-stage one from §2 above.
- §11 (CLI) — no user-facing change, but mention `./bootstrap.sh` as the seed.
- §13 (Things to know) — add: "`ngen-build` self-builds `ngen-build-graph` and `ngen-build-run` on every invocation, using mtime + depfile tracking. The
  stat-and-compile loop is intentionally simpler than the project runner's; we accept some over-compilation for code size and clarity."

---

## 9. Why not go straight to Plan D

Plan D replaces this plan's stat-and-compile loop with: hand-build an in-memory IR for the build-system, feed it to `ngen-build-run`. It's the right end
state — one execution engine across both domains.

Reasons to do C first:

1. **Risk staging.** C is local: a stat-and-compile loop is ~100 lines of C++ and never touches the runner. D requires the runner to execute a non-project IR
   on the *very first* `ngen-build` invocation after `bootstrap.sh`, which is the worst possible time for a runner bug. Doing C first means the runner has
   been hardened in production before D loads it onto the critical path.
2. **Decouples wins.** C delivers "ninja is gone" *immediately*. D delivers "everything goes through the runner" later, after we've lived with C and know
   whether the duplication actually hurts.
3. **Smaller diff to review.** C and D each touch about the same amount of surface, but in different places. Bundling them doubles review burden for no
   independence.

If after living with C the duplication never bothers anyone, we may still do D for uniformity — but at least we'll have made that call from data rather than
upfront.
