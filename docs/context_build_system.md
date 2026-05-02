# Build System v4 — Implementation Context

Companion to `docs/plan_build_system_v4.md`. The plan describes the design; this document records what's actually in the tree on `build-system-v2` after v4 landed,
so a future session can orient quickly without re-deriving everything from source.

Last refreshed after v4 (parallel `cxx::` mirror of core types).

---

## 1. Files on disk

```text
build/
  bootstrap.ninja              # 12 lines, hand-written seed
  bootstrap.cpp                # ngen-build orchestrator (stage 1)
  prebuild.cpp                 # ngen-build-pre (stage 2)
  build.cpp                    # project graph; compiled into ngen-build-graph (stage 3)
  framework/                   # header-only library, no .cpp files, no umbrella
    alias.hpp                  # Alias (selectable target indirection)
    backend.hpp                # BuildVariant + forward decls of Platform/Configuration
    backendninja.hpp           # detail::Emitter, NinjaBackend (writes _out/build.ninja)
    command.hpp                # Command (argv vector)
    configuration.hpp          # build::Configuration — fluent class with ExtensionMap
    extensionmap.hpp           # ExtensionMap (type-erased, owning + non-owning attach)
    flags.hpp                  # Error
    glob.hpp                   # GlobSpec, glob, concat, capture_tokens, repo_root, write_if_changed
    path.hpp                   # Path
    platform.hpp               # build::Platform — fluent class with ExtensionMap
    project.hpp                # Project — fluent platform()/config(), entry targets
    target.hpp                 # build::Target — identity + deps + gating + ExtensionMap
    tool.hpp                   # Tool (one-shot generic command target)
    toolchainhelpers.hpp       # shell_quote, join_command, ninja_escape_path, split_ws

    cxx/
      backendninja.hpp         # CompileInputs/LinkInputs + compile_/archive_/link_command
      configuration.hpp        # cxx::Configuration extension on build::Configuration
      flags.hpp                # OptLevel + opt_flag()
      platform.hpp             # cxx::Platform extension on build::Platform (composes Toolchain)
      target.hpp               # cxx::Target extension on build::Target (fluent wrapper)
      toolchain.hpp            # cxx::Toolchain — tools only (compiler / archiver / linker / default_std)
```

**Header-only.** Every public type and free function is defined in one of these headers. Free functions are marked `inline`; class methods are defined inside their
class bodies (implicitly inline). Implementation-only helpers live in `namespace build::detail` (currently used in `backendninja.hpp` for the `Emitter` class and
the graph-traversal helpers `append_unique`, `resolve_alias`, `collect_includes`, `object_path`).

There is no umbrella `build.hpp`. `build/build.cpp` includes the specific headers it needs.

---

## 2. Mental model

Three independent concerns:

```text
build::Target         → graph node (identity, deps, platform/config gating, ExtensionMap)
build::Project        → entry targets + platforms + configs
build::Platform       → environment identity (name, os, graphics_api, exe_suffix) + ExtensionMap
build::Configuration  → variant identity (name, out_dir) + ExtensionMap
```

The C++ side is a parallel namespace `build::cxx`:

```text
build::cxx::Toolchain      → compiler / archiver / linker / default_std (tools only)
build::cxx::Platform       → extension on build::Platform: composes Toolchain + per-platform compile_flags / link_flags / defines / system_libs
build::cxx::Configuration  → extension on build::Configuration: per-config compile_flags / link_flags / defines
build::cxx::Target         → extension on build::Target: per-target sources / includes / defines / std / link / etc.
```

Core types carry zero C++ vocabulary. Adding a language is additive: a new `build::csharp::Platform`/`Configuration`/`Target` plus a backend dispatch branch.

---

## 3. Bootstrap chain (unchanged from v2)

```text
build/bootstrap.ninja
  → _out/ngen-build           (compiled from build/bootstrap.cpp)
  → _out/ngen-build-pre       (compiled from build/prebuild.cpp)
  → _out/ngen-build-graph     (compiled from build/build.cpp; framework headers tracked via -MMD depfile)
  → _out/build.ninja          (emitted by ngen-build-graph)
  → final Ninja invocation
```

Stage entry points and their narrow dependency sets:

| Stage manifest                | Source                                                         | Builds                  |
| ----------------------------- | -------------------------------------------------------------- | ----------------------- |
| `build/bootstrap.ninja`       | `build/bootstrap.cpp`                                          | `_out/ngen-build`       |
| `_out/ngen-build-pre.ninja`   | `build/prebuild.cpp`                                           | `_out/ngen-build-pre`   |
| `_out/ngen-build-graph.ninja` | `build/build.cpp` (framework headers tracked via depfile)      | `_out/ngen-build-graph` |
| `_out/build.ninja`            | (emitted by `ngen-build-graph`)                                | the project graph       |

`bootstrap.cpp` and `prebuild.cpp` each carry their own copy of `write_if_changed` and minimal arg parsing — deliberately, so they don't depend on the framework
(the framework can't be linked until stage 3 has built it).

The `ngen-build-graph` stage uses `-MMD -MF $out.d` so any new `build/framework/*.hpp` automatically becomes a build dependency — no manual heredoc updates
required when adding framework headers.

---

## 4. ExtensionMap (`build/framework/extensionmap.hpp`)

Type-erased map keyed by `std::type_index(typeid(Ext))`. Two attachment modes:

- `add<Ext>(args...)` — owning. `ExtensionMap` heap-allocates the extension and deletes it on map destruction. Idempotent: re-calling `add<T>` returns the
  existing instance instead of replacing.
- `attach<Ext>(ext)` — non-owning. The map stores a back-pointer with a no-op deleter. Replaces any existing entry of the same type.

`get<T>()` throws if absent; check with `has<T>()` first or use the language-module `find_*(...)` accessors that return nullable.

`build::Target`'s `register_extension<T>(T&)` is now a thin wrapper around `extensions().attach(ext)` — used by `cxx::Target` to register its back-pointer. The
`extension<T>() -> T*` accessor returns nullable, which is what the backend dispatch uses.

---

## 5. Core API surface

### `build::Target`

Methods: `name`, `depend_on`, `only_on` / `except_on` / `only_in` / `except_in`, `enabled_for`, `extensions()`, plus the legacy-style `register_extension<T>` /
`extension<T>()` / `has_extension<T>()` for code that's used to that pattern.

`Tool` and `Alias` are `Target` subclasses, dispatched via `dynamic_cast` in the backend. They are language-agnostic by nature.

### `build::Platform`

Fluent setters: `os(...)`, `graphics_api(...)`, `exe_suffix(...)`. Plus `name()` accessor and `extensions()`. No language-specific fields.

### `build::Configuration`

Fluent setters: `out_dir(...)`. Plus `name()` accessor and `extensions()`. No language-specific fields.

### `build::Project`

Fluent creation: `platform(name) -> Platform&`, `config(name) -> Configuration&`. Both idempotent — re-calling with the same name returns the existing instance.
Storage is `vector<unique_ptr<Platform>>` / `vector<unique_ptr<Configuration>>` for stable addressing.

Entry-target API: `target(t)`, `default_target(t)`. Lookup: `find_platform`, `find_config`, `find` (for entry targets). Build-set computation: `build(name)`,
`build_all()`, `default_build()`.

`platforms() / configs()` return `std::vector<Platform*> / vector<Configuration*>` (borrowed pointers, sorted by insertion order).

---

## 6. C++ extension surface (`build::cxx`)

### `cxx::Toolchain`

Tools only: fluent `compiler(...)`, `archiver(...)`, `linker(...)`, `default_std(...)`. No flags. Composed inside `cxx::Platform` (not a standalone extension on
`build::Platform`).

### `cxx::Platform`

Fluent: `compile_flag(...)`, `link_flag(...)`, `define(...)`, `system_lib(...)`. Plus `toolchain()` returning the composed `Toolchain&`.

Accessors:
- `cxx::platform(build::Platform&)` — lazy-create + return reference.
- `cxx::find_platform(const build::Platform&)` — nullable.

### `cxx::Configuration`

Fluent: `compile_flag(...)`, `link_flag(...)`, `define(...)`. No system_lib (those are platform-level in ngen).

Accessors:
- `cxx::configuration(build::Configuration&)` — lazy-create + return reference.
- `cxx::find_configuration(const build::Configuration&)` — nullable.

### `cxx::Target`

Fluent wrapper that owns `unique_ptr<build::Target>` and registers itself in the base's `ExtensionMap` via `attach`. Move ctor re-attaches.

Methods:
- Sources: `sources({...})`, `std("c++20")`.
- Includes: `include(path)`, `public_include(path)` (propagates to dependents), `warning_off("name")`.
- Defines: `define("FOO=1")`.
- Compile: `compile_flag("-fPIC")`, plus typed sugar `optimize(O3)`, `debug(true)`, `pic(true)` — all desugar to `compile_flag` calls at method-call time.
- Link: `link(other_cxx_target)`, `link("system_lib")`, `link_flag("-flto")`, `link_flags({...})`, `lib_search(path)`, `rpath(path)`.
- Gating: `only_on/except_on/only_in/except_in` (delegated to base).
- Manual graph edge: `depend_on(build::Target&)`.

Factory functions: `cxx::static_library(name)`, `cxx::shared_library(name)`, `cxx::program(name)` — return `cxx::Target` by value (move-into).

---

## 7. Ninja backend behavior

`NinjaBackend::emit(project)` writes `_out/build.ninja`, all per-variant `compile_commands.json`, the merged `_out/compile_commands.json`, and materializes
required output directories.

`detail::Emitter::emit_cxx_objects` reads:

- `cxx::find_platform(*variant.platform)` — required; emit error if absent or `compiler()` is empty.
- `cxx::find_configuration(*variant.config)` — optional; if absent, no per-config flags/defines.
- `cxx::Target` extension on the target itself.

Compile-flag composition order (from outer to inner — compiler last-wins picks innermost):
1. `cxx::Platform::compile_flags()`
2. `cxx::Configuration::compile_flags()`
3. `cxx::Target::compile_flags_data` (raw + desugared from `optimize`/`debug`/`pic`)

Defines and link flags follow the same precedence. Includes come from `cxx::Target::includes_data` plus transitive `public_includes_data` from linked targets.
System libs come from `cxx::Platform::system_libs()` + `cxx::Target::system_libs_data`.

`-Wl,--start-group` / `-Wl,--end-group` wraps archive inputs at link time so over-linking still works (per v3).

---

## 8. The ngen graph (in `build/build.cpp`)

One platform: `linux-vulkan` with `clang++` / `ar`, default `c++23`, defines `NGEN_PLATFORM_LINUX, NGEN_GFX_VULKAN, GLM_FORCE_RADIANS, GLM_FORCE_DEPTH_ZERO_TO_ONE`,
compile flags `-fPIC -Wall` plus `pkg-config --cflags sdl3` tokens, system libs `vulkan, m`.

Three configs:

| Config         | Compile flags               | Link flags                            | Defines              |
| -------------- | --------------------------- | ------------------------------------- | -------------------- |
| `debug`        | `-O0 -g`                    | —                                     | `DEBUG=1`            |
| `release`      | `-O2 -g`                    | —                                     | `NDEBUG`             |
| `gamerelease`  | `-O3 -fvisibility=hidden`   | `-flto -Wl,-s -Wl,--gc-sections`      | `NDEBUG, SHIPPING=1` |

Targets:

| Name          | Type            | Notes                                                                                       |
| ------------- | --------------- | ------------------------------------------------------------------------------------------- |
| `obs`         | `static_library`| `src/obs/**/*.cpp`; public `src/obs`, `external/concurrentqueue`                            |
| `rhi`         | `static_library`| `src/rhi/*.cpp`; private `external/imgui`                                                   |
| `rhivulkan`   | `static_library`| `src/rhi/vulkan/**/*.cpp`; `only_on({"linux-vulkan"})`; links `rhi`                         |
| `rhi-backend` | `Alias`         | `select("platform", "linux-vulkan", rhivulkan)`                                             |
| `renderer`    | `static_library`| links `obs`, `rhi`, `rhi-backend`                                                           |
| `scene`       | `static_library`| excludes `src/scene/usd*.cpp`                                                               |
| `sceneusd`    | `static_library`| `c++20`, `-Wno-deprecated-declarations`, broad include set                                  |
| `imgui`       | `static_library`| explicit 7-source list (core + sdl3 + vulkan backends)                                      |
| `ui`          | `static_library`| links `renderer`, `scene`, `sceneusd`, `imgui`                                              |
| `shaders`     | `Tool`          | `glslc $in -o $out`; `for_each` over `*.vert` + `*.frag` → `<out_dir>/shaders/<name>.spv`   |
| `clean`       | `Tool`          | `rm -rf $out_dir`                                                                           |
| `format`      | `Tool` (global) | `clang-format -i` over src + build trees                                                    |
| `tidy`        | `Tool` (global) | `clang-tidy ... -std=c++23 -Ibuild/framework`                                               |
| `ngen-view`   | `program`       | `main.cpp`, `camera.cpp`, `debugdraw.cpp`, `jobsystem.cpp`; default target                  |

`ngen-view` over-links via `link(...)` to every internal library. The `--start-group` wrap absorbs order-sensitivity at link time.

USD linkage uses absolute rpath via `current_path() / "external/openusd_build/lib"`.

---

## 9. CLI

`./_out/ngen-build [--platform <name>] [--config <name>] [--backend ninja] [-v|-vv] [target]`

Special targets routed by `bootstrap.cpp` *without* `:platform:config` suffix: `clean`, `format`, `tidy`. Others become positional targets.

Graph targets: `ngen-view` (default), `clean`, `format`, `tidy`, `shaders`. Per the plan, internal libraries (`obs`, `rhi`, …) are no longer top-level invokable —
they're reached via traversal from the registered entry points.

---

## 10. Things to know before changing this code

- **Header-only framework.** Editing any `build/framework/*.hpp` rebuilds `_out/ngen-build-graph` (the single TU that includes them). Editing engine sources under
  `src/` does not rebuild build-system stages.
- **Depfile-tracked headers.** `prebuild.cpp` writes the `_out/ngen-build-graph.ninja` ninja file with `-MMD -MF $out.d`. Adding a new `build/framework/*.hpp`
  needs no manual heredoc update — the depfile picks it up after the first build.
- **`cxx::Target` move ctor invariant.** When `cxx::Target` is moved (e.g. returned from `cxx::static_library(...)` and then stored in a local), the move ctor
  must call `base_->extensions().attach(*this)` to update the back-pointer in the base. If a future field is added to `cxx::Target`, the move ctor must be
  updated too — silent footgun otherwise.
- **`ExtensionMap` ownership modes.** `add<T>` heap-allocates and owns; `attach<T>(ref)` is non-owning. Don't mix on the same key — re-`add` returns existing,
  `attach` replaces.
- **Adding a config or platform is fluent.** `p.config("foo").out_dir(...)` plus `cxx::configuration(*p.find_config("foo")).compile_flag(...)` etc. No
  designated initializers; the structs are gone.
- **`compile_commands.json`** is written per variant under `_out/<platform>/<config>/compile_commands.json` *and* merged at `_out/compile_commands.json`.
  Naive concatenation; entries are not de-duplicated across variants.
- **`-Wl,--start-group`/`--end-group`** wraps every program's archives. Removing the group wrapping would force proper transitive link order to be solved first.
- **`capture_tokens` uses `popen`.** Internal `shell_quote` tuned for a small allowed charset. Not safe for arbitrary user input — fine for fixed args like
  `pkg-config --cflags sdl3`.

---

## 11. What v4 changed from v3

- `build::Platform` is now a class with fluent setters and an `ExtensionMap`. The `cxx` field and `cxx::PlatformSettings` struct are gone.
- `build::Configuration` is now a class with fluent setters and an `ExtensionMap`. The `cxx` field and `cxx::ConfigurationSettings` struct are gone.
- `build::Project::add_platform/add_config(value)` are gone. Use fluent `platform(name)` / `config(name)`.
- `cxx::Toolchain` shrunk to tools-only (compiler / archiver / linker / default_std). Per-platform flags moved to `cxx::Platform`.
- `cxx::Target` raw-flag method names: `flag_raw` → `compile_flag`, `link_raw` → `link_flag`, `link_raw_many` → `link_flags`.
- `cxx::Target` typed sugar (`optimize`, `debug`, `pic`) now desugars at method-call time into `compile_flags_data`. The `opt_data` / `debug_info_data` /
  `pic_data` fields are gone — there is one ordered list of compile flags per target.
- `cxx::Linkage` enum is gone. `cxx::OptLevel` and `opt_flag()` moved into `cxx/flags.hpp`.
- `ExtensionMap` is the new type-erased extension storage, with explicit owning (`add`) and non-owning (`attach`) modes. `build::Target` uses `attach` for the
  cxx back-pointer; future per-platform extensions like `cxx::Platform` use `add` for owned data.
