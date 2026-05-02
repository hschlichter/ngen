# Plan v3 — Target / Project / Extensions

This document supersedes `docs/plan_build_system_v2.md` for the *structure* of the build description. It does not replace v2's bootstrap-chain or
Ninja-backend-output sections — those carry over largely unchanged. v3 reshapes how targets, the project, and language behavior are organized; the orchestrator
binaries (`ngen-build` / `ngen-build-pre` / `ngen-build-graph`), the four-stage build chain, and the on-disk shape of `_out/build.ninja` stay the same.

---

## 1. Mental Model

```text
Targets     → graph structure (identity + deps)
Project     → build context (platforms, configs) + entry points
Extensions  → build behavior (per-language)
```

Three independent concerns. Each has its own type, its own ownership, and its own visibility. `Target` knows nothing about platforms or compilers. `Project` knows
nothing about C++. Extensions know nothing about the graph mechanics outside of their own data.

The previous design conflated all three into `Graph`, `Target`, `Library`, `Program`, `CxxToolchain`, etc., and leaked C++ specifics throughout. v3 pulls them
apart.

---

## 2. Goals

**Genuine language-agnosticism in the framework core.** No `sources`, no `defines`, no `compile`, no `link` anywhere in `build::Target` / `build::Platform` /
`build::Configuration`. Those concepts belong to language modules.

**Adding a language is purely additive.** A new language module (`build::csharp`, `build::go`, …) is a new namespace, new wrapper type, new `*Settings` struct,
new ninja emitter, plus one branch in the backend dispatch. Existing code is not edited.

**Internal libraries don't pollute the user-facing surface.** Targets that exist only to support an entry target are not registered with `Project`. They're
discovered by traversal from the entry points. This shrinks what users (and the orchestrator) see by name to what they actually invoke.

**Extension data lives where the target lives.** A wrapper holds the base `Target` via `unique_ptr` and registers itself in the base's small extension map. The
backend, given a `build::Target*` from graph traversal, can look up the extension via `target->extension<cxx::Target>()`. No side tables, no global lookups.

**The `Toolchain` design problem stays bounded.** Whatever shape `cxx::Toolchain` ultimately takes (closures, data struct, something else) lives entirely
inside `build/framework/cxx/`. It can't pollute the framework core.

---

## 3. Repository Layout

```text
build/
  bootstrap.ninja              # unchanged from v2
  bootstrap.cpp                # unchanged from v2
  prebuild.cpp                 # unchanged from v2
  build.cpp                    # rewritten against v3 API
  framework/
    target.hpp                 # build::Target — identity + deps + conditionals + extension map
    project.hpp                # build::Project — entry targets + context, replaces Graph
    platform.hpp               # build::Platform — name/os/gfx + composed *Settings
    configuration.hpp          # build::Configuration — name/out_dir + composed *Settings
    alias.hpp                  # build::Alias — generic alias target
    tool.hpp                   # build::Tool — generic one-shot command target
    backendninja.hpp           # generic Ninja orchestration (variant loop, rules, phonies, dispatch)

    cxx/
      target.hpp               # build::cxx::Target — C++ wrapper, fluent API, Kind enum
      toolchain.hpp            # build::cxx::Toolchain — shape TBD (see §10)
      platform.hpp             # build::cxx::PlatformSettings
      configuration.hpp        # build::cxx::ConfigurationSettings
      backendninja.hpp         # build::cxx::ninja::emit — translates cxx::Target into ninja edges
```

`build::Graph`, `build::Library`, `build::Program`, `build::StaticLibrary`, `build::SharedLibrary`, `build::CxxToolchain`, and the inheritance-based target
hierarchy from v2 all go away.

---

## 4. Core Types (Framework)

### 4.1 `build::Target`

The graph node. Owns its identity, its outgoing dependencies, and a small typed extension map. Public data is the dep list; everything else is private with accessors.

```cpp
namespace build {

class Target {
public:
    explicit Target(std::string name);

    auto name() const -> const std::string&;

    auto depend_on(Target& other) -> Target&;

    auto only_on(std::initializer_list<std::string_view>) -> Target&;
    auto except_on(std::initializer_list<std::string_view>) -> Target&;
    auto only_in(std::initializer_list<std::string_view>) -> Target&;
    auto except_in(std::initializer_list<std::string_view>) -> Target&;
    auto enabled_for(std::string_view platform, std::string_view config) const -> bool;

    template <typename Ext>
    auto register_extension(Ext& ext) -> void;

    template <typename Ext>
    auto extension() const -> Ext*;

    template <typename Ext>
    auto has_extension() const -> bool;

    std::vector<Target*> deps;

private:
    std::string name_;
    std::unordered_map<std::type_index, void*> extensions_;
    // platform/config inclusion sets
};

}
```

`extensions_` stores raw `void*` back-pointers. The wrapper (e.g. `cxx::Target`) owns the actual data; the base just holds the pointer so the backend can find
it. Lifetime is tied to the wrapper.

### 4.2 `build::Project`

Replaces `Graph`. Holds platforms, configurations, and *entry targets only*. The dependency graph is distributed across `Target::deps` and discovered by walking
from entry targets.

```cpp
namespace build {

class Project {
public:
    auto target(Target& t) -> void;
    auto default_target(Target& t) -> void;

    auto add_platform(Platform p) -> void;
    auto add_config(Configuration c) -> void;

    auto find(std::string_view name) const -> Target*;
    auto build(std::string_view name) const -> std::vector<Target*>;
    auto build_all() const -> std::vector<Target*>;
    auto default_build() const -> std::vector<Target*>;

    auto platforms() const -> const std::vector<Platform>&;
    auto configs() const -> const std::vector<Configuration>&;

private:
    std::vector<Target*> roots_;
    Target* default_ = nullptr;
    std::vector<Platform> platforms_;
    std::vector<Configuration> configs_;
};

}
```

Build order is post-order traversal: deps before dependents. `build("ngen-view")` returns `[obs, rhi, rhivulkan, renderer, …, ngen-view]` — exactly the order
the backend should emit.

`Project::find` only searches entry targets. Internal-target lookup, if needed, requires walking from a root.

### 4.3 `build::Platform` / `build::Configuration`

Generic identity plus per-language settings as composition.

```cpp
namespace build {

struct Platform {
    std::string name;
    std::string os;
    std::string graphics_api;
    std::string exe_suffix;

    cxx::PlatformSettings cxx;
};

struct Configuration {
    std::string name;
    Path out_dir = "_out";

    cxx::ConfigurationSettings cxx;
};

}
```

Adding a language adds a sibling field (`csharp::PlatformSettings csharp;`). The framework's structs grow by one field per language; nothing else changes.

### 4.4 `build::Tool` and `build::Alias`

Stay in the framework core. Both are generic — neither is an extension target.

`Tool` is the deliberate escape hatch: a one-shot command (glslc, copy, format/tidy/clean) that doesn't deserve a typed language target. It's a `Target` subclass
in the framework, dispatched via `dynamic_cast<Tool*>(target)` in the backend.

`Alias` is graph-level indirection — a named target that resolves to another at backend time based on a context map (`platform`, `config`, …).

---

## 5. C++ as Extension

`build/framework/cxx/` defines the C++ language module. Everything in `namespace build::cxx`.

### 5.1 `cxx::Target`

The user-facing wrapper. Owns the base `build::Target` via `unique_ptr`. Holds C++-specific data fields directly. Registers itself with the base on construction.

```cpp
namespace build::cxx {

enum class Kind {
    StaticLibrary,
    SharedLibrary,
    Program,
};

class Target {
public:
    explicit Target(std::string name, Kind kind);

    Target(const Target&) = delete;
    auto operator=(const Target&) -> Target& = delete;
    auto operator=(Target&&) -> Target& = delete;

    Target(Target&& other) noexcept;  // moves data and re-registers with base_

    operator build::Target&();
    operator const build::Target&() const;

    auto kind() const -> Kind;

    auto sources(std::vector<Path>) -> Target&;
    auto std(std::string_view) -> Target&;
    auto define(std::string) -> Target&;
    auto include(Path) -> Target&;
    auto public_include(Path) -> Target&;
    auto warning_off(std::string_view) -> Target&;
    auto flag_raw(std::string) -> Target&;
    auto optimize(OptLevel) -> Target&;
    auto debug(bool) -> Target&;
    auto pic(bool) -> Target&;

    auto link(Target& other) -> Target&;            // records owner.depend_on(other.owner()) and a cxx-side link entry
    auto link(std::string_view system_lib) -> Target&;
    auto link_raw(std::string) -> Target&;
    auto lib_search(Path) -> Target&;
    auto rpath(std::string) -> Target&;

    // public data exposed for the backend to read; mutated through the fluent API above
    std::vector<Path> sources_data;
    std::vector<Path> includes_data;
    std::vector<Path> public_includes_data;
    std::vector<std::string> defines_data;
    std::vector<std::string> warning_suppressions_data;
    std::vector<std::string> raw_compile_flags_data;
    std::optional<OptLevel> opt_data;
    std::optional<bool> debug_info_data;
    bool pic_data = true;
    std::string std_data;
    std::vector<build::Target*> linked_targets_data;
    std::vector<std::string> system_libs_data;
    std::vector<std::string> raw_link_flags_data;
    std::vector<Path> lib_search_dirs_data;
    std::vector<std::string> rpaths_data;

private:
    std::unique_ptr<build::Target> base_;
    Kind kind_;
};

}
```

The constructor calls `base_->register_extension(*this)`. The move constructor moves `base_` plus all data fields, then calls `register_extension(*this)` again
to update the back-pointer for the new location. Copy and copy/move assignment are deleted.

This means `auto obs = cxx::static_library("obs").sources({...}).public_include({...});` works — the temporary is moved into `obs` once at the end, and the
move ctor updates the back-pointer.

### 5.2 Factory Functions

```cpp
namespace build::cxx {

auto static_library(std::string name) -> Target;
auto shared_library(std::string name) -> Target;
auto program(std::string name) -> Target;

}
```

Each constructs a `cxx::Target` with the appropriate `Kind`. No separate `Library` / `Program` / `StaticLibrary` classes — `Kind` is the discriminator.

### 5.3 `cxx::PlatformSettings` / `cxx::ConfigurationSettings`

Hold the C++-specific platform and configuration knobs that used to live directly on `build::Platform` / `build::Configuration`.

```cpp
namespace build::cxx {

struct PlatformSettings {
    Toolchain toolchain;  // shape TBD; see §10
    std::vector<std::string> defines;
    std::vector<std::string> extra_compile_flags;
    std::vector<std::string> extra_link_flags;
    std::vector<std::string> system_libs;
};

struct ConfigurationSettings {
    OptLevel opt = OptLevel::O0;
    bool debug_info = true;
    Linkage default_linkage = Linkage::Static;
    std::vector<std::string> defines;
    std::vector<std::string> extra_compile_flags;
    std::vector<std::string> extra_link_flags;
};

}
```

`OptLevel` and `Linkage` enums move into `build::cxx::` (their values are clang/gcc-flavored even though the concepts are general).

### 5.4 `cxx::Toolchain` (Shape Open)

The shape of `cxx::Toolchain` was not settled in the earlier discussions — multiple approaches were considered (abstract base class, struct of closures, fully
data-driven with templating) and none felt right. v3 explicitly defers this. What v3 *does* lock in is that the `Toolchain` problem is contained: it lives only
inside `build/framework/cxx/`, only `cxx::ninja::emit` consumes it, and changes to its shape don't affect anything outside the cxx module.

See §10 for what's open about it.

---

## 6. Backend Dispatch

The Ninja backend splits into two pieces:

**Generic orchestration** (`build/framework/backendninja.hpp`): walks `Project::default_build()` / `Project::build(name)` over each (platform × config) variant,
emits the rules header, top-level phonies, `compile_commands.json` aggregation, and `ensure_dirs`. Dispatches each target to a language-specific emitter.

**C++ emit** (`build/framework/cxx/backendninja.hpp`): `cxx::ninja::emit(cxx_target, variant, platform.cxx, config.cxx)` translates a `cxx::Target` into compile,
archive, and link build edges.

Dispatch shape:

```cpp
for (auto* target : project.build(requested_name)) {
    if (auto* tool = dynamic_cast<Tool*>(target)) {
        emit_tool(*tool, variant);
        continue;
    }
    if (auto* cxx_t = target->extension<cxx::Target>()) {
        cxx::ninja::emit(*cxx_t, variant, platform.cxx, config.cxx);
        continue;
    }
    return std::unexpected(Error{"no emitter for " + target->name()});
}
```

A new language module adds one branch: `if (auto* csharp_t = target->extension<csharp::Target>()) { csharp::ninja::emit(...); }`.

---

## 7. Entry Targets vs Internal Targets

This is a deliberate semantic shrink from v2.

In v2, every target added to `Graph` was invokable by name. Users could run `./_out/ngen-build obs` and get the static library built directly.

In v3, only targets registered with `Project::target(t)` are invokable by name. Internal libraries (the things `ngen-view` depends on) are *not* registered;
they're reached via traversal from the registered entry points.

For ngen, this means the user-facing surface is just `ngen-view` (plus the generic `clean` / `format` / `tidy` `Tool` targets). `obs`, `rhi`, `rhivulkan`,
`renderer`, `scene`, `sceneusd`, `imgui`, `ui` are no longer top-level invokable. Anyone wanting to build a sub-library directly would need it explicitly added
to the project as an entry point, or would invoke its concrete output path through ninja directly.

The orchestrator (`bootstrap.cpp`) is updated accordingly: passing an unknown name no longer falls back to view; it errors out.

---

## 8. What `build/build.cpp` Looks Like

```cpp
#include "framework/cxx/target.hpp"
#include "framework/cxx/toolchain.hpp"
#include "framework/project.hpp"
#include "framework/tool.hpp"

using namespace build;

auto main(int argc, char** argv) -> int {
    auto obs = cxx::static_library("obs")
        .sources(glob({.include = "src/obs/**/*.cpp"}))
        .public_include({
            "src/obs",
            "external/concurrentqueue",
        });

    auto rhi = cxx::static_library("rhi")
        .sources(glob({.include = "src/rhi/*.cpp"}))
        .public_include({"src/rhi"})
        .include({"external/imgui"});

    auto rhivulkan = cxx::static_library("rhivulkan")
        .sources(glob({.include = "src/rhi/vulkan/**/*.cpp"}))
        .public_include({"src/rhi/vulkan"})
        .include({
            "src",
            "external/imgui",
            "external/imgui/backends",
        })
        .only_on({"linux-vulkan"})
        .link(rhi);

    // … other libraries follow the same shape …

    auto view = cxx::program("ngen-view")
        .sources({
            "src/main.cpp",
            "src/camera.cpp",
            "src/debugdraw.cpp",
            "src/jobsystem.cpp",
        })
        .include({
            "src",
            "src/obs",
            // …
        })
        .link(obs)
        .link(rhi)
        .link(rhivulkan)
        .link("vulkan")
        .link("m");

    Project p;

    p.add_platform({
        .name = "linux-vulkan",
        .os = "linux",
        .graphics_api = "vulkan",
        .cxx = cxx::PlatformSettings{
            .toolchain = clang_toolchain(),
            .defines = {"NGEN_GFX_VULKAN"},
            .system_libs = {},
        },
    });

    p.add_config({
        .name = "debug",
        .out_dir = "_out/linux-vulkan/debug",
        .cxx = cxx::ConfigurationSettings{
            .opt = cxx::OptLevel::O0,
            .debug_info = true,
            .defines = {"DEBUG=1"},
        },
    });

    p.target(view);
    p.default_target(view);

    return run_ninja_backend(p, argc, argv);
}
```

`obs`, `rhi`, etc. live as locals in `main`. They must outlive `p`'s use, which they do — `main`'s scope holds them until process exit. `p.target(view)` only
registers `view` as an entry target; the rest are reached by walking `view->deps`.

---

## 9. Migration

This is one cohesive change, hard to do incrementally without leaving the build broken across commits. Order I'd suggest:

1. **Add `build::Project` alongside `build::Graph`.** Don't remove `Graph` yet. Project has the new entry-target semantics; everything else stays.

2. **Introduce extension storage on `build::Target`.** `register_extension` / `extension` / `has_extension`. No callers yet.

3. **Add `build::cxx::Target` and the factory functions.** Co-exists with `Library` / `Program` / etc. for now.

4. **Move C++-specific fields off `build::Target`.** Anything `sources`, `defines`, `include`, `link`, etc. moves to `cxx::Target`. The old `Library` / `Program`
   types stay but become thin shims that internally use `cxx::Target`. Callers (`build/build.cpp`) still compile.

5. **Move C++-specific fields off `build::Platform` and `build::Configuration`.** Composition fields `Platform::cxx` and `Configuration::cxx` populated at the
   call site.

6. **Split the Ninja backend.** Generic orchestration in `build/framework/backendninja.hpp`; cxx emit in `build/framework/cxx/backendninja.hpp`. Backend
   dispatches by `target->extension<cxx::Target>()`.

7. **Rewrite `build/build.cpp` against the v3 API.** Remove `Graph` usage; use `Project`. Rename construction to `cxx::static_library(...)` etc.

8. **Update `bootstrap.cpp`'s `ninja_target()` and unknown-name fallback.** Error on unknown names instead of substituting view.

9. **Delete `build::Graph`, `Library`, `Program`, `StaticLibrary`, `SharedLibrary`, `CxxToolchain`** (or re-shape `cxx::Toolchain` per §10), and
   the legacy fields on `build::Target` / `build::Platform` / `build::Configuration`.

10. **Update `docs/context_build_system.md`** to reflect v3.

`cxx::Toolchain` shape (§10) is a parallel, separable problem — its resolution doesn't block migration order.

---

## 10. Open Questions

**`cxx::Toolchain` shape.** Earlier exploration ruled out the inheritance + virtual base approach (clang-and-gcc-only), the closure struct (still 100 lines of
bodies somewhere), and the fully data-driven approach (templating overhead, complex framework assembler). None felt clean. v3 defers this — once the cxx module
is isolated, the toolchain is one decision in one place. Possible directions when we return to it:

- A truly minimal `cxx::Toolchain` that's just program paths + a few format strings, with `cxx::ninja::emit` containing the real flag-assembly knowledge.
- A pluggable shape where ngen ships a `clang_toolchain()` factory and a future `msvc_toolchain()` factory under `build/framework/cxx/`.
- Push the Toolchain abstraction down further: have `cxx::ninja::emit` call into `cxx::Toolchain` for argv tokens, but keep argv structure (compile / archive
  / link primitives) framework-side.

**Extension storage representation.** `std::unordered_map<std::type_index, void*>` works. A small flat-vector of `{type_index, void*}` pairs would be cheaper at
the small N we have. Cosmetic.

**Multi-entry build invocation.** `Project::build_all()` is in the API. The Ninja backend should expose this as `build all: phony e1 e2 e3`. Not hard; just
needs spelling out.

**`compile_commands.json` for build-system sources.** The pre-existing gap from v2 — `build/build.cpp`, `build/bootstrap.cpp`, `build/prebuild.cpp` aren't in the
project graph and so don't get entries. The v2 attempt to append entries in `bootstrap.cpp` was reverted. Still open; orthogonal to v3.

**`Tool` as extension or as subclass.** v3 keeps `Tool` as a `Target` subclass (today's approach). Conflating it with extensions doesn't buy anything — `Tool`
genuinely has a different shape (no language, no compile, just an opaque command).

---

## 11. Risks

**Scoping fights.** Targets must outlive `Project`'s use. In `main()` this is automatic, but moving target construction into a helper function (returning a
`cxx::Target` by value) requires care. The move constructor handles transfer correctly, but a function returning `cxx::Target` by value then storing it in a
container would require the container to handle move semantics correctly without additional copies. Vector reallocations are the obvious concern — since
`cxx::Target` move-constructs cleanly (re-registers the back-pointer), `std::vector<cxx::Target>` should work, but it's worth verifying early in migration.

**Loss of name-based dispatch for internal targets.** Anyone relying on `./_out/ngen-build obs` to build the `obs` static library directly will see this break.
Replacement is to invoke the concrete output path or to register the library as an entry target. Worth an explicit note when the migration lands.

**Toolchain still unsolved.** v3 doesn't fix the toolchain problem; it isolates it. If the eventual answer requires changes to `cxx::ninja::emit` or
`cxx::PlatformSettings` shape, those changes are localized but real.

**Move ctor correctness.** `cxx::Target`'s move constructor must re-register every time. If a future field is added and the move ctor isn't updated,
silent breakage (dangling back-pointer). Worth a comment on the class explaining the invariant.

---

## 12. Non-Goals

- Replacing v2's bootstrap chain. The four-stage build (`bootstrap.ninja` → `ngen-build` → `ngen-build-pre` → `ngen-build-graph` → `_out/build.ninja`) stays
  exactly as v2 specified it.
- Replacing the Ninja backend's output format. v3 changes how the graph is *modeled*; it does not change what `_out/build.ninja` looks like.
- Adding C# / Go / Rust support now. v3 makes adding them additive when the time comes; it does not add them speculatively.
- Solving the `cxx::Toolchain` shape. Bounded, deferred.
- Handling cross-language targets (e.g. a target that's both C++ and C#). The extension model permits multiple extensions on one `build::Target` in principle,
  but no design effort is being spent on it.
