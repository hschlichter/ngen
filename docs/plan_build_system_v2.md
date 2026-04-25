# Plan v2 - A C++ Build Framework for ngen

This document supersedes `docs/plan_build_system.md`. It keeps the same core idea:
ngen's build description is a C++ program written against a reusable build
framework. The framework is project-agnostic and lives under `build/framework/`.
ngen is the first consumer.

The v2 changes are mostly corrective:

- Toolchain ownership is consistently modeled through `Platform`; the older
  `Graph::setToolchain()` idea is removed.
- The CLI explicitly supports target names plus build flags such as `--config`
  and `--platform`; only parallel introspection flags are out of scope.
- The bootstrap protocol avoids relinking over an executing binary.
- The concrete ngen graph includes the targets and include roots needed to match
  the current Makefile.
- Generated shader outputs live under `_out/<platform>/<config>/`, not next to
  sources.
- Wasm/WebGPU is treated as a later platform-enablement milestone, not a Phase 4
  promise of parity with the current Vulkan/OpenUSD application.

---

## 1. Goals

**The framework is reusable.** `build/framework/*` must have no engine knowledge.
It can be moved to another repository or consumed as a submodule later. Project
code lives outside the framework, specifically in:

- `build/bootstrap.ninja`
- `build/prebuild.cpp`
- `build.cpp`

**The build graph is the model.** User-facing targets such as `Program`,
`Library`, `StaticLibrary`, `SharedLibrary`, and `Tool` are composite graph
nodes. Expansion lowers them into primitive compile, archive, link, and tool
rules consumed by a backend.

**Intent is typed.** The build program expresses includes, defines, standard
version, warning suppressions, optimization, debug info, linkage, rpaths, and
system libraries as structured intent. Concrete toolchains turn intent into
argv.

**Ninja is the first backend.** Ninja handles scheduling, parallelism, mtime
incrementality, depfiles, and command reruns. An in-process backend remains a
future option behind the same expanded primitive graph.

**Debugging uses the debugger.** Build programs are plain C++ binaries. Use
`gdb --args ./_out/ngen-build ...` or `gdb --args ./_out/prebuild ...` to inspect graph
construction and expansion. The framework should not grow a parallel
introspection subsystem in v1.

**Ngen parity comes before expansion.** The first production milestone is a Linux
Vulkan build that matches the current Makefile. Configurations and additional
platforms come after that baseline is stable.

---

## 2. Repository Layout

```text
build/
  framework/
    build.hpp
    path.hpp
    glob.hpp / glob.cpp
    command.hpp
    flags.hpp
    target.hpp
    program.hpp / program.cpp
    library.hpp / library.cpp
    staticlibrary.hpp / staticlibrary.cpp
    sharedlibrary.hpp / sharedlibrary.cpp
    tool.hpp / tool.cpp
    graph.hpp / graph.cpp
    configuration.hpp
    platform.hpp
    toolchain.hpp
    cxxtoolchain.hpp / cxxtoolchain.cpp
    toolchainhelpers.hpp / toolchainhelpers.cpp
    backend.hpp
    backendninja.hpp / backendninja.cpp

  bootstrap.ninja
  prebuild.cpp

build.cpp
```

`build/framework/` is the extraction boundary. The build program still owns
project-specific toolchain configuration; it is written directly in `build.cpp`
instead of hidden behind a project-local toolchain wrapper.

---

## 3. User-Facing API

All public types live in `namespace build`.

### 3.1 Graph

```cpp
class Graph {
public:
    template<class T, class... Args>
    T& add(std::string name, Args&&... args);

    Target* find(std::string_view name);

    void addPlatform(Platform);
    void addConfig(Configuration);

    const Platform& activePlatform() const;
    const Configuration& activeConfig() const;

    void setDefault(Target&);

    struct Expanded {
        std::vector<PrimitiveRule> rules;
        Path out_dir;
    };
    Expanded expand(Target& desired) const;
};
```

There is no `Graph::setToolchain()`. The active `Platform` owns the active
toolchain.

### 3.2 Targets

Concrete target types:

| Type | Output | Expansion |
| --- | --- | --- |
| `Program` | executable | compile objects, then link executable |
| `Library` | `.a` or shared library | lowers according to active config linkage |
| `StaticLibrary` | `.a` | compile objects, then archive |
| `SharedLibrary` | `.so`, `.dylib`, or `.dll` | compile objects, then link shared |
| `Alias` | selected target output | resolves to another target by graph policy |
| `Tool` | declared outputs | invoke arbitrary commands |

Common target surface:

```cpp
T& cxx(std::vector<Path> sources);
T& std(std::string_view version);
T& define(std::string macro);
T& include(Path dir);
T& include(std::vector<Path> dirs);
T& public_include(Path dir);
T& public_include(std::vector<Path> dirs);
T& warning_off(std::string_view name);
T& flag_raw(std::string token);
T& flags_raw(std::vector<std::string> tokens);
T& optimize(OptLevel);
T& debug(bool = true);
T& pic(bool = true);

T& depend_on(Target& other);
T& link(Target& other);
T& link(std::string_view system_lib);
T& link_raw(std::string token);
T& link_raw_many(std::vector<std::string> tokens);
T& lib_search(Path dir);
T& rpath(std::string path);

T& when(std::string_view config_name, auto fn);
T& only_in(std::initializer_list<std::string_view> config_names);
T& except_in(std::initializer_list<std::string_view> config_names);

T& when_platform(std::string_view platform_name, auto fn);
T& only_on(std::initializer_list<std::string_view> platform_names);
T& except_on(std::initializer_list<std::string_view> platform_names);
```

`Library` additionally has:

```cpp
Library& linkage(Linkage);
```

`Alias` additionally has:

```cpp
Alias& to(Target& target);
Alias& select(std::string_view key, std::string_view value, Target& target);
Alias& fallback(Target& target);
```

At expansion, an `Alias` is replaced by the target it resolves to. The simplest
form is `.to(target)`, which gives a stable graph name to another target.
Conditional forms use graph-context keys, for example
`.select("platform", "linux-vulkan", target)` or
`.select("config", "gamerelease", target)`. The framework only needs a small
well-known context map at expansion time; `platform` and `config` are the first
keys, and project-defined feature keys can be added later if needed.

This keeps `Alias` general: it is a named indirection point in the graph. It can
represent a stable public name for an implementation target, the active RHI
backend, a default asset pipeline, a preferred test runner, or a system-vs-bundled
dependency choice.

`Tool` supports:

```cpp
Tool& command(std::vector<std::string> argv_template);
Tool& inputs(std::vector<Path>);
Tool& outputs(std::vector<Path>);
Tool& for_each(std::vector<Path> inputs,
               std::function<Path(const Path&)> output_for);
```

### 3.3 File Selection

```cpp
struct GlobSpec {
    std::string include;
    std::string exclude;
};

std::vector<Path> glob(GlobSpec);
std::vector<Path> concat(std::initializer_list<std::vector<Path>> lists);
```

`glob()` supports `**`. Multiple include patterns are expressed with `concat()`,
not with an implicit `operator+` on vectors.

Example:

```cpp
auto shader_sources = concat({
    glob({.include = "shaders/*.vert"}),
    glob({.include = "shaders/*.frag"}),
});
```

---

## 4. Platforms and Configurations

### 4.1 Platform

A platform models what the artifact targets: OS, graphics API, toolchain, common
defines, platform-specific flags, system libraries, and artifact suffixes.

```cpp
struct Platform {
    std::string name;
    std::string os;
    std::string graphics_api;
    std::unique_ptr<Toolchain> toolchain;
    std::vector<std::string> defines;
    std::vector<std::string> extra_cxx_flags;
    std::vector<std::string> extra_link_flags;
    std::vector<std::string> system_libs;
    std::string exe_suffix = "";
};
```

Initial ngen platform:

| Platform | Scope | Notes |
| --- | --- | --- |
| `linux-vulkan` | Phase 3 parity | Native `clang++`, Vulkan, SDL3, OpenUSD |

Future platform:

| Platform | Scope | Notes |
| --- | --- | --- |
| `wasm-webgpu` | Later enablement | Requires application and renderer portability work, not just `emcc` |

### 4.2 Configuration

A configuration models how the artifact is built.

```cpp
struct Configuration {
    std::string name;
    OptLevel opt = OptLevel::O0;
    bool debug_info = true;
    Linkage default_linkage = Linkage::Static;
    std::vector<std::string> defines;
    std::vector<std::string> extra_cxx_flags;
    std::vector<std::string> extra_link_flags;
    Path out_dir = "_out";
};
```

Initial configs:

| Config | Opt | Debug info | Default linkage | Defines |
| --- | --- | --- | --- | --- |
| `debug` | O0 | yes | Static | `DEBUG=1` |
| `release` | O2 | yes | Static | `NDEBUG` |
| `gamerelease` | O3 | no | Shared | `NDEBUG`, `SHIPPING=1` |

Artifacts live under:

```text
_out/<platform>/<config>/
```

For Phase 3, before config/platform work lands, the implementation may use:

```text
_out/linux-vulkan/debug/
```

as a fixed path to avoid a later migration.

---

## 5. Toolchain Interface

```cpp
struct CompileIntent {
    Path source;
    Path object;
    std::string std;
    OptLevel opt;
    bool debug;
    bool pic;
    std::vector<std::string> defines;
    std::vector<Path> includes;
    std::vector<std::string> warning_off;
    std::vector<std::string> raw;
};

struct LinkIntent {
    std::vector<Path> objects;
    std::vector<Path> archives;
    std::vector<Path> shared_libs;
    std::vector<std::string> external_libs;
    std::vector<Path> lib_search;
    std::vector<std::string> rpaths;
    std::vector<std::string> raw;
    Path output;
};

class Toolchain {
public:
    virtual ~Toolchain() = default;

    virtual std::string name() const = 0;
    virtual Command compile_cxx(const CompileIntent&) const = 0;
    virtual Command archive(std::vector<Path> objects, Path output) const = 0;
    virtual Command link_exe(const LinkIntent&) const = 0;
    virtual Command link_shared(const LinkIntent&) const = 0;

    struct DepSupport {
        std::string depfile;
        std::string deps_format;
    };
    virtual std::optional<DepSupport> dep_support(Path object) const = 0;

    virtual std::string static_lib_name(std::string_view stem) const = 0;
    virtual std::string shared_lib_name(std::string_view stem) const = 0;
    virtual std::string exe_name(std::string_view stem,
                                 std::string_view platform_suffix) const = 0;
};
```

ngen uses a framework-provided configurable C++ toolchain. The complete
configuration is written in `build.cpp` when the platform is registered; there
is no hidden `build/toolchains/clang.*` wrapper. The config should expose:

- compiler path
- archiver path
- optional linker path, with empty meaning use the compiler driver
- default C++ standard
- global extra compile and link flags
- shell/Ninja escaping through framework helpers

Expansion fails if a compile rule resolves to an empty standard.

---

## 6. Ninja Backend Requirements

The Ninja backend owns filesystem materialization and backend output.

It must:

1. Expand the graph for the requested target.
2. Create all required parent directories before writing generated files.
3. Emit `_out/<platform>/<config>/build.ninja`.
4. Emit one rule per primitive kind: `cxx`, `archive`, `link_exe`,
   `link_shared`, and tool rules.
5. Emit depfile metadata for compile rules when the toolchain supports it.
6. Emit phony target groups as `build <name>: phony <deps...>`.
7. Emit `default <target>` when a default target is set.
8. Emit `_out/<platform>/<config>/compile_commands.json`.
9. Invoke `ninja -f <generated-ninja> <desired-output-or-phony>`.

Directory creation can be implemented by the build program/backend before
emitting Ninja and by command prefixes for dynamically computed outputs. The
important invariant is that compile and archive commands never fail because
their output directories are missing.

---

## 7. Bootstrap and Self-Rebuild

There are three build programs/artifacts:

```text
build/bootstrap.ninja -> ./_out/prebuild -> ./_out/ngen-build -> ngen-view executable
```

### 7.1 Stage Ownership

`build/bootstrap.ninja`
: Hand-written. Builds the build system itself from source, producing both
  `_out/prebuild` and the initial `_out/ngen-build`. After
  `ninja -f build/bootstrap.ninja`, the normal user entry point exists.

`./_out/prebuild`
: Builds `_out/ngen-build` from `build.cpp` and the framework sources it needs.
  Then it execs the freshly built `_out/ngen-build`.

`./_out/ngen-build`
: Builds the engine graph. If invoked directly by the user, it delegates to
  `_out/prebuild` first unless an internal environment variable says it was already
  reached through prebuild.

### 7.2 No Self-Rebuild of Prebuild

`_out/prebuild` does not rebuild itself and does not invoke
`build/bootstrap.ninja`. Bootstrap creates `_out/prebuild`; later invocations of
`_out/ngen-build` run `_out/prebuild` only to refresh `_out/ngen-build`.

This avoids the running-binary overwrite problem for `_out/prebuild`: there is
no path where prebuild links over its own executable. When `_out/ngen-build`
needs to refresh itself, it first `execv`s into `_out/prebuild`, so the old
`_out/ngen-build` process is no longer running while prebuild links the new one.

### 7.3 Entry Protocol

`prebuild`:

```text
1. emit _out/prebuild.ninja
2. run ninja -f _out/prebuild.ninja
3. set NGEN_FROM_PREBUILD=1
4. execv("./_out/ngen-build", original argv)
```

`ngen-build`:

```text
1. if NGEN_FROM_PREBUILD is not set:
       execv("./_out/prebuild", original argv)
2. parse CLI
3. emit engine build.ninja
4. run ninja for the requested target
```

Fresh clone:

```sh
ninja -f build/bootstrap.ninja
```

This is the only fresh-clone setup command. Bootstrap creates `_out/prebuild`
and the initial `_out/ngen-build`; it does not build the engine programs. After
bootstrap has produced `_out/ngen-build`, the normal workflow is:

```sh
./_out/ngen-build
./_out/ngen-build --config release
./_out/ngen-build ngen-view
```

### 7.4 CLI

The build programs accept target names and build-control flags:

```text
./_out/ngen-build [--platform <name>] [--config <name>] [--backend ninja] [target]
```

Supported Phase 3 target names:

- `ngen-view`
- `shaders`
- `clean`
- `format`
- `tidy`

Out of scope for the first framework version:

- `--list`
- `--explain`
- `--graphviz`
- `--dry-run`
- bespoke introspection output

This distinction keeps normal build control explicit while avoiding a second
debugging surface.

---

## 8. Concrete ngen Graph

The current Makefile builds:

- every `src/**/*.cpp`
- ImGui core sources
- ImGui SDL3 and Vulkan backends
- all shaders under `shaders/*.vert` and `shaders/*.frag`
- USD TUs with C++20, USD include path, and `-Wno-deprecated-declarations`
- all other TUs with C++23

The v2 build graph must preserve that behavior first.

The first application target is `ngen-view`. The project name remains `ngen`;
future programs should be modeled as sibling `Program` targets with names such
as `ngen-import`, `ngen-pack`, or `ngen-inspect`, sharing libraries where it
makes sense.

### 8.1 Include Ownership

Include roots live on targets, not on `Platform` and not in one shared project
list. A target exports the directories needed by consumers of its public headers
with `.public_include(...)`; it keeps implementation-only directories private
with `.include(...)`. Dependency propagation then carries public include roots to
consumers.

The first migration can still be pragmatic because the current code uses flat
quoted includes across folders. If a target needs a broad private include to
compile today, declare it on that target. Do not hide that breadth in a global
list.

### 8.2 Targets

Suggested initial target split:

| Target | Type | Sources | Notes |
| --- | --- | --- | --- |
| `obs` | `Library` | `src/obs/**/*.cpp` | Needs `external/concurrentqueue` include root |
| `rhi` | `Library` | `src/rhi/*.cpp` | Backend-agnostic interfaces |
| `rhivulkan` | `Library` | `src/rhi/vulkan/**/*.cpp` | Linux Vulkan only, links `rhi` |
| `rhi-backend` | `Alias` | selected backend | Stable capability name; resolves to `rhivulkan` on `linux-vulkan` |
| `renderer` | `Library` | `src/renderer/**/*.cpp` | Links `rhi` and `rhi-backend` |
| `scene` | `Library` | `src/scene/*.cpp` excluding `src/scene/usd*.cpp` | Non-USD scene code |
| `sceneusd` | `StaticLibrary` | `src/scene/usd*.cpp` | C++20, USD includes, warning suppression |
| `ui` | `Library` | `src/ui/**/*.cpp` | Links renderer, scene, sceneusd |
| `imgui` | `StaticLibrary` | explicit `.cxx({...})` list in the target declaration | Core plus SDL3/Vulkan backends |
| `ngen-view` | `Program` | `src/main.cpp`, `src/camera.cpp`, `src/debugdraw.cpp`, `src/jobsystem.cpp` | First application program; links all libraries |
| `shaders` | `Tool` | `shaders/*.vert`, `shaders/*.frag` | Emits SPIR-V under `_out/.../shaders` |

`rhi-backend` is an alias target. The renderer depends on "the active RHI
backend" rather than spelling `rhivulkan` directly. This is one use of `Alias`,
not the definition of it. Today that alias still resolves to Vulkan because
`src/renderer/renderer.cpp` includes `rhieditoruivulkan.h`;
cleanly removing that implementation dependency is a renderer/RHI refactor, not
part of the build-system migration.

### 8.3 Linux Vulkan Platform Sketch

```cpp
auto sdl3_cflags = capture_tokens({"pkg-config", "--cflags", "sdl3"});
auto sdl3_libs = capture_tokens({"pkg-config", "--libs", "sdl3"});

auto cxx = std::make_unique<CxxToolchain>(CxxToolchain::Config{
    .name = "clang",
    .cxx = "clang++",
    .ar = "ar",
    .linker = "",
    .default_std = "c++23",
});

g.addPlatform(Platform{
    .name = "linux-vulkan",
    .os = "linux",
    .graphics_api = "vulkan",
    .toolchain = std::move(cxx),
    .defines = {
        "NGEN_PLATFORM_LINUX",
        "NGEN_GFX_VULKAN",
        "GLM_FORCE_RADIANS",
        "GLM_FORCE_DEPTH_ZERO_TO_ONE",
    },
    .extra_cxx_flags = concat_tokens({"-fPIC", "-Wall"}, sdl3_cflags),
    .system_libs = {"vulkan", "m"},
});
```

`capture_tokens()` should execute argv directly, not through shell backticks.
Phase 1 can use `popen()` as a pragmatic implementation if arguments are fixed
and controlled; the helper should still return tokenized output.

### 8.4 USD Linkage

Use explicit library search and link entries rather than one opaque raw string:

```cpp
view
    .lib_search("external/openusd_build/lib")
    .rpath("$ORIGIN/../external/openusd_build/lib")
    .link_raw("-lusd_usd")
    .link_raw("-lusd_usdGeom")
    .link_raw("-lusd_usdShade")
    .link_raw("-lusd_usdLux")
    .link_raw("-lusd_sdf")
    .link_raw("-lusd_pcp")
    .link_raw("-lusd_tf")
    .link_raw("-lusd_vt")
    .link_raw("-lusd_gf")
    .link_raw("-lusd_ar")
    .link_raw("-lusd_arch")
    .link_raw("-lusd_plug")
    .link_raw("-lusd_js")
    .link_raw("-lusd_work")
    .link_raw("-lusd_trace")
    .link_raw("-lusd_ts")
    .link_raw("-lusd_pegtl")
    .link_raw("-lusd_kind");
```

The rpath should be tested from the final executable location. If the executable
lands in `_out/linux-vulkan/debug/ngen-view`, `$ORIGIN/../../../external/openusd_build/lib`
may be required instead of the Makefile's current root-relative layout. Prefer an
absolute rpath matching the current Makefile first if runtime lookup becomes a
distraction:

```text
-Wl,-rpath,<repo-root>/external/openusd_build/lib
```

Then revisit relative `$ORIGIN` once install/package layout exists.

### 8.5 Shader Outputs

Do not write generated SPIR-V into `shaders/`.

For source:

```text
shaders/lighting.vert
```

emit:

```text
_out/linux-vulkan/debug/shaders/lighting.vert.spv
```

The engine currently expects shader files by path. Phase 3 can either:

1. copy generated shader outputs into the runtime working tree as an explicit
   compatibility step, or
2. update runtime shader lookup to prefer `_out/<platform>/<config>/shaders`.

The plan should choose option 1 for strict Makefile parity and defer runtime path
cleanup.

---

## 9. Build.cpp Shape

This is intentionally shown as a full-context sketch. The exact helper
signatures can change during implementation, but the build description should
remain this explicit: platform setup, configuration setup, source grouping,
target declarations, link policy, default target, and backend dispatch are all
visible in one place.

```cpp
#include "build/framework/build.hpp"

using namespace build;

int main(int argc, char** argv) {
    Graph g;

    auto sdl3_cflags = capture_tokens({"pkg-config", "--cflags", "sdl3"});

    auto cxx = std::make_unique<CxxToolchain>(CxxToolchain::Config{
        .name = "clang",
        .cxx = "clang++",
        .ar = "ar",
        .linker = "",
        .default_std = "c++23",
    });

    g.addPlatform(Platform{
        .name = "linux-vulkan",
        .os = "linux",
        .graphics_api = "vulkan",
        .toolchain = std::move(cxx),
        .defines = {
            "NGEN_PLATFORM_LINUX",
            "NGEN_GFX_VULKAN",
            "GLM_FORCE_RADIANS",
            "GLM_FORCE_DEPTH_ZERO_TO_ONE",
        },
        .extra_cxx_flags = concat_tokens({"-fPIC", "-Wall"}, sdl3_cflags),
        .system_libs = {"vulkan", "m"},
    });

    g.addConfig(Configuration{
        .name = "debug",
        .opt = OptLevel::O0,
        .debug_info = true,
        .default_linkage = Linkage::Static,
        .defines = {"DEBUG=1"},
    });

    g.addConfig(Configuration{
        .name = "release",
        .opt = OptLevel::O2,
        .debug_info = true,
        .default_linkage = Linkage::Static,
        .defines = {"NDEBUG"},
    });

    g.addConfig(Configuration{
        .name = "gamerelease",
        .opt = OptLevel::O3,
        .debug_info = false,
        .default_linkage = Linkage::Shared,
        .defines = {"NDEBUG", "SHIPPING=1"},
        .extra_cxx_flags = {"-fvisibility=hidden"},
        .extra_link_flags = {"-flto", "-Wl,-s", "-Wl,--gc-sections"},
    });

    g.selectFromArgs(argc, argv); // resolves active platform/config before expansion.

    auto& obs = g.add<Library>("obs")
        .cxx(glob({.include = "src/obs/**/*.cpp"}))
        .public_include({"src/obs", "external/concurrentqueue"});

    auto& rhi = g.add<Library>("rhi")
        .cxx(glob({.include = "src/rhi/*.cpp"}))
        .public_include({"src/rhi"})
        .include({"external/imgui"});

    auto& rhivulkan = g.add<Library>("rhivulkan")
        .cxx(glob({.include = "src/rhi/vulkan/**/*.cpp"}))
        .public_include({"src/rhi/vulkan"})
        .include({"external/imgui", "external/imgui/backends"})
        .only_on({"linux-vulkan"})
        .link(rhi);

    auto& rhi_backend = g.add<Alias>("rhi-backend")
        .select("platform", "linux-vulkan", rhivulkan);

    auto& renderer = g.add<Library>("renderer")
        .cxx(glob({.include = "src/renderer/**/*.cpp"}))
        .public_include({"src/renderer", "src/renderer/passes"})
        .include({"src", "src/scene", "src/obs"})
        .link(obs)
        .link(rhi)
        .link(rhi_backend);

    auto& scene = g.add<Library>("scene")
        .cxx(glob({
            .include = "src/scene/*.cpp",
            .exclude = "src/scene/usd*.cpp",
        }))
        .public_include({"src", "src/scene"})
        .include({"src/ui", "src/renderer"});

    auto& sceneusd = g.add<StaticLibrary>("sceneusd")
        .std("c++20")
        .cxx(glob({.include = "src/scene/usd*.cpp"}))
        .public_include({"src", "src/scene"})
        .warning_off("deprecated-declarations")
        .include("external/openusd_build/include");

    auto& imgui = g.add<StaticLibrary>("imgui")
        .cxx({
            "external/imgui/imgui.cpp",
            "external/imgui/imgui_draw.cpp",
            "external/imgui/imgui_tables.cpp",
            "external/imgui/imgui_widgets.cpp",
            "external/imgui/imgui_demo.cpp",
            "external/imgui/backends/imgui_impl_vulkan.cpp",
            "external/imgui/backends/imgui_impl_sdl3.cpp",
        })
        .public_include({"external/imgui", "external/imgui/backends"});

    auto& ui = g.add<Library>("ui")
        .cxx(glob({.include = "src/ui/**/*.cpp"}))
        .public_include({"src/ui"})
        .include({"src", "src/obs", "src/rhi", "src/rhi/vulkan"})
        .link(renderer)
        .link(scene)
        .link(sceneusd)
        .link(imgui);

    auto& shaders = g.add<Tool>("shaders")
        .command({"glslc", "$in", "-o", "$out"})
        .for_each(concat({
            glob({.include = "shaders/*.vert"}),
            glob({.include = "shaders/*.frag"}),
        }), [&](const Path& source) {
            return g.activeOutputDir() / "shaders" / (source.filename().string() + ".spv");
        });

    auto& view = g.add<Program>("ngen-view")
        .cxx({
            "src/main.cpp",
            "src/camera.cpp",
            "src/debugdraw.cpp",
            "src/jobsystem.cpp",
        })
        .include({"src", "src/obs", "src/rhi", "src/rhi/vulkan", "src/renderer",
                  "src/renderer/passes", "src/scene", "src/ui"})
        .link(obs)
        .link(rhi)
        .link(renderer)
        .link(scene)
        .link(sceneusd)
        .link(ui)
        .link(imgui)
        .link_raw_many(capture_tokens({"pkg-config", "--libs", "sdl3"}))
        .depend_on(shaders)
        .lib_search("external/openusd_build/lib")
        // May become an absolute rpath first; see the rpath note in section 8.4.
        .rpath("$ORIGIN/../../../external/openusd_build/lib")
        .link_raw("-lusd_usd")
        .link_raw("-lusd_usdGeom")
        .link_raw("-lusd_usdShade")
        .link_raw("-lusd_usdLux")
        .link_raw("-lusd_sdf")
        .link_raw("-lusd_pcp")
        .link_raw("-lusd_tf")
        .link_raw("-lusd_vt")
        .link_raw("-lusd_gf")
        .link_raw("-lusd_ar")
        .link_raw("-lusd_arch")
        .link_raw("-lusd_plug")
        .link_raw("-lusd_js")
        .link_raw("-lusd_work")
        .link_raw("-lusd_trace")
        .link_raw("-lusd_ts")
        .link_raw("-lusd_pegtl")
        .link_raw("-lusd_kind");

    g.setDefault(view);

    Target& desired = resolve_desired_target(g, argc, argv, view);
    return NinjaBackend{}.build(g, desired) ? 0 : 1;
}
```

Exact link edges can be tightened after compile verification. The first graph
should be allowed to over-link internal static libraries if that gets parity
quickly.

---

## 10. Migration Phases

### Phase 0 - Minimal Framework Spike

- Implement `Path`, `Command`, `Graph`, `Program`, `Toolchain`,
  `CxxToolchain`, `NinjaBackend`, and `glob`.
- Build a tiny hello-world from `build.cpp`.
- Emit a valid Ninja file and run it.

Acceptance:

- A manually compiled `_out/ngen-build` can build and run the toy executable
  through Ninja.

### Phase 1 - Target Composition

- Add `StaticLibrary`, `SharedLibrary`, and `Library`.
- Implement public/private include propagation.
- Implement transitive static-library link behavior.
- Add cycle detection in `Graph::expand()`.
- Add depfile support.
- Add `compile_commands.json`.

Acceptance:

- A toy `StaticLibrary + Program + Tool` graph works.
- Include propagation and transitive static links are covered by tests or small
  build fixtures.

### Phase 2 - Bootstrap Chain

- Add `build/bootstrap.ninja`.
- Add `build/prebuild.cpp`.
- Implement the `NGEN_FROM_PREBUILD` handoff.
- Build `_out/ngen-build` from `build.cpp`.
- Make `build/bootstrap.ninja` produce `_out/ngen-build` without starting the
  engine build.

Acceptance:

- `ninja -f build/bootstrap.ninja` produces `_out/ngen-build`.
- Subsequent `./_out/ngen-build` invocations run through prebuild.
- Editing `build.cpp` is picked up on the next invocation.

### Phase 3 - Linux Vulkan Makefile Parity

- Implement the concrete ngen graph from section 8.
- Include ImGui core and SDL3/Vulkan backend sources.
- Preserve all current include roots.
- Preserve USD C++20 compile settings and link libraries.
- Build shaders into `_out/.../shaders` and copy for runtime compatibility.
- Add `clean`, `format`, and `tidy` dispatch targets.

Acceptance:

- `./_out/ngen-build` builds a working Linux Vulkan `ngen-view` binary.
- The binary can run from the same workflow as the Makefile output.
- `compile_commands.json` exists under the active output directory.
- The Makefile remains available during validation.

### Phase 4 - Configurations

- Implement `Configuration`, `--config`, config-specific output dirs.
- Add `debug`, `release`, and `gamerelease`.
- Convert internal libraries to `Library` where config-driven linkage is useful.

Acceptance:

- `./_out/ngen-build --config debug`
- `./_out/ngen-build --config release`
- `./_out/ngen-build --config gamerelease`

all build into separate output directories. Shared-library gamerelease can be
deferred if it exposes unresolved symbol/export issues; do not block release
config on that.

### Phase 5 - Platform Axis

- Implement `Platform`, `--platform`, and platform-specific output dirs.
- Move Linux Vulkan setup into a registered platform.
- Keep `wasm-webgpu` as a skeleton until the app has real portability seams.

Acceptance:

- `./_out/ngen-build --platform linux-vulkan` matches the prior default.
- Excluded platform-specific targets are absent from the expanded graph.

### Phase 6 - Retire or Shrink Makefile

- Replace Makefile with a thin forwarder or remove it.
- Update `README.md` and `CLAUDE.md`.
- Remove stale references to `make` as the primary workflow.

Acceptance:

- Repository docs use `./_out/ngen-build`.
- Existing developer tasks have equivalents.

### Phase N - In-Process Backend

- Add `InProcessBackend` behind `--backend inproc`.
- Implement topological scheduling, process spawning, depfile parsing, mtime and
  command-hash incrementality.

Acceptance:

- Matches Ninja behavior on clean and incremental builds.
- Ships only when the Ninja dependency is a real problem.

---

## 11. Risks and Decisions to Lock Early

**Rpath from `_out`.** The Makefile links with an rpath based on repository root.
Moving the executable deeper into `_out/<platform>/<config>/` changes relative
runtime lookup. Use an absolute rpath first if needed.

**Current renderer leaks Vulkan.** `renderer.cpp` includes a Vulkan RHI header.
The build graph reflects this through the `rhi-backend` alias, which currently resolves to
`rhivulkan` on `linux-vulkan`. Fixing the implementation dependency is a
renderer architecture task.

**Shared-library gamerelease can expose symbol visibility problems.** Treat
shared linkage as config infrastructure plus a later hardening task. Static
release parity is more important initially.

**Tool target output paths affect runtime.** Shader outputs in `_out` are cleaner
for build isolation but require a compatibility copy or runtime path update.
Make that explicit in Phase 3.

**pkg-config is still raw.** Day 1 can capture `pkg-config` tokens for SDL3.
Later, a `.use_pkg_config("sdl3")` intent would be cleaner.

**No silent cycles.** `Graph::expand()` must detect cycles and report target
names. Letting Ninja discover cycles later makes debugging worse.

**No hidden vector magic.** Use explicit helpers such as `concat()` and
`concat_tokens()`. Do not rely on non-standard operators in examples unless the
framework defines them.

---

## 12. Non-Goals

- Package management, dependency fetching, or vendoring.
- Full CMake/Meson replacement scope.
- Cross-compilation beyond what a registered platform/toolchain explicitly
  supports.
- A generic raw-rule DSL. Prefer typed targets or `Tool`.
- Build graph introspection commands in the first implementation.
