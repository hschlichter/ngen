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

**Toolchain-aware from day one, concrete toolchains project-local.** The framework ships only the abstract `Toolchain` interface and reusable helpers;
each project writes its own concrete impls under `build/toolchains/` (Clang, Emscripten, GCC, MSVC — whichever it needs). Intent stays in the target; the
toolchain object owns translation to flags and argv. New toolchains are drop-in additions on the project side, not framework rewrites.

**Platform as a first-class axis, orthogonal to Configuration.** The framework models *what OS + graphics API you target* (Linux/Vulkan, Wasm/WebGPU, …) as
`Platform`, separately from *how you build* (debug/release/gamerelease) as `Configuration`. Each platform owns its toolchain. Artifacts for each
platform × config combination live in their own `_out/<platform>/<config>/` subtree so they coexist on disk.

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
    glob.hpp              / glob.cpp        # glob({.include=…, .exclude=…}) → vector<Path>
    command.hpp           # Command (argv), tokenisation helpers
    flags.hpp             # Flag intent types (Define, Include, Std, WarningOff, Raw, …)
    target.hpp            # Target abstract base
    program.hpp           / program.cpp
    library.hpp           / library.cpp         # Library: linkage decided by active Configuration
    staticlibrary.hpp     / staticlibrary.cpp
    sharedlibrary.hpp     / sharedlibrary.cpp
    tool.hpp              / tool.cpp
    graph.hpp             / graph.cpp          # Graph: owns targets/platforms/configs, expansion, traversal
    configuration.hpp                          # Configuration struct (opt, debug_info, linkage, defines, …)
    platform.hpp                               # Platform struct (os, graphics_api, toolchain, system_libs, …)
    toolchain.hpp                              # abstract Toolchain interface (concrete impls live per-project)
    toolchainhelpers.hpp  / toolchainhelpers.cpp   # optional utilities for impls (depfile formatting, quoting, …)
    backend.hpp                                # Backend interface
    backendninja.hpp      / backendninja.cpp

  toolchains/             # project-local: ngen's concrete toolchain implementations
    clang.hpp    / clang.cpp       # ClangToolchain — Linux/macOS native; uses framework toolchainhelpers
    emscripten.hpp / emscripten.cpp   # EmscriptenToolchain — wasm/wasi via emcc (Phase 4)

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
| `Library`         | `.a` or `.so`/…      | Resolves to Static or Shared at expansion, per active `Configuration::default_linkage` (overridable via `.linkage(...)`). |
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

### 4.1 `Program`, `Library`, `StaticLibrary`, `SharedLibrary`

Four build-target kinds. `Program` always produces an executable. The three library types differ only in how linkage is chosen:

- **`Library`** — linkage decided at expansion time. Picks up `Configuration::default_linkage` unless the target sets `.linkage(Linkage::Static|Shared)`
  explicitly. This is the everyday choice.
- **`StaticLibrary`** — always produces a `.a`, regardless of config.
- **`SharedLibrary`** — always produces a `.so` / `.dylib` / `.dll`, regardless of config.

Use `Library` by default. Reach for `StaticLibrary` or `SharedLibrary` only when a specific library genuinely *must* be one or the other no matter the
configuration (vendored deps with ABI quirks, loadable plugins, etc.).

All four share the same intent-setting surface:

```cpp
T& cxx(std::vector<Path> sources);        // add source TUs
T& std(std::string_view version);         // "c++20", "c++23", "c++26" — overrides the toolchain's default_std
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
Library& linkage(Linkage);                // override default_linkage for this Library only
StaticLibrary& position_independent();    // shorthand for .pic(true)
SharedLibrary& soname(std::string);
```

Example — ngen engine split into two libraries plus the executable. `core` follows the active config's linkage; `usdbind` is forced static by a project-level
decision about OpenUSD:

```cpp
auto& core = g.add<Library>("enginecore")
    .cxx(coreSrcs)
        .define("GLM_FORCE_RADIANS").define("GLM_FORCE_DEPTH_ZERO_TO_ONE")
    .public_include("src").public_include("external/glm")
    .optimize(OptLevel::O0).debug().pic();

auto& usdbind = g.add<StaticLibrary>("enginensdbind")   // always static regardless of config
    .cxx(usdSrcs)
    .std("c++20")                                        // OpenUSD forces C++20 on consumers
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

### 4.3 File selection: `glob`

Every build program ends up listing source files; `glob` is the framework-provided helper for that. One shape, used consistently:

```cpp
struct GlobSpec {
    std::string include;          // required — a single shell-style pattern
    std::string exclude;          // optional — empty means no filtering
};

std::vector<Path> glob(GlobSpec);
```

Supports `**` for recursive descent. Both fields are single patterns — for multiple patterns, call `glob` twice and concatenate the results with `+`. Typical
uses:

```cpp
glob({.include = "src/ui/**/*.cpp"})                                    // one pattern
glob({.include = "src/scene/*.cpp", .exclude = "src/scene/usd*.cpp"})   // with filter
glob({.include = "shaders/*.vert"}) + glob({.include = "shaders/*.frag"})   // union
```

Keeping `glob` in the framework (rather than project-local) means every consumer of the framework gets consistent file-selection semantics without each
project re-implementing its own helper.

### 4.4 Intent vs. raw flags

The API separates *intent* (portable) from *raw* (toolchain-specific). `.define("X")` works on any toolchain; `.flag_raw("-fno-rtti")` is an escape hatch and
is silently passed through. Prefer intent. When intent is missing, we either add an intent verb (preferred) or the user reaches for `flag_raw`.

### 4.5 Configurations

A **Configuration** is a named bundle of build-wide choices — optimization level, debug info, default library linkage, extra compile/link flags, which
targets are active, and an output directory. Exactly one is active per invocation; you select it on the command line:

```
./ngen-build --config release ngen-view
./ngen-build --config gamerelease
./ngen-build                                   # defaults to the first-registered config
```

Each config's artifacts land under `<out_dir>/<config_name>/` so all three coexist on disk without colliding. Prebuild is *not* parameterized by config —
`./ngen-build` itself is always built the same way, in `_out/build/`. Configuration only affects what `./ngen-build` produces.

**The `Configuration` type:**

```cpp
struct Configuration {
    std::string name;                                // "debug", "release", "gamerelease"
    OptLevel    opt             = OptLevel::O0;      // applied to every target unless overridden
    bool        debug_info      = true;              // -g equivalent
    Linkage     default_linkage = Linkage::Static;   // policy hint; see below
    std::vector<std::string> defines;                // added to every compile
    std::vector<std::string> extra_cxx_flags;        // added to every compile
    std::vector<std::string> extra_link_flags;       // added to every exe/shared link
    Path        out_dir         = "_out";            // actual dir: out_dir / name
};

g.addConfig(Configuration{...});    // first registered becomes the default
g.addConfig(Configuration{...});
```

**Per-target overrides** — `.when(name, fn)`:

```cpp
auto& renderer = g.add<Library>("renderer")
    .cxx(glob({.include = "src/renderer/**/*.cpp"}))
    .when("release",     [](auto& t){ t.define("ENABLE_PROFILER"); })
    .when("gamerelease", [](auto& t){ t.flag_raw("-flto").define("SHIPPING=1"); });
```

`.when()` runs the lambda only if the active config's name matches. Multiple `.when()`s per target are fine; they stack in registration order.

**Per-target inclusion** — `.only_in({...})` / `.except_in({...})`:

```cpp
auto& devtools = g.add<Library>("devtools")            // hypothetical dev-only library
    .cxx(glob({.include = "src/devtools/*.cpp"}))
    .only_in({"debug", "release"});                    // not built in gamerelease

auto& view = g.add<Program>("ngen-view")
    .link(core).link(renderer).link(devtools);         // automatically drops the devtools edge in gamerelease
```

A target excluded from the active config is absent from the expanded graph. Other targets that `.link()` it have that edge dropped automatically; explicit
`.link_if_present(devtools)` is redundant.

**Static vs shared linkage per config.** Use `Library` (§4.1) for any library whose linkage should follow the active config. At expansion the framework lowers
each `Library` to either a `StaticLibrary` or `SharedLibrary` subgraph, based on `Configuration::default_linkage`. `StaticLibrary` and `SharedLibrary` remain
available for libraries that genuinely must be one or the other regardless of config (vendored deps with ABI quirks, loadable plugins). Per-target override
is `.linkage(Linkage::Static|Shared)` on a `Library`.

**Starting set of configs for ngen:**

| Config         | Opt | Debug info | Default linkage | Key defines          | Notes                                                              |
|----------------|-----|------------|-----------------|----------------------|--------------------------------------------------------------------|
| `debug`        | O0  | full       | Static          | `DEBUG=1`            | Default. Asserts on; extra diagnostic code paths on.               |
| `release`      | O2  | yes        | Static          | `NDEBUG`             | Optimised with debug info for profiling and crash dumps.           |
| `gamerelease`  | O3  | none       | Shared          | `NDEBUG`, `SHIPPING=1` | LTO, hidden visibility, stripped. Shared libs for smaller binary and easier patching. |

These are starting points, not framework defaults — they live in ngen's `build.cpp`. Other projects define their own configs.

### 4.6 Platforms

A **Platform** is an orthogonal axis to Configuration. Where a Configuration says *how* to build (opt, debug, linkage, asserts), a Platform says *what* you are
building for: operating system, graphics API, toolchain, system libraries, artifact naming. One platform is active per invocation; you pick it alongside the
config:

```
./ngen-build --platform linux-vulkan --config debug ngen-view
./ngen-build --platform wasm-webgpu  --config release
./ngen-build                                               # first-registered platform + first-registered config
```

Artifacts go under `<out_dir>/<platform>/<config>/`, so all combinations coexist on disk: `_out/linux-vulkan/debug/`, `_out/wasm-webgpu/release/`, etc.

**Why Platform owns the toolchain.** The choice of compiler is dictated by the target OS, not by the developer's build mode. Linux/Vulkan builds call `clang++`;
Wasm/WebGPU builds call `emcc`. Toolchain therefore moves out of `Graph` and into each `Platform`. The user still writes the toolchain config inline in
`build.cpp` — just now it lives inside the `addPlatform` call rather than a top-level `setToolchain`. Same visibility; different locus.

**The `Platform` type:**

```cpp
struct Platform {
    std::string name;                                  // "linux-vulkan", "wasm-webgpu"
    std::string os;                                    // "linux", "wasi"
    std::string graphics_api;                          // "vulkan", "webgpu"
    std::unique_ptr<Toolchain> toolchain;              // native clang vs emscripten, etc.
    std::vector<std::string> defines;                  // applied to every compile on this platform
    std::vector<std::string> extra_cxx_flags;
    std::vector<std::string> extra_link_flags;
    std::vector<std::string> system_libs;              // linked into every Program on this platform
    std::string exe_suffix = "";                       // "" / ".exe" / ".wasm"
};

g.addPlatform(Platform{...});                          // first registered = default
const Platform& active = g.activePlatform();
```

**Per-target platform conditionals.** Mirrors the config API — different verbs so the two axes don't collide:

- `.when_platform(name, fn)` — run `fn` only if the active platform matches.
- `.only_on({names…})` / `.except_on({names…})` — presence filter by platform. Edges to targets excluded on this platform drop automatically.

**Starting pair for ngen:**

| Platform       | OS      | Graphics | Toolchain     | Key defines                              | Notes                                                              |
|----------------|---------|----------|---------------|------------------------------------------|--------------------------------------------------------------------|
| `linux-vulkan` | linux   | vulkan   | `clang++`     | `NGEN_PLATFORM_LINUX`, `NGEN_GFX_VULKAN` | Primary. System libs: vulkan, m. `exe_suffix = ""`.                |
| `wasm-webgpu`  | wasi    | webgpu   | `emcc`        | `NGEN_PLATFORM_WASM`,  `NGEN_GFX_WEBGPU` | Secondary. Emscripten provides WebGPU. `exe_suffix = ".wasm"`.     |

Additional platforms (Windows/Vulkan, Windows/DX12, macOS/Metal) are not Phase 1 scope — they come online as Toolchain impls land.

**Prebuild is platform-agnostic.** `./ngen-build` itself is always a native host binary (you run it from your dev machine). Platforms only affect the *engine
artifacts* that `./ngen-build` produces. `build/prebuild.cpp` and `build.cpp`'s own compile use the host's default toolchain; platform selection starts at the
engine graph.

**Source file selection per platform.** Platform-specific libraries (`rhivulkan` vs a future `rhiwebgpu`) use `.only_on({...})` and the framework drops them
from the expanded graph on the other platform. The `Program` declares `.link(rhivulkan).link(rhiwebgpu)` unconditionally; the framework resolves which edge
survives based on the active platform.

**Platform × Configuration matrix.** The two axes multiply: three configs × two platforms = six buildable variants, each in its own `_out/<platform>/<config>/`
subtree. They're independent — you don't need a new config entry per platform, and vice versa. A target's `.when("release", ...)` applies whatever the
platform; `.when_platform("wasm-webgpu", ...)` applies whatever the config; combining them is chaining two `.when...` calls on the same target.

### 4.7 What lives on the `Graph` vs. on Targets

On the `Graph`: registered platforms (each owning its toolchain), registered configs, default target, active platform + config selection. On Targets:
everything about how that target is built, including per-config and per-platform `.when...()` blocks. No hidden global state.

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
```

**Concrete toolchains are project-local, not framework.** The framework ships the interface above and a small `toolchainhelpers.hpp` with utilities
implementations commonly need (gcc-style depfile flag assembly, shell-quoting, joining argv vectors). It does *not* ship a `ClangToolchain` or any other
compiler-specific implementation. Reasons: toolchain choice is genuinely project-specific (ccache wrappers, mold linker, distributed/deterministic builds,
in-house compilers), and a single blessed `ClangToolchain` in the framework would either be wrong for most consumers or pressure everyone into the same
narrow opinions.

Projects put their concrete toolchains under `build/toolchains/` (see §2 folder layout). For ngen those files are:

```cpp
// build/toolchains/clang.hpp  — ngen-local, not framework.
class ClangToolchain : public build::Toolchain {
public:
    struct Config {
        Path cxx         = "clang++";     // compiler + driver
        Path ar          = "ar";
        Path linker      = {};            // empty → use cxx as the link driver
        std::string default_std = "c++23"; // used by CompileIntent when the target didn't set .std(...)
        std::vector<std::string> extra_cxx_flags;   // appended to every compile
        std::vector<std::string> extra_link_flags;  // appended to every link
    };
    explicit ClangToolchain(Config = {});
    /* … implements the Toolchain virtuals using framework helpers … */
};

// build/toolchains/emscripten.hpp  — ngen-local, Phase 4.
class EmscriptenToolchain : public build::Toolchain {
public:
    struct Config {
        Path emcc        = "emcc";
        Path emar        = "emar";
        std::string default_std = "c++23";
        std::vector<std::string> extra_cxx_flags;
        std::vector<std::string> extra_link_flags;   // e.g. "-sUSE_WEBGPU=1", "-sWASM=1"
    };
    explicit EmscriptenToolchain(Config = {});
    /* … */
};
```

A `Config` struct per concrete toolchain makes its knobs visible in `build.cpp`; the `default_std` field lives here because it's a project-language
opinion. The framework's `Toolchain` interface knows nothing about C++ standards specifically — a hypothetical `RustToolchain` in another project would carry
an `edition` field instead; a `CToolchain` would carry `c_std`. Targets that don't call `.std(...)` inherit the active toolchain's default; `.std(...)` on a
target overrides.

**The toolchain is owned by the active `Platform` (§4.6), not by `Graph` directly.** At expansion time, each primitive rule's intent is handed to the active
platform's Toolchain to produce a `Command`. Adding a new platform means constructing a `Platform` with its own toolchain and calling `g.addPlatform(...)` —
see §9 for the call site. No target code changes.

**Reference implementations.** There's a first-time cost to writing a `ClangToolchain` (~100–200 lines of argv assembly). ngen's copy becomes the de facto
reference — other projects adopting the framework can copy it and modify. A curated "reference toolchains" repo external to the framework is a reasonable
future artefact; it is explicitly *not* part of the framework itself.

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

### 7.2 Entry protocol — always run prebuild

The chain is a one-way pipeline driven by `execv`. Each stage's last act is to hand control to the next one. Nothing ever re-execs itself.

**`./prebuild` main()**:

```
1. system("ninja -f build/bootstrap.ninja")   # idempotent; rebuilds on-disk prebuild if needed
2. emit _out/prebuild.ninja
3. system("ninja -f _out/prebuild.ninja")     # idempotent; rebuilds on-disk ngen-build if needed
4. execv("./ngen-build", argv)                # hand off to fresh ngen-build
```

**`./ngen-build` main()**:

```
1. if this process was not reached via prebuild's execv:
       execv("./prebuild", argv)              # user invoked us directly; delegate
2. emit _out/build.ninja
3. system("ninja -f _out/build.ninja")        # builds engine
```

The "reached via prebuild" signal is an **internal env var** (e.g. `NGEN_FROM_PREBUILD=1`) that `./prebuild` sets just before its `execv` in step 4. Users
do not interact with it; it's not a CLI flag. Its only job is to tell `./ngen-build` "you are already fresh — don't bounce back through prebuild."

What this gives:

- **`build.cpp` edits** — first invocation is correct. Prebuild's step 3 rebuilds `./ngen-build` from the new `build.cpp`, then step 4 execs the fresh binary,
  which emits the updated engine ninja.
- **Engine source edits** — first invocation is correct. Ninja in step 3 of `./ngen-build` detects source changes and rebuilds.
- **`prebuild.cpp` edits** — takes two invocations to fully propagate. Step 1 rebuilds `./prebuild` on disk, but the currently-running prebuild process is
  still the *old* binary — so its step 2 (emitting `_out/prebuild.ninja`) uses old logic. On the next invocation, the user's process starts from the fresh
  prebuild and everything is consistent. Acceptable: `prebuild.cpp` changes are rare and usually produce an equivalent prebuild.ninja anyway.
- **`bootstrap.ninja` edits** — hand-edited, no chain involvement. Changes take effect on the next run.

No re-exec, no loop-breaker flag, no mtime heuristic. One env var, hidden, that prevents the direct-user-invokes-`./ngen-build` path from bouncing more than
once.

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

No special CLI flags on `./ngen-build` or `./prebuild` beyond the normal target name. The "are we being handed off from prebuild" signal used by the entry
protocol (§7.2) is an env var, not a flag, and is invisible to users. To debug `./ngen-build` in isolation, set that env var yourself so it skips the
prebuild delegation: `NGEN_FROM_PREBUILD=1 gdb --args ./ngen-build <target>`.

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
#include "build/toolchains/clang.hpp"            // ngen-local toolchain
#include "build/toolchains/emscripten.hpp"       // ngen-local toolchain (Phase 4)
using namespace build;

int main(int argc, char** argv) {
    Graph g;

    // --- platforms --------------------------------------------------------
    // Each platform owns its toolchain, so the compiler choice is visible and
    // editable right here. First-registered is the default. The toolchain
    // implementations (ClangToolchain, EmscriptenToolchain) are project-local
    // under build/toolchains/, not framework.
    // Project-wide defines and cxx flags live on Platform so they are applied once
    // rather than repeated on every library declaration below.
    const std::vector<std::string> ngenDefines = { "GLM_FORCE_RADIANS", "GLM_FORCE_DEPTH_ZERO_TO_ONE" };
    auto sdl3Cflags = captureTokens("pkg-config --cflags sdl3");     // Linux-only
    auto sdl3Libs   = captureTokens("pkg-config --libs   sdl3");

    g.addPlatform(Platform{
        .name            = "linux-vulkan",
        .os              = "linux",
        .graphics_api    = "vulkan",
        .toolchain       = std::make_unique<ClangToolchain>(ClangToolchain::Config{
            .cxx         = "clang++",
            .ar          = "ar",
            .linker      = {},                           // empty: use cxx as link driver
            .default_std = "c++23",                      // ngen-wide default; targets override with .std(...)
        }),
        .defines         = concat({"NGEN_PLATFORM_LINUX", "NGEN_GFX_VULKAN"}, ngenDefines),
        .extra_cxx_flags = concat({"-fPIC"}, sdl3Cflags),
        .system_libs     = {"vulkan", "m"},
        .exe_suffix      = "",
    });
    g.addPlatform(Platform{
        .name             = "wasm-webgpu",
        .os               = "wasi",
        .graphics_api     = "webgpu",
        .toolchain        = std::make_unique<EmscriptenToolchain>(EmscriptenToolchain::Config{
            .emcc             = "emcc",
            .emar             = "emar",
            .default_std      = "c++23",
            .extra_link_flags = {"-sUSE_WEBGPU=1", "-sWASM=1", "-sALLOW_MEMORY_GROWTH=1"},
        }),
        .defines          = concat({"NGEN_PLATFORM_WASM", "NGEN_GFX_WEBGPU"}, ngenDefines),
        .extra_cxx_flags  = {"-fPIC"},
        .system_libs      = {},                          // emcc bundles webgpu
        .exe_suffix       = ".wasm",
    });
    const Platform& activePlatform = g.activePlatform();

    // --- configurations ---------------------------------------------------
    g.addConfig(Configuration{
        .name            = "debug",
        .opt             = OptLevel::O0,
        .debug_info      = true,
        .default_linkage = Linkage::Static,
        .defines         = {"DEBUG=1"},
    });
    g.addConfig(Configuration{
        .name            = "release",
        .opt             = OptLevel::O2,
        .debug_info      = true,                         // keep -g for profiling / crash dumps
        .default_linkage = Linkage::Static,
        .defines         = {"NDEBUG"},
    });
    g.addConfig(Configuration{
        .name             = "gamerelease",
        .opt              = OptLevel::O3,
        .debug_info       = false,
        .default_linkage  = Linkage::Shared,
        .defines          = {"NDEBUG", "SHIPPING=1"},
        .extra_cxx_flags  = {"-fvisibility=hidden"},
        .extra_link_flags = {"-flto", "-Wl,-s", "-Wl,--gc-sections"},
    });
    const Configuration& active = g.activeConfig();      // chosen from --config, default = first

    // --- one library per src/ folder -------------------------------------
    // Library picks static/shared from active Configuration.default_linkage.
    // sceneusd is explicitly StaticLibrary — OpenUSD's compile quirks stay contained
    // in a static archive regardless of config.
    // Per-library: sources, std, exposed includes, inter-library links. The
    // ngen-wide concerns (GLM defines, -fPIC, sdl3 cflags) live on Platform above.
    const Path src = "src";

    auto& obs = g.add<Library>("obs")
        .cxx(glob({.include = "src/obs/**/*.cpp"}))
        .public_include(src);

    auto& rhi = g.add<Library>("rhi")
        .cxx(glob({.include = "src/rhi/*.cpp"}))        // non-recursive: excludes vulkan/
        .public_include(src);

    auto& rhivk = g.add<Library>("rhivulkan")
        .cxx(glob({.include = "src/rhi/vulkan/**/*.cpp"}))
        .only_on({"linux-vulkan"})
        .public_include(src)
        .link(rhi);

    auto& rhiwgpu = g.add<Library>("rhiwebgpu")
        .cxx(glob({.include = "src/rhi/webgpu/**/*.cpp"}))   // future implementation
        .only_on({"wasm-webgpu"})
        .public_include(src)
        .link(rhi);

    auto& renderer = g.add<Library>("renderer")
        .cxx(glob({.include = "src/renderer/**/*.cpp"}))
        .public_include(src)
        .link(rhi);

    auto& scene = g.add<Library>("scene")
        .cxx(glob({.include = "src/scene/*.cpp",
                    .exclude = "src/scene/usd*.cpp"}))
        .public_include(src);

    auto& sceneusd = g.add<StaticLibrary>("sceneusd")
        .std("c++20")                            // explicit: OpenUSD requires C++20
        .cxx(glob({.include = "src/scene/usd*.cpp"}))
        .warning_off("deprecated-declarations")
        .public_include(src)
        .public_include("external/openusd_build/include");

    auto& ui = g.add<Library>("ui")
        .cxx(glob({.include = "src/ui/**/*.cpp"}))
        .public_include(src)
        .link(renderer).link(scene);

    // --- shaders ---------------------------------------------------------
    auto& shaders  = g.add<Tool>("shaders")
    .command({"glslc", "$in", "-o", "$out"})
    .for_each(glob({.include = "shaders/*.vert"}) + glob({.include = "shaders/*.frag"}),
            [](const Path& s){ return s.string() + ".spv"; });

    // --- executable ------------------------------------------------------
    auto& view = g.add<Program>("ngen-view")
        .cxx({"src/main.cpp", "src/camera.cpp", "src/debugdraw.cpp", "src/jobsystem.cpp"})
        .link(obs)
        .link(rhi)
        .link(rhivk)
        .link(rhiwgpu)           // only the active-platform backend survives
        .link(renderer)
        .link(scene)
        .link(sceneusd)
        .link(ui)
        .flag_raw_many(sdl3Libs)                                  // link-time sdl3 libs; cflags are on Platform
        .when_platform("linux-vulkan", [](auto& t) {
            t
                .link_raw("-lusd_usd")
                .link_raw("-lusd_usdGeom")
                .link_raw("-lusd_usdShade")
                // … full USD library list, Linux-only …
                .rpath("$ORIGIN/../external/openusd_build/lib");
        })
        .when_platform("wasm-webgpu", [](auto& t) {
            // USD + SDL are Linux-only; the wasm build uses minimal replacements.
            t
                .flag_raw("-sUSE_SDL=3")                          // emcc port
                .link_raw("--preload-file=assets@/assets");        // bundle assets into the .wasm
        })
        .depend_on(shaders)
        .when("gamerelease", [](auto& t){
            t
                .flag_raw("-flto")
                .link_raw("-Wl,--strip-all");
        });

    g.setDefault(view);

    return NinjaBackend{}.build(g, resolveDesiredFromArgs(argc, argv, view)) ? 0 : 1;
}
```

Observations:

- Eight libraries on `linux-vulkan` (obs, rhi, rhivulkan, renderer, scene, sceneusd, ui, imgui); eight on `wasm-webgpu` (`rhivulkan` swaps for `rhiwebgpu`).
  The Program itself compiles the four cross-cutting `src/*.cpp` files directly.
- `glob` is a framework helper (§4.3). One form, `glob({.include = "…", .exclude = "…"})`, used consistently — designated-initialiser fields are
  self-labelling and let `.exclude` be added later without changing the call shape. `scene` uses it to exclude `usd*.cpp`, which live in `sceneusd` with
  their own flag set.
- **Project-wide flags live on `Platform`.** `GLM_FORCE_*` defines, `-fPIC`, and SDL3 cflags are set once on the active platform rather than repeated on
  every library. Each library declaration is short: sources, std version, its own exposed includes, and its inter-library link edges.
- `Configuration` supplies opt level, debug info, and linkage at expansion. `Platform` supplies OS/gfx defines, toolchain, and cross-cutting cxx flags.
  Targets carry only what is genuinely per-target.
- Every engine library is `Library` (linkage flows from config) except `sceneusd`, which is explicitly `StaticLibrary` because OpenUSD's compile quirks are
  easier to contain inside a static archive regardless of config. In `gamerelease`, all the `Library`-typed ones expand to `SharedLibrary` subgraphs;
  `sceneusd` stays static and gets absorbed into the `Program` link like any other `.a`.
- Inter-library `.link()` edges (rhivulkan→rhi, renderer→rhi, ui→renderer+scene) reflect compile-time header dependencies; they propagate `.public_include`
  from the linked library into the consumer's compile commands. Exact edges will firm up in Phase 1 as we verify compiles.
- Artifacts per platform × config land in `_out/<platform>/<config>/`. `./ngen-build --platform linux-vulkan --config release ngen-view` produces
  `_out/linux-vulkan/release/ngen-view`. `./ngen-build --platform wasm-webgpu --config release` produces `_out/wasm-webgpu/release/ngen-view.wasm`.
- On `linux-vulkan`, `rhivulkan` is in the graph and `rhiwebgpu` is not; the `Program` `.link(rhivk).link(rhiwgpu)` declarations both appear, and the
  framework drops the edge to whichever library is excluded from the active platform. No conditional `.link()` on the Program.
- `sdl3Cflags` / `sdl3Libs` resolved in-process via `popen` (no backticks reach the backend). `sdl3Cflags` goes on the linux platform's `extra_cxx_flags`;
  `sdl3Libs` goes on the Program's link flags because it's a link-time concern, not a compile-time concern.
- Splitting the monolithic `enginecore` into folder-matched libraries gives faster incremental builds (a change in `src/ui/` only recompiles `ui`'s TUs and
  relinks the Program) and clearer archive-level ownership.

---

## 10. Phased Migration

Each phase is independently shippable; the Makefile remains usable until phase 5.

**Phase 0 — Framework skeleton, one Target type, Ninja backend.**
- Implement `Graph`, `Target`, `Program`, the abstract `Toolchain` interface, `toolchainhelpers`, `NinjaBackend`, `Command`, `Flags`, `glob`.
- Write `build/toolchains/clang.hpp/.cpp` in the toy project (project-local, not framework) implementing `Toolchain` against Clang.
- Toy program builds a two-file hello-world via `Program("hello").cxx({…})`.
- Accept: `./hello` runs.

**Phase 1 — Full Target types and composition.**
- Add `StaticLibrary`, `SharedLibrary`, `Tool`, `.public_include` propagation, transitive link, rpath handling.
- Toy program that assembles a StaticLibrary + a Program linking it + a Tool.
- Accept: transitive deps and include propagation verified.

**Phase 2 — Bootstrap chain and entry protocol on Linux.**
- Write `build/bootstrap.ninja`, `build/prebuild.cpp`, ngen's `build.cpp`.
- Implement the entry protocol (§7.2): `./prebuild` always runs bootstrap-ninja, emits + runs prebuild.ninja, `execv`s `./ngen-build`; `./ngen-build`
  delegates to `./prebuild` unless an internal env var marks it as already handed off.
- Accept: every `./ngen-build` invocation runs the full chain; editing `build.cpp` or `prebuild.cpp` is picked up on the next invocation without any manual
  step; unchanged state is fast (ninja no-ops end-to-end).

**Phase 3 — ngen-view parity and polish.**
- `ngen-view` builds and runs on Linux matching Makefile output.
- `compile_commands.json` emitted; `bear` dropped.
- `clean` / `format` / `tidy` as dispatch verbs.
- Accept: developer workflow is `./ngen-build` instead of `make`; problems in the build are debugged with `gdb ./ngen-build`. Default config (debug) only —
  other configs land in the next phase.

**Phase 3b — Configurations and `Library`.**
- Implement `Configuration`, `Graph::addConfig` / `activeConfig`, `--config <name>` CLI parsing, per-config output dirs (nested under the platform dir).
- Implement `.when(name, fn)` and `.only_in({...})` / `.except_in({...})` on targets; wire into expansion so excluded targets and their incoming edges drop.
- Implement the `Library` type (§4.1): lowers to `StaticLibrary` or `SharedLibrary` subgraph at expansion per `Configuration::default_linkage`, overridable
  via `.linkage(Linkage)`.
- Register `debug` / `release` / `gamerelease` in ngen's `build.cpp`; convert the engine libraries to `Library` (keeping `sceneusd` as `StaticLibrary`).
  Put project-wide GLM defines, SDL3 cflags, and `-fPIC` on the linux platform's `defines` / `extra_cxx_flags` so per-library declarations stay small.
- Accept: `./ngen-build --config release ngen-view` and `./ngen-build --config gamerelease ngen-view` both produce working binaries in their own output
  dirs; the `gamerelease` build produces `.so`s for the `Library`-typed engine libraries and still links `sceneusd` as a static archive.

**Phase 4 — Platforms.**
- Implement `Platform` (owns `Toolchain`, defines + system_libs + exe_suffix), `Graph::addPlatform` / `activePlatform`, `--platform <name>` CLI parsing,
  per-platform output dirs: `_out/<platform>/<config>/`.
- Implement `.when_platform(name, fn)` and `.only_on({...})` / `.except_on({...})` on targets.
- Move existing Linux/Vulkan setup to a registered `linux-vulkan` platform, constructing ngen's existing `ClangToolchain` (under `build/toolchains/`).
- Add `build/toolchains/emscripten.hpp/.cpp` (ngen-local implementation of `Toolchain` against emcc). Register the `wasm-webgpu` platform. Create the
  `rhiwebgpu` library skeleton (`only_on({"wasm-webgpu"})`).
- Accept: `./ngen-build --platform linux-vulkan` produces a working native binary; `./ngen-build --platform wasm-webgpu` produces a loadable `.wasm`/`.html`
  artifact under `_out/wasm-webgpu/debug/`. Unused backend libraries are absent from each build's expanded graph.

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
- **Fluent API mistakes are silent.** `.cxx()` on a target that forgot `.std()` inherits the active toolchain's `default_std`. If a build program constructs
  a toolchain without setting `default_std` (or relies on the framework default being right), this can pick up an unintended standard version. Consider
  making expansion fail if any resolved std is empty.
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
