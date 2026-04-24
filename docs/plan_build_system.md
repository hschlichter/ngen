# Plan — A C++ Build Framework for ngen (and Beyond)

The build description for ngen is a C++ program. The *framework* those programs are written against is a reusable library — project-agnostic, portable, and
designed to move to other projects with minimal effort. ngen is the first consumer, not the sole owner.

This plan defines:

1. The conceptual model (graph of typed target nodes; toolchain-aware; pluggable backends).
2. The user-facing API — `Program`, `StaticLibrary`, `SharedLibrary`, `Tool` — with a fluent, composable surface.
3. The graph model underneath that API: what a composite node is, how subgraphs compose, how expansion to primitive steps works.
4. The toolchain abstraction (flag intent → compiler-specific argv), sized for Clang/Linux on day one and GCC/MSVC later.
5. Backends (Ninja first; in-process later) and how the same graph feeds both.
6. The three-stage bootstrap chain (`bootstrap.ninja → prebuild → ngen-build → ngen-view`) and the self-rebuild mechanisms that keep each stage
   current.
7. Debugging via an attached debugger — no parallel introspection surface.
8. How ngen's `build.cpp` looks expressed against this API.
9. Phased migration off the Makefile.

The existing `docs/build_concept.md` is one sketch of a minimal implementation. This plan does not commit to that sketch; it commits to the *concept* and sizes
the framework to support real growth (multiple binaries, shared/static libraries, codegen tools, more toolchains).

---

## 1. Goals and Constraints

**Framework is a product.** `build/framework/*` is project-agnostic. It must be extractable to its own repo or submodule by moving that one directory,
with zero engine knowledge leaking in. Project-specific code lives in exactly three files: `build/bootstrap.ninja`, `build/prebuild.cpp`, and `build.cpp`.

**Graph is the model.** Everything — compile, archive, link, tool invocation, shader compile — is a node. Dependencies are edges. Composite nodes
(`Program`, `StaticLibrary`, etc.) are subgraphs that expand into primitive rules at emit time. Composition across subgraphs is first-class: a `Program` that
links a `StaticLibrary` inherits its archive output as a dependency and (opt-in) its public include dirs.

**Fluent, composable API.** Reading a build program should feel like reading a declarative description, not a pile of tuples. Builders return `*this`; target
references compose via `.link(other)` / `.depend_on(other)`; intent verbs (`.define`, `.include`, `.warning_off`) are toolchain-agnostic.

**Toolchain-aware from day one.** Even though Phase 1 only ships Clang/Linux, the interface is designed so GCC, Clang/macOS, and MSVC are drop-in additions
later, not rewrites. Intent stays in the target; the toolchain object owns translation to flags and argv.

**Pluggable backends.** Ninja is Phase 1. An in-process backend (thread pool, spawning processes, mtime/hash incrementality) is Phase N. Both consume the
same expanded graph.

**Debuggable with a debugger.** The build program is a plain C++ binary — attach `gdb` / `lldb` to `./ngen-build` or `./prebuild` and step through graph
construction or expansion to see exactly what's happening. No parallel introspection surface to build or maintain.

**Three-stage self-hosting with unconditional chain execution.** Every `./ngen-build` invocation runs the full `bootstrap → prebuild → build` chain. Ninja
handles incrementality at every level, so "nothing changed" is fast; "something changed" is correct. No mtime heuristics in our own code (see §7).

---

## 2. Folder Layout

```
build/
  framework/              # portable — extracts to its own repo / submodule with no edits
    build.hpp             # umbrella header, users include just this
    path.hpp              # Path alias, tiny helpers
    command.hpp           # Command (argv), tokenisation helpers
    flags.hpp             # Flag intent types (Define, Include, Std, WarningOff, Raw, …)
    target.hpp            # Target abstract base
    program.hpp           / program.cpp
    staticlibrary.hpp     / staticlibrary.cpp
    sharedlibrary.hpp     / sharedlibrary.cpp
    tool.hpp              / tool.cpp
    graph.hpp             / graph.cpp          # Graph: owns targets, expansion, traversal
    toolchain.hpp                              # Toolchain interface
    toolchainclang.hpp    / toolchainclang.cpp # first concrete impl
    backend.hpp                                # Backend interface
    backendninja.hpp      / backendninja.cpp

  bootstrap.ninja         # project-local: compiles prebuild.cpp → ./prebuild
  prebuild.cpp            # project-local: build program that builds ./ngen-build
build.cpp                 # project-local: build program that builds ngen-view (repo root)
```

---

## 3. Graph Model

### 3.1 Targets as composite nodes

A `Target` is an abstract node in the graph. Its public handle has:

- A unique name within the graph.
- Zero or more *primary outputs* (usually one — `.a`, `.so`, exe, or a file produced by a tool).
- A set of *intents* (typed flag-like values: include dirs, defines, std version, warnings, link deps, …).
- A set of edges to other Targets (`deps`, `link_targets`).

Concrete Target subclasses:

| Type              | Primary output       | Expands to (primitive steps)                                   |
|-------------------|----------------------|----------------------------------------------------------------|
| `Program`         | executable           | N × compile(.cpp→.o) + 1 × link-exe                            |
| `StaticLibrary`   | `.a` archive         | N × compile + 1 × archive                                      |
| `SharedLibrary`   | `.so` / `.dylib`/DLL | N × compile + 1 × link-shared (+ import lib on Windows)        |
| `Tool`            | user-declared        | 1 × invoke-command (or N×M with `.for_each()`)                 |

Expansion is the step where the graph of composite Targets is lowered to a flat list of primitive `Rule`s (compile, archive, link-exe, link-shared,
invoke-tool) that the backend consumes. Expansion runs once per build. The Toolchain is the authority that turns intent + source paths into the final
`Command` (argv) for each primitive rule.

### 3.2 Subgraph composition

A `Program` that calls `.link(staticLib)` gets:

- An edge: `Program.link-exe-step` depends on `staticLib.archive-step.output`.
- A transitive walk: `staticLib`'s own `.link(otherLib)` deps are also pulled in (static linking composes transitively).
- An include-dir transfer: `staticLib`'s `.public_include(dir)` entries propagate into `Program`'s compile steps' include intent. Its `.include(dir)`
  (private) does not.
- Link-flag transfer for shared deps: a `Program` linking a `SharedLibrary` inherits rpath and `-l<name>` automatically.

This is the payoff of "everything is a graph": cross-target dependencies are typed and propagated by the framework. The user declares intent, not flags.

### 3.3 Primitive rule kinds

These are emitted by expansion and consumed by the backend. They're not the user-facing API but they are a stable contract between Target expansion and the
backend:

- `compile_cxx(source, object, toolchain, compile_intent)`
- `archive(objects, archive_path, toolchain)`
- `link_exe(objects, archives, shared_libs, external_libs, exe_path, toolchain, link_intent)`
- `link_shared(objects, archives, external_libs, so_path, toolchain, link_intent)`
- `invoke_tool(inputs, outputs, command_template, substitutions)`

Each resolves to exactly one backend-level rule execution. Header-dep tracking attaches to `compile_cxx` via the toolchain (`-MMD -MF $out.d` / equivalent).

### 3.4 Graph surface

```cpp
class Graph {
public:
    template<class T, class... Args>
    T& add(std::string name, Args&&... args);       // constructs & registers

    Target* find(std::string_view name);            // nullptr if absent
    std::vector<Target*> topLevel() const;          // targets not depended on by any other
    std::vector<Target*> topoOrder(Target& root) const;

    void setDefault(Target&);
    void setToolchain(std::unique_ptr<Toolchain>);

    // Lowering
    struct Expanded { std::vector<PrimitiveRule> rules; /* … */ };
    Expanded expand() const;
};
```

No print / explain / graphviz / list methods. When something is wrong, attach a debugger and inspect the `Graph` directly — it's plain C++ data.

---

## 4. User-Facing API

All types in `namespace build`. Fluent builders return `*this` (method chaining). Target handles are references owned by the `Graph`.

### 4.1 `Program`, `StaticLibrary`, `SharedLibrary`

Shared intent-setting surface (present on all three):

```cpp
T& cxx(std::vector<Path> sources);        // add source TUs
T& std(std::string_view version);         // "c++20", "c++23", "c++26" — toolchain translates
T& define(std::string macro);             // "FOO" or "FOO=1"
T& include(Path dir);                     // private include
T& public_include(Path dir);              // propagated to anything linking this target
T& warning_off(std::string_view name);    // "deprecated-declarations" → "-Wno-…"
T& flag_raw(std::string token);           // escape hatch: raw token, passed through verbatim
T& optimize(OptLevel);                    // O0 / O1 / O2 / O3 / Os / Og / Debug
T& debug(bool = true);                    // -g or equivalent
T& pic(bool = true);                      // -fPIC or equivalent

T& depend_on(Target& other);              // pure ordering edge, no flag transfer
T& link(Target& other);                   // link against another in-graph target
T& link(std::string_view external);       // system lib by name (e.g. "sdl3", "vulkan")
T& link_raw(std::string token);           // raw linker token (e.g. "-Wl,--start-group")
T& rpath(std::string path);               // runtime search path entry (link-exe / link-shared)
```

Per-kind additions:

```cpp
Program& install_to(Path dir);            // copy exe into dir after link (optional step)
StaticLibrary& position_independent();    // shorthand for .pic(true)
SharedLibrary& soname(std::string);
```

Example — ngen engine split into two static libraries plus the executable:

```cpp
auto& core = g.add<StaticLibrary>("enginecore")
    .cxx(coreSrcs)
    .std("c++23")
    .define("GLM_FORCE_RADIANS").define("GLM_FORCE_DEPTH_ZERO_TO_ONE")
    .public_include("src").public_include("external/glm")
    .optimize(OptLevel::O0).debug().pic();

auto& usdbind = g.add<StaticLibrary>("enginensdbind")
    .cxx(usdSrcs)
    .std("c++20")                                     // OpenUSD forces C++20 on consumers
    .warning_off("deprecated-declarations")
    .public_include("external/openusd_build/include")
    .optimize(OptLevel::O0).debug().pic();

auto& view = g.add<Program>("ngen-view")
    .cxx({"src/main.cpp"})                            // entry point lives on the exe itself
    .link(core).link(usdbind)
    .link("sdl3").link("vulkan").link("m")
    .link_raw("-lusd_usd").link_raw("-lusd_usdGeom") /* … */
    .rpath("$ORIGIN/../external/openusd_build/lib");

g.setDefault(view);
```

### 4.2 `Tool`

Arbitrary command steps (shader compilation, codegen, asset preprocessing). Two shapes:

```cpp
// Single invocation
Tool& command(std::vector<std::string> argv_template);   // tokens, with $in / $out markers
Tool& inputs(std::vector<Path>);
Tool& output(Path);

// N-to-N (one invocation per input, computed output)
Tool& for_each(std::vector<Path> inputs,
               std::function<Path(const Path&)> outputFor);
```

Example — shaders:

```cpp
auto& shaders = g.add<Tool>("shaders")
    .command({"glslc", "$in", "-o", "$out"})
    .for_each(shaderSrcs, [](const Path& src){ return src.string() + ".spv"; });

view.depend_on(shaders);                              // ngen-view pulls shaders in
```

### 4.3 Intent vs. raw flags

The API separates *intent* (portable) from *raw* (toolchain-specific). `.define("X")` works on any toolchain; `.flag_raw("-fno-rtti")` is an escape hatch and
is silently passed through. Prefer intent. When intent is missing, we either add an intent verb (preferred) or the user reaches for `flag_raw`.

### 4.4 What lives on the `Graph` vs. on Targets

On the `Graph`: toolchain, default target, top-level behaviours (output dir, verbosity). On Targets: everything about how that target is built. No hidden
global state.

---

## 5. Toolchain Abstraction

The `Toolchain` interface is the only place flag-syntax knowledge lives. Everything above it deals in intent.

```cpp
struct CompileIntent {
    Path source;
    Path object;
    std::string std;                    // "c++20", "c++23", "c++26"
    OptLevel opt;                       // enum
    bool debug, pic;
    std::vector<std::string> defines;
    std::vector<Path>        includes;  // public + private, deduped
    std::vector<std::string> warning_off;
    std::vector<std::string> raw;
};

struct LinkIntent {
    std::vector<Path>        objects;
    std::vector<Path>        archives;        // static libs to swallow
    std::vector<Path>        shared_libs;     // full paths to .so's
    std::vector<std::string> external_libs;   // "-l"-style names
    std::vector<Path>        lib_search;      // "-L"-style
    std::vector<std::string> rpaths;
    std::vector<std::string> raw;
    Path output;
};

class Toolchain {
public:
    virtual ~Toolchain() = default;
    virtual std::string name() const = 0;                     // "clang", "gcc", "msvc"

    virtual Command compile_cxx(const CompileIntent&) const = 0;
    virtual Command archive(std::vector<Path> objs, Path out) const = 0;
    virtual Command link_exe(const LinkIntent&) const = 0;
    virtual Command link_shared(const LinkIntent&) const = 0;

    // Header-dep support. Returns the pair of flags to inject and the depfile pattern the
    // backend should expose to Ninja (e.g. {"$out.d", "gcc"}).
    struct DepSupport { std::string depfile; std::string deps_format; };
    virtual std::optional<DepSupport> dep_support() const = 0;

    // Naming conventions.
    virtual std::string static_lib_name(std::string_view stem) const = 0;   // libfoo.a / foo.lib
    virtual std::string shared_lib_name(std::string_view stem) const = 0;   // libfoo.so / foo.dll
    virtual std::string exe_name(std::string_view stem) const = 0;
};

class ClangToolchain : public Toolchain {
public:
    struct Config {
        Path cxx       = "clang++";      // compiler + driver
        Path ar        = "ar";
        Path linker    = {};             // empty → use cxx as the link driver
        std::vector<std::string> extra_cxx_flags;   // appended to every compile
        std::vector<std::string> extra_link_flags;  // appended to every link
    };
    explicit ClangToolchain(Config = {});
    /* … */
};
// Later: GccToolchain, MsvcToolchain, ClangClToolchain.
```

Concrete toolchains expose a `Config` struct so users can see and override the compiler/linker/archiver paths directly in `build.cpp`. No `detectToolchain()`
magic — see §9 for the call site. The graph stores intent on targets; at expansion time, each primitive rule's intent is handed to the current Toolchain to
produce a `Command`. Swapping toolchain means constructing a different one and calling `g.setToolchain(...)` before expansion. No target code changes.

---

## 6. Backends

```cpp
class Backend {
public:
    virtual ~Backend() = default;
    // Full pipeline: expand graph, materialise rules, execute.
    virtual bool build(const Graph&, Target& desired) = 0;
};
```

### 6.1 Ninja backend (Phase 1)

`NinjaBackend`:

1. Calls `graph.expand()` to get the flat primitive-rule list.
2. Emits `_out/build.ninja`: one `rule <name>` per primitive kind (`cxx`, `cxx_usd`-equivalent is gone — flags differ per *rule* now, not per rule kind), plus
   `rule archive`, `rule link_exe`, `rule link_shared`, and one `rule <toolName>` per distinct `Tool` command template. Depfile + deps emitted on compile
   rules when the toolchain reports support.
3. Emits `build <out>: <rule> <inputs…>` for each primitive rule, with a per-rule indented `flags = …` when needed. Phony groups emitted as
   `build <out>: phony <inputs…>` with no command.
4. Emits `default <target>` if set.
5. Writes `compile_commands.json` as a side output by walking the compile primitive rules.
6. `std::system("ninja -f _out/build.ninja <desired>")`; forwards exit code.

### 6.2 In-process backend (Phase N)

`InProcessBackend`: same `Backend` interface, different body. Topological schedule, thread pool, `posix_spawn` / `CreateProcessW`, depfile parsing for header
deps, mtime + command-hash incrementality. Deferred because Ninja already does all this well; the framework is designed so dropping this in is additive.

---

## 7. Three-Stage Bootstrap and Self-Rebuild

Three binaries, each responsible for building the next. Each is its own build program with its own graph, its own emitted ninja file, and its own debug
output. The separation is the debugging boundary: you can inspect `prebuild`'s behaviour without the engine graph in the picture, and vice versa.

```
┌──────────────────┐   ninja      ┌──────────────┐   runs    ┌──────────────┐   runs   ┌─────────────┐
│ bootstrap.ninja  │ ───────────▶ │  ./prebuild  │ ────────▶ │ ./ngen-build │ ───────▶ │ ./ngen-view │
│ (hand-written)   │              │  (agnostic)  │           │  (ngen-only) │          │   (engine)  │
└──────────────────┘              └──────────────┘           └──────────────┘          └─────────────┘
                                     │                          │
                                     │ emits _out/prebuild.ninja│ emits _out/build.ninja
                                     │ + runs ninja on it       │ + runs ninja on it
```

### 7.1 What each stage owns

- **`bootstrap.ninja`** (hand-written). Compiles `build/prebuild.cpp` + the subset of `build/framework/*.cpp` that it uses → `./prebuild`, using
  `-std=c++26`. Minimal; no SDL3, no Vulkan, no USD. When the framework grows, this file gains source entries but stays small.
- **`./prebuild`** (build program). Its graph describes how to compile `build.cpp` (and any `build/*.cpp` growth) plus the framework it depends on →
  `./ngen-build`, using `.std("c++26")` on everything it produces. Uses the full `Program`/`StaticLibrary` API. When `build.cpp` splits into multiple TUs
  with shared helpers, `prebuild.cpp` is where the composition is described.
- **`./ngen-build`** (build program). Its graph is ngen's engine graph (§9). No self-rebuild plumbing in `build.cpp` — that all lives in `prebuild.cpp`.
  The engine libraries it produces use C++23 (with `sceneusd` at C++20); `./ngen-build` itself, being build-system code, was built with C++26.
- **`./ngen-view`** (engine). Final artifact. Produced by `./ngen-build`.

**Standard split, summarised:** the build system (framework, `prebuild.cpp`, `build.cpp`) compiles with **C++26**. The engine libraries compile with C++23,
except `sceneusd` at C++20. Each standard is scoped to the code that needs it; the build system gets the latest because it's the most constrained in its
external dependencies (stdlib only) and benefits most from modern language features when expressing graph construction.

### 7.2 Entry protocol — always run the full chain

Every invocation of a stage runs the entire upstream chain unconditionally. Correctness does not depend on any staleness comparison we perform ourselves —
it's delegated to ninja, which is authoritative. The chain is fast when nothing has changed (ninja no-ops all the way down); it is always correct when
something has.

The protocol at the top of each stage's `main()`:

```
if not --no-self-rebuild:
    invoke parent stage (blocking)          # parent may rebuild us on disk
    execv(argv[0], argv + [--no-self-rebuild])   # replace this process with the fresh binary
else:
    # --no-self-rebuild is set: parent already ran and re-exec'd us.
    # Proceed with this stage's own work.
    ...emit ninja, run it, etc.
```

Per stage:

- **`./ngen-build`.** Parent is `./prebuild`. Invokes `./prebuild` with no args (prebuild runs its own chain), then execs self with `--no-self-rebuild`
  appended.
- **`./prebuild`.** Parent is `ninja -f build/bootstrap.ninja`. Invokes ninja on the bootstrap file (which checks and rebuilds `./prebuild` if sources
  changed), then execs self with `--no-self-rebuild` appended.
- **`bootstrap.ninja`.** No parent. Ninja's own incrementality decides whether any work is needed.

The `--no-self-rebuild` flag is strictly a loop-breaker: it says "the chain above me has already run this pass; don't run it again." There is no user-facing
meaning beyond that. Users do not pass it; only the entry protocol itself does, during the self-exec.

No mtime comparison in user code, no heuristic to get wrong. If `prebuild.cpp` is stale, ninja-on-bootstrap rebuilds `./prebuild`; `./prebuild` then runs and
ninja-on-prebuild.ninja rebuilds `./ngen-build`; `./ngen-build` then runs and ninja-on-build.ninja rebuilds the engine. Each link in the chain is a
ninja-level operation, which is what ninja is good at.

### 7.3 Why three stages, not two

The critical user-stated reason: each stage is a debugging boundary. When `./prebuild` misbehaves you attach a debugger to `./prebuild` and step through a
small graph that *only* describes compiling `build.cpp`. You never have to wade through the engine graph in memory. Same when the engine build misbehaves —
attaching to `./ngen-build` gives you only engine concerns. Mixing the two into a single `build.cpp` with self-compile rules (as I originally had it) collapses
that boundary.

Secondary reason: scale. `build.cpp` will grow into `build/*.cpp` with helpers and possibly its own mini-modules. `prebuild.cpp` correspondingly grows to
describe that. Each stays focused.

### 7.4 Cold start and recovery

- **Fresh clone.** `ninja -f build/bootstrap.ninja && ./ngen-build <target>`. The bootstrap produces `./prebuild`; invoking `./ngen-build` (which does not
  yet exist but prebuild will produce it as the chain runs) requires prebuild first — so the one-liner is
  `ninja -f build/bootstrap.ninja && ./prebuild && ./ngen-build <target>`. Once `./ngen-build` exists, every subsequent invocation chains through prebuild
  itself and the user only ever types `./ngen-build <target>`.
- **Broken `build.cpp`.** `./ngen-build` fails to compile inside the chain; prebuild prints the error and the chain aborts before the engine build runs.
  Previous `./ngen-build` is still on disk. Fix source and rerun.
- **Broken `prebuild.cpp`.** `./prebuild` fails to compile inside the chain; the bootstrap-ninja step prints the error. Previous `./prebuild` is still on
  disk. Fix source and rerun.
- **Broken `bootstrap.ninja`.** Hand-edit required. Rare; this file is tiny.
- **Broken framework code.** Whichever stage uses the broken TU fails to link; the chain reports it at that stage's parent. Fix and rerun.

---

## 8. Debugging

The build program is a plain C++ binary. Debugging is done by attaching a debugger to it:

```
gdb --args ./ngen-build <target>
```

Set breakpoints in `Graph::expand()`, in a specific target's `cxx()` / `link()` call, in the toolchain's `compile_cxx()`, or wherever is relevant. Inspect
the `Graph` structure, intent lists, and expanded rules directly in C++ memory. Same for `./prebuild`.

This replaces a parallel introspection surface (no `--print`, `--explain`, `--graphviz`, `--list`, `--dry-run` flags, no `introspection.cpp`). Those would be
duplicated work against a medium the debugger already gives us for free.

One flag that is *not* introspection but is part of the entry protocol (§7.2) remains:

- `./ngen-build --no-self-rebuild` — skip the "invoke parent and re-exec" step. Used only by the entry protocol itself, to break the otherwise-infinite
  recursion after it has re-exec'd. Users do not pass it.

Same flag works on `./prebuild`.

---

## 9. ngen's `build.cpp` — Concrete Shape

Sketch assuming the API in §4 and the ngen structure. Not final code; shows the shape:

One `StaticLibrary` per top-level folder under `src/`: **obs**, **renderer**, **rhi**, **rhi/vulkan**, **scene**, **ui**. The `src/rhi/` split (backend-agnostic
interfaces separate from the Vulkan implementation) matches the project's folder convention and keeps future backends (d3d12, metal) as siblings of
`rhivulkan`. Cross-cutting TUs at `src/` root (`main.cpp`, `camera.cpp`, `debugdraw.cpp`, `jobsystem.cpp`) stay on the `Program` itself — they are the
glue, not a library.

One deviation from "one library per folder": the USD TUs (`src/scene/usd*.cpp`) require C++20 plus `-Wno-deprecated-declarations` plus the USD include
path, while the rest of `src/scene/` compiles with C++23 under normal engine flags. Because a `StaticLibrary` carries one flag set for all its TUs, `scene`
splits into two libraries — `scene` (non-USD) and `sceneusd` (USD-specific). This is the only folder that splits.

```cpp
#include "build/framework/build.hpp"
using namespace build;

// Common engine flag set applied to non-USD libraries and to the Program.
static StaticLibrary& applyEngineCxx(StaticLibrary& l, const std::vector<std::string>& sdl3Cflags) {
    return l.std("c++23")
            .define("GLM_FORCE_RADIANS").define("GLM_FORCE_DEPTH_ZERO_TO_ONE")
            .optimize(OptLevel::O0).debug().pic()
            .public_include("src")
            .public_include("external/glm").public_include("external/cgltf")
            .public_include("external/stb").public_include("external/imgui")
            .public_include("external/imgui/backends").public_include("external/concurrentqueue")
            .flag_raw_many(sdl3Cflags);
}

int main(int argc, char** argv) {
    Graph g;

    // Toolchain is declared here, in the build program, so it's visible and
    // editable. No auto-detection — if you want a different compiler/linker,
    // change this block.
    ClangToolchain clang{{
        .cxx    = "clang++",
        .ar     = "ar",
        .linker = {},                                    // empty: use cxx as link driver
    }};
    g.setToolchain(clang);

    auto sdl3Cflags = captureTokens("pkg-config --cflags sdl3");
    auto sdl3Libs   = captureTokens("pkg-config --libs   sdl3");

    // --- one static library per src/ folder -------------------------------
    auto& obs      = applyEngineCxx(g.add<StaticLibrary>("obs"),      sdl3Cflags)
                        .cxx(glob("src/obs/**/*.cpp"));

    auto& rhi      = applyEngineCxx(g.add<StaticLibrary>("rhi"),      sdl3Cflags)
                        .cxx(glob("src/rhi/*.cpp"));                 // non-recursive: excludes vulkan/

    auto& rhivk    = applyEngineCxx(g.add<StaticLibrary>("rhivulkan"),sdl3Cflags)
                        .cxx(glob("src/rhi/vulkan/**/*.cpp"))
                        .link(rhi);                                  // uses rhi interfaces

    auto& renderer = applyEngineCxx(g.add<StaticLibrary>("renderer"), sdl3Cflags)
                        .cxx(glob("src/renderer/**/*.cpp"))          // includes passes/
                        .link(rhi);

    auto& scene    = applyEngineCxx(g.add<StaticLibrary>("scene"),    sdl3Cflags)
                        .cxx(glob("src/scene/*.cpp",
                                  /*exclude=*/ glob("src/scene/usd*.cpp")));

    auto& sceneusd = g.add<StaticLibrary>("sceneusd")                // USD-specific flag set
                        .cxx(glob("src/scene/usd*.cpp"))
                        .std("c++20")
                        .warning_off("deprecated-declarations")
                        .define("GLM_FORCE_RADIANS").define("GLM_FORCE_DEPTH_ZERO_TO_ONE")
                        .optimize(OptLevel::O0).debug().pic()
                        .public_include("src")
                        .public_include("external/openusd_build/include")
                        .public_include("external/glm").public_include("external/imgui")
                        .flag_raw_many(sdl3Cflags);

    auto& ui       = applyEngineCxx(g.add<StaticLibrary>("ui"),       sdl3Cflags)
                        .cxx(glob("src/ui/**/*.cpp"))
                        .link(renderer).link(scene);

    // ImGui stays as its own library (vendored third-party TUs).
    auto& imgui    = applyEngineCxx(g.add<StaticLibrary>("imgui"),    sdl3Cflags)
                        .cxx(imguiSources());                         // the 6 imgui TUs

    // --- shaders ----------------------------------------------------------
    auto& shaders  = g.add<Tool>("shaders")
                        .command({"glslc", "$in", "-o", "$out"})
                        .for_each(glob("shaders/*.vert") + glob("shaders/*.frag"),
                                  [](const Path& s){ return s.string() + ".spv"; });

    // --- executable -------------------------------------------------------
    auto& view = g.add<Program>("ngen-view")
        .cxx({"src/main.cpp", "src/camera.cpp", "src/debugdraw.cpp", "src/jobsystem.cpp"})
        .std("c++23")
        .define("GLM_FORCE_RADIANS").define("GLM_FORCE_DEPTH_ZERO_TO_ONE")
        .link(obs).link(rhi).link(rhivk).link(renderer).link(scene).link(sceneusd).link(ui).link(imgui)
        .link("vulkan").link("m")
        .flag_raw_many(sdl3Cflags).flag_raw_many(sdl3Libs)
        .link_raw("-lusd_usd").link_raw("-lusd_usdGeom").link_raw("-lusd_usdShade")
        // … full USD library list …
        .rpath("$ORIGIN/../external/openusd_build/lib")
        .depend_on(shaders);

    g.setDefault(view);

    // Non-graph verbs (clean/format/tidy) handled by main dispatch before here.
    return NinjaBackend{}.build(g, resolveDesiredFromArgs(argc, argv, view)) ? 0 : 1;
}
```

Observations:

- Eight libraries total: six per user spec (obs, rhi, rhivulkan, renderer, scene, ui), plus `sceneusd` (forced by the USD compile requirements), plus `imgui`
  (vendored third-party, naturally its own library). The Program itself compiles the four cross-cutting `src/*.cpp` files directly.
- Inter-library `.link()` edges (rhivulkan→rhi, renderer→rhi, ui→renderer+scene) reflect compile-time header dependencies; they propagate `.public_include`
  from the linked library into the consumer's compile commands. Exact edges will firm up in Phase 1 as we verify compiles.
- `applyEngineCxx` is a one-function helper in `build.cpp` to avoid repeating the common flag set. Not part of the framework — it's project-local shorthand.
- `sceneusd` does not use the helper because its flag set diverges meaningfully (C++20, extra include, warning suppression).
- `sdl3Cflags` / `sdl3Libs` resolved in-process via `popen` (no backticks reach the backend).
- Splitting the monolithic `enginecore` into folder-matched libraries gives faster incremental builds (a change in `src/ui/` only recompiles `ui`'s TUs and
  relinks the Program) and clearer archive-level ownership.

---

## 10. Phased Migration

Each phase is independently shippable; the Makefile remains usable until phase 5.

**Phase 0 — Framework skeleton, one Target type, Ninja backend.**
- Implement `Graph`, `Target`, `Program`, `Toolchain` interface + `ClangToolchain`, `NinjaBackend`, `Command`, `Flags`.
- Toy program builds a two-file hello-world via `Program("hello").cxx({…})`.
- Accept: `./hello` runs.

**Phase 1 — Full Target types and composition.**
- Add `StaticLibrary`, `SharedLibrary`, `Tool`, `.public_include` propagation, transitive link, rpath handling.
- Toy program that assembles a StaticLibrary + a Program linking it + a Tool.
- Accept: transitive deps and include propagation verified.

**Phase 2 — Bootstrap chain and entry protocol on Linux.**
- Write `build/bootstrap.ninja`, `build/prebuild.cpp`, ngen's `build.cpp`.
- Implement the entry protocol (§7.2) in `./prebuild` and `./ngen-build`: unconditional parent invocation + execv-self-with-`--no-self-rebuild`.
- Accept: every `./ngen-build` invocation runs the full chain; editing `build.cpp` or `prebuild.cpp` is picked up on the next invocation without any manual
  step; unchanged state is fast (ninja no-ops end-to-end).

**Phase 3 — ngen-view parity and polish.**
- `ngen-view` builds and runs on Linux matching Makefile output.
- `compile_commands.json` emitted; `bear` dropped.
- `clean` / `format` / `tidy` as dispatch verbs.
- Accept: developer workflow is `./ngen-build` instead of `make`; problems in the build are debugged with `gdb ./ngen-build`.

**Phase 4 — Toolchain and platform breadth.**
- Implement `ClangMacOsToolchain` (or fold into `ClangToolchain` with platform dispatch).
- Implement `MsvcToolchain` and/or `GccToolchain` as demand emerges.
- Accept: green build on each platform we target.

**Phase 5 — Retire `Makefile`.**
- Delete or shrink to a one-line forwarder.
- Update `CLAUDE.md` Build section and `README.md`.
- Accept: no `make` references in docs; CI green.

**Phase N — In-process backend.**
- Implement `InProcessBackend` (§6.2).
- Gate behind `./ngen-build --backend=inproc`.
- Accept: matches Ninja on a full rebuild within latency budget; ships when the `ninja` dependency becomes inconvenient somewhere.

**Phase M — Extract the framework.**
- Move `build/framework/` to its own repo, consume as a submodule at `external/build/`.
- Accept: other projects can bring in the same framework and write their own `build.cpp`.

---

## 11. Risks / Open Questions

- **pkg-config as an intent.** Day 1 uses `flag_raw_many(capture(...))`. Later, consider `.use_pkgconfig("sdl3")` as a proper intent so the toolchain /
  platform layer decides whether to call pkg-config, vcpkg, or a hand-rolled lookup. Not urgent.
- **Transitive link policy.** Static → static pulls in archives; shared → shared pulls in `-l<name> + rpath`; static → shared is ambiguous (include the shared
  lib at program link time or embed it into the archive?). Decide by convention: static libs record their shared-lib deps as "to-be-linked-later", and any
  Program/SharedLibrary that links them inherits those at final link. Document and test explicitly in Phase 1.
- **`.public_include` deduplication.** With many targets exporting their own include dirs, Program compile commands grow. Dedupe at expansion; enforce order
  (public-then-private) so overrides are predictable.
- **Windows.** `bootstrap.ninja` is Unix-shaped. For Windows, either ship a `bootstrap.bat` that compiles `prebuild.cpp` with `cl.exe` or require
  `ninja`+`clang` on PATH. Defer to Phase 4.
- **Fluent API mistakes are silent.** `.cxx()` on a target that forgot `.std()` would inherit toolchain default. Maybe require `.std()` as a mandatory step
  (fail at expansion if absent). Discuss when we see real usage.
- **Namespace collision.** The name `build::` is generic enough to clash with user code. Acceptable because it's scoped to build programs, not ngen engine
  code. Revisit if it proves awkward.
- **No cycle detection at API level.** Adding `.link(a)` on `b` and `.link(b)` on `a` silently produces a cycle; backend detects but late. Add cycle check
  in `Graph::expand()` in Phase 1.

---

## 12. Non-Goals (Explicit)

- **Cross-compilation** (Linux → Windows, etc.) beyond what native toolchains give. Revisit only when demanded.
- **Package management.** No fetching, vendoring, or version resolution. Dependencies come via the project's own submodules / system packages.
- **Full CMake/Meson replacement.** We target ngen and similar C++ projects. We're not aiming for broad ecosystem support (language front-ends, obscure
  platforms, IDE integrations beyond `compile_commands.json`).
- **Generic rule DSL.** The framework's leverage comes from the typed Target vocabulary. Adding a "raw rule" escape hatch is discouraged — if you need it,
  add a Target subclass or a `Tool` instance instead.

Explicitly *not* a non-goal anymore: **reusability across projects.** That is a primary goal.
