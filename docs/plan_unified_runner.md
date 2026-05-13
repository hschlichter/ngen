# Plan: Unified Runner — One Execution Engine for Everything

**Status.** Follow-up to `plan_remove_ninja_from_bootstrap.md`. Do not start until that plan has landed and the stat-and-compile loop inside `ngen-build` has
been in use long enough to be boring. Builds on `plan_custom_build_backend.md` (the project runner).

This plan collapses the *two* execution engines that exist after the previous two plans (the project runner for the user's graph, and the stat-and-compile
loop for the build-system itself) into *one*: the project runner, used for both domains. `ngen-build` constructs an in-memory IR describing how to compile
its own dependencies (`ngen-build-graph`, and the runner itself if its sources changed), hands that IR to `ngen-build-run`, and gets back a built tree.

This is the end-state architecture. After this lands, there is exactly one scheduler, one build log format, one set of dirty-detection semantics, and one
process-launching abstraction across the entire build system.

---

## 1. Why this is a separate plan

The unified-runner shape is the right target. We arrive there in three deliberate steps rather than one because:

- **The runner has to be trustworthy first.** It executes the project graph in `plan_custom_build_backend.md`. By the time this plan starts, the runner has
  been the default backend for a real release cycle and has accumulated soak time.
- **`bootstrap.sh` will compile the runner before anything else can use it.** Adding one more line to that script is trivial, but the chicken-and-egg story
  needs to be designed, not improvised.
- **A clean before/after.** Plan C removed ninja. This plan removes the stat-and-compile loop. Each step has a single, reviewable diff.

---

## 2. End state

```text
bootstrap.sh
  → _out/ngen-build              (compiled from build/bootstrap.cpp)
  → _out/ngen-build-run          (compiled from build/run/*.cpp)            ← NEW: also built by the seed

ngen-build invocation
  → build a small in-memory IR describing the build-system's own graph:
      • edge: compile build/build.cpp     → _out/ngen-build-graph
      • edge: compile build/run/*.cpp     → _out/ngen-build-run
  → hand that IR to _out/ngen-build-run (no IR file written to disk by default)
  → ngen-build-run schedules + executes those edges; uses the same buildlog/hashing/depfile machinery as the project runner
  → _out/<plat>/<cfg>/build.ngenir    (emitted by the now-fresh ngen-build-graph)
  → _out/ngen-build-run executes the project IR
```

`bootstrap.sh` compiles two binaries: `ngen-build` and `ngen-build-run`. From that point on, everything else flows through them.

The runner executes two kinds of work, distinguished only by which IR was handed to it:

- **Build-system IR.** Constructed in memory by `ngen-build` on every invocation. Tiny — two or three edges. Never written to disk.
- **Project IR.** Read from `_out/<plat>/<cfg>/build.ngenir` as today.

Both go through the same scheduler, same `Process::spawn`, same `_out/.ngen-buildlog` (with separate keyspaces, see §4.3).

---

## 3. The runner API after this plan

Today (post-plan-A) the runner is a binary you invoke as a process: `ngen-build-run --ir <path> ...`. This plan adds one capability and otherwise leaves the
binary unchanged: the runner exposes a small C++ entry point that `ngen-build` can call directly without going through the CLI.

```cpp
namespace ngen::run {

struct RunOptions {
    int  jobs            = std::thread::hardware_concurrency();
    int  keep_going      = 1;
    bool verbose         = false;
    bool very_verbose    = false;
    Path buildlog_path;                    // override; default _out/.ngen-buildlog
    std::vector<std::string> targets;      // empty = all edges in IR
};

std::expected<int, Error> execute(const ir::IR& ir, const RunOptions& opts);

}
```

`execute()` is implemented in the same translation units that today implement the runner's CLI. The CLI's `main` becomes a thin wrapper: parse args, load IR
from disk, call `execute()`. Nothing else changes for the project-build path.

`ngen-build` includes the runner's headers and calls `ngen::run::execute()` directly with an in-memory IR. No file gets written, no second process gets
spawned. The runner is a library and a binary; this plan exposes the library face.

This requires `bootstrap.sh` to link the runner's TUs into both binaries (`ngen-build` and `ngen-build-run`), or to build a small static archive in between.
See §5.

---

## 4. The build-system IR

### 4.1 Construction

`ngen-build` constructs the IR in C++ on every invocation. Hand-built, not loaded from a file:

```cpp
ir::IR self_build_ir() {
    ir::Builder b;

    auto graph_edge = b.edge()
        .name("ngen-build-graph")
        .command(std::format("{} -std=c++23 -O2 -MMD -MF $out.d -Ibuild/framework "
                             "build/build.cpp -o _out/ngen-build-graph", cxx_compiler()))
        .input("build/build.cpp")
        .output("_out/ngen-build-graph")
        .depfile("_out/ngen-build-graph.d")
        .description("CXX ngen-build-graph");

    auto runner_edge = b.edge()
        .name("ngen-build-run")
        .command(/* ... clang++ build/run/*.cpp ... */)
        .inputs(glob("build/run/*.cpp"))
        .output("_out/ngen-build-run")
        .depfile("_out/ngen-build-run.d")
        .description("CXX ngen-build-run");

    return b.finalize();
}
```

This is structurally identical to how `build/build.cpp` builds the project IR — same `ir::Builder` API, same edge fields. The build-system graph is just a
much smaller user of the same machinery.

`Builder` lives in `build/framework/ir/` (added in plan A; this plan may extend it slightly to make in-memory construction ergonomic). It already supports
fully-baked commands per `plan_custom_build_backend.md` §4.

### 4.2 No file emission

The in-memory IR is passed directly to `ngen::run::execute()`. No `build.ngenir` file is written for the self-build path. The IR exists for the duration of
the `execute()` call.

Optional debugging affordance: `ngen-build --dump-self-build-ir <path>` writes the build-system IR to disk in the same binary format used for project IRs, so
it can be inspected with the same tools (`--dump-graph` JSON, etc.). Not on the hot path.

### 4.3 Build log keyspace

Both the build-system IR and the project IR use `_out/.ngen-buildlog` for persistent state. To keep keyspaces from colliding, edge names are namespaced:

- Build-system edges: `system/ngen-build-graph`, `system/ngen-build-run`.
- Project edges: unchanged (`obs`, `renderer`, `renderer/src/renderer/foo.cpp`, etc.).

Practically: the IR builder for the self-build prepends `system/` to every edge name. The runner doesn't know or care; it just sees keys. The buildlog naturally
holds both sets of entries because they never collide.

If we ever want isolated logs, the `RunOptions::buildlog_path` lets the caller point each invocation at a different file. For now a single shared log is
simpler and the keyspaces are tiny.

---

## 5. Bootstrap changes

`bootstrap.sh` grows from three lines to roughly five:

```sh
#!/usr/bin/env sh
set -eu
mkdir -p _out
CXX="${CXX:-c++}"
# Compile the runner's TUs once into a static archive, then link both binaries against it.
$CXX -std=c++23 -O2 -c build/run/process.cpp build/run/buildlog.cpp build/run/hash.cpp \
     build/run/scheduler.cpp build/run/depfile.cpp build/run/progress.cpp \
     -Ibuild/framework
ar rcs _out/libngenrun.a process.o buildlog.o hash.o scheduler.o depfile.o progress.o
rm -f *.o
$CXX -std=c++23 -O2 -o _out/ngen-build     build/bootstrap.cpp  -Ibuild/framework _out/libngenrun.a
$CXX -std=c++23 -O2 -o _out/ngen-build-run build/run/main.cpp   -Ibuild/framework _out/libngenrun.a
```

Five lines is still small enough to read at a glance. Once the runner is up, all subsequent rebuilds of these binaries go through the runner itself, so the
shell script only runs on initial clone.

Alternative considered: build `ngen-build-run` first, then have it bootstrap `ngen-build` via its CLI. Rejected — it requires writing a build-system IR to
disk before `ngen-build` exists to construct it. Two `cc` lines in the seed script is much simpler.

---

## 6. What gets deleted

After this plan lands:

- The stat-and-compile loop in `bootstrap.cpp` (added in `plan_remove_ninja_from_bootstrap.md`) — replaced by `self_build_ir()` + `ngen::run::execute()`.
- `bootstrap.cpp`'s direct `Process::run` invocations for compiling graph/runner — replaced by runner edges.
- Any "is it stale?" heuristics inside `bootstrap.cpp` — the runner's existing hash+mtime dirty rule applies uniformly.

What stays:

- The thin orchestration logic in `bootstrap.cpp` for CLI parsing, special-target routing (`clean`/`format`/`tidy`), and selecting variants. None of that
  belongs in the runner.

The `ngen-build` binary is now genuinely thin: parse args, decide what work to do, build small IRs, hand them to the runner. The runner does everything else.

---

## 7. Phased rollout

Two phases.

### Phase 1 — Expose `ngen::run::execute()` and refactor the runner CLI

- Promote the runner's main-line logic into `ngen::run::execute(const IR&, const RunOptions&)`.
- The CLI `main()` in `build/run/main.cpp` becomes a small wrapper that parses args, loads IR from disk, calls `execute()`.
- Update `bootstrap.sh` to produce `_out/libngenrun.a` and link both binaries against it.
- No behavior change visible. The runner does exactly what it did before; we've just lifted its entry point.
- Ship: nothing user-visible. CI confirms `ngen-build-run` behaves identically.

### Phase 2 — Use the runner for the build-system self-build

- Add `self_build_ir()` in `bootstrap.cpp`.
- Replace the stat-and-compile loop with `ngen::run::execute(self_build_ir(), {...})`.
- Verify on a clean checkout that `bootstrap.sh && ./_out/ngen-build` builds everything and produces a working `ngen-view`.
- Verify that touching `build/build.cpp` rebuilds only `ngen-build-graph`, and touching `build/run/scheduler.cpp` rebuilds only `ngen-build-run`.
- Update `build_system.md` — replace the "stat-and-compile" section from the previous plan with "runner is the single execution engine."
- Update `CLAUDE.md` if anything in the bootstrap story changed.

After phase 2, the build system has one execution engine, one CLI affordance for it, and one source of truth for what's stale.

---

## 8. Risks and mitigations

- **A runner bug now breaks the build system itself, not just the project build.** This is the main reason for the staging: the runner gets a full release
  cycle as the project executor before we put it on the self-build critical path. Mitigation: keep `bootstrap.sh` self-contained — if `ngen-build` won't
  start, running the three lines manually still gets you a `_out/ngen-build` binary, which contains enough error reporting to diagnose what the runner
  refused to do.
- **In-memory IR construction has to stay simple.** Twenty lines of `ir::Builder` calls in `bootstrap.cpp`. If `self_build_ir()` starts growing flags,
  defines, or platform conditionals, that's a smell — pull the structure into a helper but keep the IR shape obvious. Two edges is two edges; if we add more
  binaries to the build system, it's still small.
- **`Builder` API gap.** `build/build.cpp` uses the framework's full cxx wrapper stack to produce IR via `backendir.hpp`. The self-build doesn't want all
  that machinery — no `cxx::Platform`, no `cxx::Configuration`, no extension map dispatch. It just wants to add two raw edges. So `ir::Builder` needs a
  low-level "add an edge with these exact strings" API exposed separately from the `backendir.hpp` traversal. Confirm this exists in plan-A's `Builder`
  before starting; add it in phase 1 if not.
- **Buildlog keyspace collisions.** `system/` prefix is enforced by the self-build IR builder and asserted-not-present in user-defined target names by the
  project IR builder. Single assertion, single line of code.

---

## 9. Open questions

- **Should `ngen::run::execute()` accept an in-memory IR or always go through a file?** Library form (in-memory) saves the serialization round-trip and the
  spawn of a second process for tiny build-system IRs. File form is more uniform with how the CLI runs. The plan above goes with library form for the
  self-build path, file form for the project path. The CLI `main` is essentially "load from file, call library." We could make the library always accept a
  value type and have the CLI deserialize — clean. Confirm during phase 1.
- **Caching the build-system IR.** It's reconstructed on every `ngen-build` invocation. For our two-edge graph this is microseconds; not worth caching. If
  the build-system grows to many internal binaries, revisit.
- **Variants for the build-system itself?** Today we don't build the build-system in debug vs release configurations — there's no platform/config axis on
  it. If we ever want one (e.g. ASan'd `ngen-build-run` for debugging), the IR shape supports it trivially; we just don't expose it as a CLI option yet.
- **`bootstrap.sh` portability.** Still Linux-shell. When Windows lands (out of scope for this plan and the chain it sits in), it gets a `bootstrap.bat` or
  similar. The four `cc` invocations have direct PowerShell/cmd equivalents.

---

## 10. Why this is worth doing

Two arguments:

1. **One bug surface.** After this plan, "is this edge stale?" has exactly one answer everywhere in the system. The build-system gains content hashing and
   precise depfile tracking for free, because it now goes through the same code path the project does. Without this plan, the build-system uses a coarser
   mtime-only check that can miss e.g. a `__has_include`-style header change that doesn't bump the depender's mtime.
2. **Pressure on the runner.** Every `ngen-build` invocation exercises the runner against a tiny graph before touching the user's graph. Bugs and
   regressions in the runner that would have hidden in rare project-build scenarios get caught immediately because the runner runs *first thing*, every time.
   This is much more valuable than it sounds.

The end-state is also philosophically cleaner: the build system is a graph executor with a small in-memory bootstrap graph and an arbitrarily large
on-disk project graph. There is no second execution engine and no second mental model. That's what we wanted from the beginning.
