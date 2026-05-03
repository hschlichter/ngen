# Build System

This document is the working reference for ngen's self-hosted build system. It describes what's actually in the tree and how the pieces fit together.

The build system is a small header-only C++ framework under `build/framework/` that you write your project graph against (in `build/build.cpp`), plus a four-stage
Ninja-based bootstrap chain that compiles and runs that project graph to produce the actual project's `_out/build.ninja`.

---

## 1. Files on disk

```text
build/
  bootstrap.ninja              # 12 lines, hand-written seed
  bootstrap.cpp                # ngen-build orchestrator (stage 1)
  prebuild.cpp                 # ngen-build-pre (stage 2)
  build.cpp                    # project graph; compiled into ngen-build-graph (stage 3)
  framework/                   # header-only library, no .cpp files, no umbrella
    alias.hpp                  # Alias — fluent wrapper, attached as extension on build::Target
    backend.hpp                # BuildVariant + forward decls of Platform/Configuration
    backendninja.hpp           # detail::Emitter, NinjaBackend (writes _out/build.ninja); join_command, ninja_escape_path
    command.hpp                # Command (argv vector)
    configuration.hpp          # build::Configuration — fluent class with ExtensionMap
    extensionmap.hpp           # ExtensionMap (type-erased; owning add + non-owning attach; nullable get)
    glob.hpp                   # GlobSpec, glob, concat, capture_tokens, repo_root, write_if_changed, shell_quote, split_ws, Error
    path.hpp                   # Path
    platform.hpp               # build::Platform — fluent class with ExtensionMap
    project.hpp                # Project — registers entry targets / platforms / configs
    target.hpp                 # build::Target — identity + deps + gating + ExtensionMap
    tool.hpp                   # Tool — fluent wrapper, attached as extension on build::Target

    cxx/
      backendninja.hpp         # CompileInputs/LinkInputs + compile_/archive_/link_command
      configuration.hpp        # build::cxx::Configuration — fluent wrapper, attached as extension on build::Configuration
      objectfile.hpp           # build::cxx::ObjectFile — per-translation-unit node, attached as extension on build::Target
      platform.hpp             # build::cxx::Platform — fluent wrapper, attached as extension on build::Platform (composes Toolchain)
      target.hpp               # build::cxx::Target — fluent wrapper, attached as extension on build::Target; OptLevel + Kind enums
      toolchain.hpp            # build::cxx::Toolchain — tools only (compiler / archiver / linker / default_std)
```

**Header-only.** Every public type and free function is defined in one of these headers. Free functions are marked `inline`; class methods are defined inside
their class bodies (implicitly inline). Implementation-only helpers live in `namespace build::detail` (currently used in `backendninja.hpp` for the `Emitter`
class and the graph-traversal helpers `append_unique`, `resolve_alias`, `collect_includes`, `object_path`, and `glob_match` in `glob.hpp`).

There is no umbrella `build.hpp`. `build/build.cpp` includes the specific headers it needs.

---

## 2. Mental model

Three layers, each built from a small set of types.

### 2.1 Core (language-agnostic)

```text
build::Target         → graph node (name, deps, platform/config gating, ExtensionMap)
build::Project        → entry targets + registered platforms + registered configs
build::Platform       → environment identity (name, os, graphics_api, exe_suffix) + ExtensionMap
build::Configuration  → variant identity (name, out_dir) + ExtensionMap
```

The core types carry zero language vocabulary. They model "what to build", "where", and "how identities relate". All language-specific knowledge (compilers,
flags, defines) lives in extensions attached through `ExtensionMap`.

### 2.2 The C++ language module (`build::cxx`)

A parallel namespace mirroring the core, with each type acting as a fluent wrapper that owns its corresponding core type via `shared_ptr` and registers itself
as the cxx extension on that core type's `ExtensionMap`:

```text
build::cxx::Toolchain      → compiler / archiver / linker / default_std (tools only — composed inside cxx::Platform, not its own extension)
build::cxx::Platform       → wraps build::Platform; per-platform compile_flags / link_flags / defines / system_libs + a Toolchain
build::cxx::Configuration  → wraps build::Configuration; per-config compile_flags / link_flags / defines
build::cxx::Target         → wraps build::Target; one node per library/program; sources / includes / defines / std / link / etc.
build::cxx::ObjectFile     → wraps build::Target; one node per translation unit; per-TU defines / compile_flags / std / warning suppressions
```

A library or program is `cxx::Target`; each `.cpp` it owns is a `cxx::ObjectFile` child whose base is dep-edged to the parent's base. Compile edges
(`build .../foo.cpp.o: cxx ...`) are emitted from the ObjectFile node, archive/link edges from the parent. The framework graph is therefore one node per
TU plus one node per library/program — sources are first-class, not an opaque list inside the parent.

Adding another language is purely additive: a new `build::csharp::Platform`/`Configuration`/`Target` plus a backend dispatch branch, no edits to anything in
`build::`.

### 2.3 Generic auxiliary targets

```text
build::Tool   → fluent wrapper around build::Target; attached as extension; runs an opaque shell command (glslc, rm, clang-format, …)
build::Alias  → fluent wrapper around build::Target; attached as extension; resolves to another target based on (platform, config) selectors
```

`Tool` and `Alias` are language-agnostic. They use the same wrapper-attached-as-extension pattern as the cxx types. The backend dispatches by checking
`target->extension<Tool>()` / `target->extension<Alias>()`.

---

## 3. Bootstrap chain

```text
build/bootstrap.ninja
  → _out/ngen-build           (compiled from build/bootstrap.cpp)
  → _out/ngen-build-pre       (compiled from build/prebuild.cpp)
  → _out/ngen-build-graph     (compiled from build/build.cpp; framework headers tracked via -MMD depfile)
  → _out/build.ninja          (emitted by ngen-build-graph)
  → final ninja invocation
```

| Stage manifest                | Source                                                         | Builds                  |
| ----------------------------- | -------------------------------------------------------------- | ----------------------- |
| `build/bootstrap.ninja`       | `build/bootstrap.cpp`                                          | `_out/ngen-build`       |
| `_out/ngen-build-pre.ninja`   | `build/prebuild.cpp`                                           | `_out/ngen-build-pre`   |
| `_out/ngen-build-graph.ninja` | `build/build.cpp` (framework headers tracked via depfile)      | `_out/ngen-build-graph` |
| `_out/build.ninja`            | (emitted by `ngen-build-graph`)                                | the project graph       |

`bootstrap.cpp` and `prebuild.cpp` each carry their own copy of `write_if_changed` and minimal arg parsing — deliberately, so they don't depend on the
framework (the framework can't be linked until stage 3 has built it).

The `ngen-build-graph` stage uses `-MMD -MF $out.d`, so any new `build/framework/*.hpp` automatically becomes a build dependency — no manual heredoc updates
required when adding framework headers.

Both `bootstrap.cpp` and `prebuild.cpp` shell out to `ninja` via `std::system`, marked with `// NOLINT(bugprone-command-processor)` since this is the
deliberate orchestration boundary.

---

## 4. ExtensionMap (`build/framework/extensionmap.hpp`)

Type-erased map keyed by `std::type_index(typeid(Ext))`. Two attachment modes:

- `add<Ext>(args...)` — owning. `ExtensionMap` heap-allocates the extension and deletes it on map destruction. Idempotent: re-calling `add<T>` returns the
  existing instance instead of replacing.
- `attach<Ext>(ext)` — non-owning. The map stores a back-pointer with a no-op deleter. Replaces any existing entry of the same type.

`get<Ext>() -> Ext*` returns `nullptr` when the extension is absent — no exceptions. Exceptions are forbidden in this codebase; `<stdexcept>` is not included
anywhere in the framework. The const overload returns `const Ext*`. Use `has<Ext>() -> bool` or just check the pointer.

`build::Target::extension<Ext>() const -> Ext*` is a thin wrapper around `extensions().get<Ext>()` (with a `const_cast` to keep the historical API shape).

---

## 5. Core API surface

### `build::Target`

Methods: `name`, `depend_on`, `only_on` / `except_on` / `only_in` / `except_in`, `enabled_for`, `extensions()`, plus the legacy `register_extension<T>` /
`extension<T>()` / `has_extension<T>()` for code that prefers that pattern.

### `build::Platform`

Fluent setters: `os(...)`, `graphics_api(...)`, `exe_suffix(...)`. Plus `name()` accessor and `extensions()`. No language-specific fields.

### `build::Configuration`

Fluent setter: `out_dir(...)`. Plus `name()` accessor and `extensions()`. No language-specific fields.

### `build::Project`

Registration: `target(Target&)`, `default_target(Target&)`, `platform(Platform&)`, `config(Configuration&)`. All idempotent — passing the same reference twice
is a no-op.

Lookup: `find_platform(name)`, `find_config(name)`, `find(name)` (entry targets only).

Build-set computation: `build(name)`, `build_all()`, `default_build()` — all return a post-order vector of `Target*`.

`platforms() / configs() / roots()` return `const std::vector<X*>&` — borrowed pointers, insertion order. The Project does not own the targets/platforms/configs;
it just records pointers to user-owned objects (typically locals in `main()`).

---

## 6. C++ extension surface (`build::cxx`)

Every `cxx::*` wrapper follows the same shape: it owns its base via `std::shared_ptr<build::*>`, exposes the base's setters via delegation, plus its own
language-specific setters. Move and copy constructors re-attach `*this` to the base's `ExtensionMap` so the back-pointer always reflects the current wrapper.
`shared_ptr` lets the chained-construction idiom work — `auto x = cxx::factory("name").a().b()` copy-constructs `x` from the materialized prvalue and the new
copy re-attaches.

### `cxx::Toolchain`

Tools only: `compiler(...)`, `archiver(...)`, `linker(...)`, `default_std(...)`. Composed inside `cxx::Platform`, not a separate extension.

Free factory: `cxx::toolchain()` returns a fresh `Toolchain` by value.

### `cxx::Platform`

Fluent: `os/graphics_api/exe_suffix` (delegated to base), `compile_flag` / `link_flag` / `define` / `system_lib` (singular, takes one string), and the plural
variants `compile_flags` / `link_flags` / `defines` / `system_libs` (each takes a `std::vector<std::string>` and appends all). Plus `toolchain()` returning the
composed `Toolchain&` and `toolchain(Toolchain)` setting the entire toolchain in one call.

Free factory: `cxx::platform(name)` returns a `cxx::Platform` by value.

Lookup helper: `cxx::find_platform(const build::Platform&) -> const Platform*` (nullable).

### `cxx::Configuration`

Fluent: `out_dir(...)` (delegated), `compile_flag` / `link_flag` / `define` (singular), and `compile_flags` / `link_flags` / `defines` (plural, take vectors).

Free factory: `cxx::configuration(name)`.

Lookup helper: `cxx::find_configuration(const build::Configuration&) -> const Configuration*` (nullable).

### `cxx::Target`

Methods:

- **Sources / std**: `sources({...})`, `std("c++20")`. Each path passed to `sources(...)` materializes a `cxx::ObjectFile` child stored in
  `objects_data` and dep-edged to the parent's base; sources are not an opaque list. `std(...)` sets the library/program-wide default that ObjectFiles
  inherit unless they set their own `std(...)`.
- **Per-TU access**: `for_source(path, fn)` looks up the `ObjectFile` whose source matches `path` and runs `fn(ObjectFile&)`. A miss is a configuration
  error and aborts with a clear message naming the missing source and the parent target.
- **Includes**: `include(path | vec | init_list)`, `public_include(...)` (propagates to dependents), `warning_off("name")`.
- **Defines**: `define("FOO=1")`, `defines(vec)` (bulk).
- **Compile flags**: `compile_flag(string)`, `compile_flags(vec)`. Plus typed sugar `optimize(O3)`, `debug(true)`, `pic(true)` — all desugar at method-call time
  into entries in `compile_flags_data`.
- **Link**: `link(other_cxx_target)`, `link("system_lib")`, `link_flag(string)`, `link_flags(vec)`, `system_libs(vec)`, `lib_search(path)`, `rpath(path)`.
- **Gating**: `only_on / except_on / only_in / except_in` (delegated to base).
- **Manual graph edge**: `depend_on(build::Target&)`.

Factory functions: `cxx::static_library(name)`, `cxx::shared_library(name)`, `cxx::program(name)` — each constructs a `cxx::Target` with the appropriate
`Kind`. The `OptLevel` and `Kind` enums live at the top of `cxx/target.hpp`.

### `cxx::ObjectFile`

One node per translation unit. Constructed implicitly by `cxx::Target::sources(...)` — there is no public factory and there is no fluent surface on the
parent for "add this single source"; everything goes through `sources(...)`.

Identity: the underlying `build::Target` is named `<parent-target-name>/<source-path>` (e.g. `renderer/src/renderer/renderthread.cpp`). This naming
guarantees uniqueness even when the same source is compiled into two libraries (each gets its own ObjectFile with its own object output).

Lifetime: held by `std::shared_ptr` in `cxx::Target::objects_data`. The parent owns the children; the framework's `Project` only sees the underlying
`build::Target*` via dep edges. ObjectFile is non-copyable and non-movable — only the `shared_ptr` moves.

Inheritance: ObjectFile holds a `shared_ptr<build::Target>` back to its parent's base and resolves the live `cxx::Target` extension through the base's
`ExtensionMap` whenever needed. This keeps the back-pointer correct across the parent's copy/move into its final home.

Gating: an ObjectFile is enabled exactly when its parent is. The check is performed in `emit_object_file` against the parent — ObjectFile's own base has
no `only_on/except_on` sets.

Per-TU fluent surface (mirrors a subset of `cxx::Target`):

- `define(string)`, `defines(vec)`
- `warning_off(string_view)`
- `compile_flag(string)`, `compile_flags(vec)`
- `std(string_view)` — overrides parent + toolchain default for this TU only

Use via `for_source` on the parent:

```cpp
auto sceneusd =
    cxx::static_library("sceneusd")
        .sources(glob({.include = "src/scene/usd*.cpp"}))
        .for_source("src/scene/usdscene.cpp", [](cxx::ObjectFile& obj) {
            obj.warning_off("deprecated-declarations").std("c++20");
        });
```

Library- and per-TU surfaces coexist. A library-wide `cxx::Target::define("FOO=1")` and a per-TU `obj.define("BAR=1")` both end up on the targeted TU's
command line; siblings only get `FOO=1`. For `std`, the ObjectFile value wins if set, otherwise the parent's, otherwise the toolchain default.

There is no `optimize/debug/pic` per-TU sugar by design — those live at platform/config level. Use `compile_flag("-O3")` directly if you need it on a
single TU.

---

## 7. Tool and Alias

`Tool` and `Alias` mirror the cxx wrappers' pattern: each owns a `shared_ptr<Target>`, attaches itself to the base's `ExtensionMap`, and offers a fluent
factory + chain.

### `Tool`

Free factory: `tool(name)` returns a `Tool` by value. Chained: `command({...})`, `inputs(...)`, `outputs(...)`, `for_each(paths, fn)`, `global(bool)`. The
`for_each` form lets a tool emit one ninja edge per input, with the output path computed by the supplied callable. `global(true)` produces a single global
phony edge instead of per-variant edges (used for `clean` / `format` / `tidy`).

### `Alias`

Free factory: `alias(name)`. Chained: `select("platform", "linux-vulkan", target)`, `to(target)`, `fallback(target)`, `resolve(context)`. Used for graph-level
indirection that depends on `(platform, config)` — e.g. `rhi-backend` resolves to `rhivulkan` on `linux-vulkan`.

The backend dispatches `Tool` and `Alias` via `target->extension<Tool>()` / `target->extension<Alias>()` — same mechanism as `cxx::Target`. There is no
`dynamic_cast` in the dispatch path.

---

## 8. Ninja backend behavior

`NinjaBackend::emit(project)` writes `_out/build.ninja`, all per-variant `compile_commands.json`, the merged `_out/compile_commands.json`, and creates required
output directories.

`detail::Emitter::emit_target` resolves aliases (walking `target->extension<Alias>()` chains), then dispatches by extension type:

```cpp
if      (auto* tool = target->extension<Tool>())              { emit_tool(*tool, ...); }
else if (auto* obj  = target->extension<cxx::ObjectFile>())   { emit_object_file(*obj, variant); }
else if (auto* cxx_t = target->extension<cxx::Target>())      { emit_cxx(*cxx_t, variant, order_only); }
```

ObjectFile is checked before `cxx::Target` so a TU node never falls through to library/program emit. The dep-walk preceding dispatch filters out
`cxx::ObjectFile` outputs from the `order_only` accumulator — those object paths flow into the parent's archive/link edge as direct inputs via
`gather_object_outputs`, so adding them to `order_only` would just double-list them.

`emit_object_file(ObjectFile&, BuildVariant&)` emits one `build .../foo.cpp.o: cxx src/.../foo.cpp` edge. It reads:

- `cxx::find_platform(*variant.platform)` — required; emit error if absent or `compiler()` is empty.
- `cxx::find_configuration(*variant.config)` — optional; if absent, no per-config flags/defines.
- `obj.parent()` — required; resolved live through the parent's `ExtensionMap`. If parent is disabled for the variant, returns `Path{}` (no edge).

`gather_object_outputs(cxx::Target&, BuildVariant&)` is what `emit_cxx_library` and `emit_cxx_program` call to assemble the parent's input list — it
looks up each child ObjectFile in the emitter's `outputs_` cache (keyed by `name|platform|config`). Children are always emitted before their parent
because `Project::build_all()` is post-order and `emit_target` recurses into deps before emitting self.

Compile-flag composition order (compiler last-wins picks innermost):

1. `cxx::Platform::compile_flags()`
2. `cxx::Configuration::compile_flags()`
3. `cxx::Target::compile_flags_data` (raw + desugared `optimize` / `debug` / `pic`) — the parent library/program
4. `cxx::ObjectFile::compile_flags_data` — the per-TU layer

Defines follow the same four-step precedence (platform → config → parent → ObjectFile). Warning suppressions are appended in the same order. `std`
resolves as: ObjectFile's `std_data` if set, else parent's `std_data`, else toolchain `default_std()`. Includes come from `cxx::Target::includes_data`
plus transitive `public_includes_data` from linked targets — ObjectFile inherits the include set from its parent verbatim and does not currently extend
it. Link flags and system libs are parent-level concerns; ObjectFile has no link-side fields.

`-Wl,--start-group` / `-Wl,--end-group` wraps archive inputs at link time so over-linking still works without the user having to curate transitive link order.

---

## 9. Glob matching

`build::detail::glob_match` is a hand-rolled recursive matcher; `std::regex` is not used (it can throw, and exceptions are forbidden). Supported syntax:

- `*` — any sequence of non-`/` characters within a single path segment
- `**` — any sequence including `/`
- `**/` — zero or more path segments followed by `/`
- `?` — any single non-`/` character
- All other characters match literally

`\` is normalized to `/` before matching.

---

## 10. The ngen graph (in `build/build.cpp`)

One platform: `linux-vulkan` with `clang++` / `ar`, default `c++23`, defines `NGEN_PLATFORM_LINUX, NGEN_GFX_VULKAN, GLM_FORCE_RADIANS,
GLM_FORCE_DEPTH_ZERO_TO_ONE`, compile flags `-fPIC -Wall` plus `pkg-config --cflags sdl3` tokens, system libs `vulkan, m`.

Three configs:

| Config         | Compile flags               | Link flags                            | Defines              |
| -------------- | --------------------------- | ------------------------------------- | -------------------- |
| `debug`        | `-O0 -g`                    | —                                     | `DEBUG=1`            |
| `release`      | `-O2 -g`                    | —                                     | `NDEBUG`             |
| `gamerelease`  | `-O3 -fvisibility=hidden`   | `-flto -Wl,-s -Wl,--gc-sections`      | `NDEBUG, SHIPPING=1` |

Targets:

| Name          | Type             | Notes                                                                                       |
| ------------- | ---------------- | ------------------------------------------------------------------------------------------- |
| `obs`         | `static_library` | `src/obs/**/*.cpp`; public `src/obs`, `external/concurrentqueue`                            |
| `rhi`         | `static_library` | `src/rhi/*.cpp`; private `external/imgui`                                                   |
| `rhivulkan`   | `static_library` | `src/rhi/vulkan/**/*.cpp`; `only_on({"linux-vulkan"})`; links `rhi`                         |
| `rhi-backend` | `Alias`          | `select("platform", "linux-vulkan", rhivulkan)`                                             |
| `renderer`    | `static_library` | links `obs`, `rhi`, `rhi-backend`                                                           |
| `scene`       | `static_library` | excludes `src/scene/usd*.cpp`                                                               |
| `sceneusd`    | `static_library` | `c++20`, `-Wno-deprecated-declarations`, broad include set                                  |
| `imgui`       | `static_library` | explicit 7-source list (core + sdl3 + vulkan backends)                                      |
| `ui`          | `static_library` | links `renderer`, `scene`, `sceneusd`, `imgui`                                              |
| `shaders`     | `Tool`           | `glslc $in -o $out`; `for_each` over `*.vert` + `*.frag` → `<out_dir>/shaders/<name>.spv`   |
| `clean`       | `Tool`           | `rm -rf $out_dir`                                                                           |
| `format`      | `Tool` (global)  | `clang-format -i` over src + build trees                                                    |
| `tidy`        | `Tool` (global)  | `clang-tidy ... -std=c++23 -Ibuild/framework`                                               |
| `ngen-view`   | `program`        | `main.cpp`, `camera.cpp`, `debugdraw.cpp`, `jobsystem.cpp`; default target                  |

`ngen-view` over-links via `link(...)` to every internal library. The `--start-group` wrap absorbs order-sensitivity at link time.

USD linkage uses an absolute rpath via `current_path() / "external/openusd_build/lib"`.

---

## 11. CLI

`./_out/ngen-build [--platform <name>] [--config <name>] [--backend ninja] [-v|-vv] [target]`

Special targets routed by `bootstrap.cpp` *without* a `:platform:config` suffix: `clean`, `format`, `tidy`. Others become positional targets.

Graph targets: `ngen-view` (default), `clean`, `format`, `tidy`, `shaders`. Internal libraries (`obs`, `rhi`, …) are not top-level invokable — they're reached
via traversal from registered entry points.

Verbosity:
- default: clean ninja output
- `-v`: `TERM=dumb` (forces non-tty output for scripts/log capture)
- `-vv`: `ninja -v` (full command echo)

---

## 12. Adding a platform or configuration

Use the cxx factories and register with the project:

```cpp
auto clang = cxx::toolchain()
    .compiler("clang++")
    .archiver("ar")
    .default_std("c++23");

auto linux_vulkan = cxx::platform("linux-vulkan")
    .os("linux")
    .graphics_api("vulkan")
    .toolchain(clang)
    .compile_flag("-fPIC")
    .define("NGEN_PLATFORM_LINUX")
    .system_lib("vulkan");

auto debug = cxx::configuration("debug")
    .out_dir("_out")
    .compile_flag("-O0")
    .compile_flag("-g")
    .define("DEBUG=1");

Project p;
p.platform(linux_vulkan);
p.config(debug);
```

To add a new platform, construct another `cxx::platform("name")` chain and register it. Same for configurations. No designated initializers, no settings
structs, no string-keyed lookup at construction time.

---

## 13. Things to know before changing this code

- **Header-only framework.** Editing any `build/framework/*.hpp` rebuilds `_out/ngen-build-graph` (the single TU that includes them). Editing engine sources
  under `src/` does not rebuild build-system stages — the bootstrap chain is independent.
- **Depfile-tracked headers.** `prebuild.cpp` writes `_out/ngen-build-graph.ninja` with `-MMD -MF $out.d`. Adding a new `build/framework/*.hpp` needs no manual
  heredoc update — the depfile picks it up.
- **Wrapper move/copy invariant.** Every cxx wrapper (and `Tool`, `Alias`) re-attaches itself to the base's `ExtensionMap` in both move and copy constructors.
  If a future field is added, both constructors must be updated. Silent footgun otherwise — the back-pointer would point at a stale or destroyed object.
  `cxx::ObjectFile` is the exception — it lives behind `shared_ptr` from the moment `sources(...)` constructs it, never gets copied or moved by user code,
  and is `=delete`d for both. Don't try to value-store an ObjectFile.
- **Per-TU graph nodes.** Each `.cpp` is its own `build::Target` (with a `cxx::ObjectFile` extension) registered as a dep of its parent library/program.
  This is the *only* place in the framework where dep edges and the user-visible "what to address" surface diverge: ObjectFile names like
  `renderer/src/renderer/foo.cpp` are unique build edges in the graph but are not surfaced via `Project::roots()`. If anything walks
  `Project::build_all()` looking only for "real" libraries, filter on `extension<cxx::ObjectFile>()` first.
- **`ExtensionMap` ownership modes.** `add<T>` heap-allocates and owns; `attach<T>(ref)` is non-owning. Don't mix on the same key — `add<T>` returns existing,
  `attach` replaces. The cxx wrappers all use `attach` (they own their data themselves).
- **No exceptions.** `ExtensionMap::get` returns nullable pointers; `glob_match` doesn't use `std::regex`. The convention is `std::expected<T, Error>` at the
  framework boundary (`Error` lives in `glob.hpp`).
- **`compile_commands.json`** is written per variant under `_out/<platform>/<config>/compile_commands.json` *and* merged at `_out/compile_commands.json`. The
  merge is naive concatenation; entries are not de-duplicated across variants.
- **`-Wl,--start-group` / `--end-group`** wraps every program's archives. Removing the group wrapping would require curating transitive link order first.
- **`capture_tokens` uses `popen`.** `shell_quote` is tuned for a small allowed charset. Not safe for arbitrary user input — fine for fixed args like
  `pkg-config --cflags sdl3`.
- **`std::system` calls** in `bootstrap.cpp` and `prebuild.cpp` are deliberate (they shell out to `ninja`) and marked with `// NOLINT(bugprone-command-processor)`.

---

## 14. Adding another language

The framework is designed so that adding a new language module is purely additive:

1. Create `build/framework/<lang>/{toolchain,platform,configuration,target,backendninja}.hpp`.
2. Each follows the same pattern as the cxx files: a wrapper that owns a `shared_ptr` to the corresponding `build::*` base, attaches itself as an extension,
   and exposes a fluent surface.
3. Add a free factory `<lang>::<lang>(name)` for the language target equivalent (`cxx::program`, `cxx::static_library`, …).
4. Add a backend dispatch branch in `build/framework/backendninja.hpp`'s `emit_target`:

   ```cpp
   else if (auto* x = target->extension<lang::Target>()) { lang::ninja::emit(*x, ...); }
   ```

No edits to `build::Target`, `build::Project`, `build::Platform`, or `build::Configuration` are required.
