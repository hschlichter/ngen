# Plan v4 — Parallel `cxx::` Mirror of the Core Types

This plan refines `docs/plan_build_system_v3.md`. It does not replace v3's bootstrap chain or Ninja-backend output shape — those carry over. v4 reshapes one
specific thing: the boundary between the framework core (`build::Project`, `build::Platform`, `build::Configuration`, `build::Target`) and language modules
(`build::cxx::*`).

After v4, the core types carry zero language vocabulary. Each one has an `ExtensionMap`, and the C++ side lives entirely in a parallel set of types under
`build::cxx`:

```text
build {
    Project, Platform, Configuration, Target           # core, language-agnostic

    cxx {
        Toolchain                                       # composed inside cxx::Platform
        Platform                                        # extension on build::Platform
        Configuration                                   # extension on build::Configuration
        Target                                          # extension on build::Target (the existing fluent wrapper)
        program / static_library / shared_library      # factories for cxx::Target
    }
}
```

Adding another language is purely additive — `build::csharp::Platform`, `build::csharp::Configuration`, etc. — with no edits to the core.

---

## 1. Mental model

```text
Targets         → graph structure (identity + deps + gating)
Project         → build context (platforms, configs) + entry points
Platform        → environment identity (name, os, graphics_api, exe_suffix)
Configuration   → variant identity (name, out_dir)
ExtensionMap    → on each core type; holds language-specific extension data

cxx::Toolchain      → which compiler / linker / archiver to invoke
cxx::Platform       → per-platform C++ compile/link/define data + a Toolchain
cxx::Configuration  → per-config C++ compile/link/define data
cxx::Target         → per-target C++ data (sources, includes, defines, link, std, plus per-target raw escapes)
```

The core describes *what to build* and *where*. The cxx mirror describes *how the C++ compiler should be invoked*, layered: per-platform defaults, per-config
overrides, per-target specifics.

---

## 2. Goals

**No language leakage in the core.** `build::Platform` does not name `cxx`. `build::Configuration` does not name `cxx`. `build::Project` and `build::Target`
are similarly clean. Every C++-flavored field, method, or accessor lives under `namespace build::cxx`.

**Parallel naming.** The cxx extension on `build::Platform` is `build::cxx::Platform`. On `build::Configuration` it is `build::cxx::Configuration`. On
`build::Target` it is `build::cxx::Target` (already true). Same name, namespace-qualified, mirroring the core. This is intentional — it makes the relationship
between the core type and its language extension self-documenting.

**ExtensionMap on every core type.** `Project`, `Platform`, `Configuration`, `Target` each own one `ExtensionMap`. Multiple extensions per owner are fine; nested
extensions (an extension that itself owns an `ExtensionMap`) are not.

**Toolchain is tools, not flags.** `cxx::Toolchain` describes the compiler/linker/archiver binaries and the default C++ standard. It does not own compile_flag /
link_flag / define lists. Those live on `cxx::Platform` (per-platform defaults) and `cxx::Configuration` (per-config overrides).

**Adding a language is additive.** A new `build::csharp::Platform` plus its accessor and ninja-emitter is enough to teach the build system about C#. No edits
to `build::Platform` or `build::Configuration`.

---

## 3. Non-goals

- Changing the bootstrap chain or the on-disk shape of `_out/build.ninja`. v2 territory; frozen.
- Adding new languages speculatively. v4 makes adding `build::csharp` purely additive when wanted; it does not write any csharp code.
- Cross-language targets (one `build::Target` with both `cxx::Target` and `csharp::Target` extensions). The model permits it; no design effort spent until
  there's a real use case.
- Reworking `Tool` or `Alias`. Both stay as `Target` subclasses in the framework — they're language-agnostic by nature.

---

## 4. Concrete API targets

### 4.1 `build::ExtensionMap`

New file: `build/framework/extensionmap.hpp`. Header-only, in `namespace build`.

```cpp
namespace build {

class ExtensionMap {
public:
    template <typename Ext>
    auto has() const -> bool;

    template <typename Ext>
    auto get() -> Ext&;

    template <typename Ext>
    auto get() const -> const Ext&;

    template <typename Ext, typename... Args>
    auto add(Args&&... args) -> Ext&;

    template <typename Ext>
    auto attach(Ext& ext) -> void;

private:
    struct Entry {
        std::unique_ptr<void, void (*)(void*)> ptr;
    };

    std::unordered_map<std::type_index, Entry> entries_;
};

}
```

Two attachment modes:

- `add<Ext>(args...)` — `ExtensionMap` owns the extension. Used for `cxx::Platform` and `cxx::Configuration`, which have no other home.
- `attach<Ext>(ext)` — non-owning. The deleter is a no-op; the extension's lifetime is controlled by whoever owns the `Ext&`. Used by `cxx::Target`, which owns
  itself and registers a back-pointer in the base `Target`'s `ExtensionMap`.

`add<Ext>` is idempotent over an existing key — returns the existing entry. `attach<Ext>` over an existing key replaces (this matches today's
`register_extension` semantics on `build::Target`'s move ctor).

### 4.2 `build::Target` (revised storage; same external API)

Keeps the v3 surface (`name`, `depend_on`, `only_on`/`only_in`/`except_on`/`except_in`/`enabled_for`, `extension<T>()`, `has_extension<T>()`). Internal storage
moves from `unordered_map<type_index, void*>` to an `ExtensionMap` whose entries are non-owning (via `attach`). `cxx::Target`'s back-pointer registration
becomes `target.extensions().attach(*this)`.

### 4.3 `build::Platform` (new shape)

```cpp
namespace build {

class Platform {
public:
    explicit Platform(std::string name);

    auto name() const -> const std::string&;

    auto os(std::string value) -> Platform&;
    auto graphics_api(std::string value) -> Platform&;
    auto exe_suffix(std::string value) -> Platform&;

    auto os() const -> const std::string&;
    auto graphics_api() const -> const std::string&;
    auto exe_suffix() const -> const std::string&;

    auto extensions() -> ExtensionMap&;
    auto extensions() const -> const ExtensionMap&;

private:
    std::string name_;
    std::string os_;
    std::string graphics_api_;
    std::string exe_suffix_;
    ExtensionMap extensions_;
};

}
```

No `cxx` field. The C++ data lives in a `cxx::Platform` extension inside `extensions_`.

### 4.4 `build::Configuration` (new shape)

```cpp
namespace build {

class Configuration {
public:
    explicit Configuration(std::string name);

    auto name() const -> const std::string&;

    auto out_dir(Path value) -> Configuration&;
    auto out_dir() const -> const Path&;

    auto extensions() -> ExtensionMap&;
    auto extensions() const -> const ExtensionMap&;

private:
    std::string name_;
    Path out_dir_ = "_out";
    ExtensionMap extensions_;
};

}
```

No `cxx` field. Per-config C++ overrides live in a `cxx::Configuration` extension.

### 4.5 `build::Project` (revised)

```cpp
namespace build {

class Project {
public:
    auto target(Target& t) -> void;
    auto default_target(Target& t) -> void;

    auto platform(std::string name) -> Platform&;
    auto config(std::string name) -> Configuration&;

    auto find_platform(std::string_view name) -> Platform*;
    auto find_config(std::string_view name) -> Configuration*;

    auto find(std::string_view name) const -> Target*;
    auto build(std::string_view name) const -> std::vector<Target*>;
    auto build_all() const -> std::vector<Target*>;
    auto default_build() const -> std::vector<Target*>;

    auto roots() const -> const std::vector<Target*>&;
    auto platforms() const -> std::vector<Platform*>;
    auto configs() const -> std::vector<Configuration*>;

private:
    std::vector<Target*> roots_;
    Target* default_ = nullptr;
    std::vector<std::unique_ptr<Platform>> platforms_;
    std::vector<std::unique_ptr<Configuration>> configs_;
};

}
```

`platform(name)` and `config(name)` create on first call, return existing on subsequent calls (idempotent — same shape as `cxx::platform(p)` and
`cxx::configuration(c)` accessors). Storage uses `unique_ptr` for stable addresses across vector growth and re-entry into `platform("...")`.

### 4.6 `build::cxx::Toolchain` (trimmed)

```cpp
namespace build::cxx {

class Toolchain {
public:
    auto compiler(std::string value) -> Toolchain&;
    auto archiver(std::string value) -> Toolchain&;
    auto linker(std::string value) -> Toolchain&;
    auto default_std(std::string value) -> Toolchain&;

    auto compiler() const -> const std::string&;
    auto archiver() const -> const std::string&;
    auto linker() const -> const std::string&;
    auto default_std() const -> const std::string&;

private:
    std::string compiler_;
    std::string archiver_;
    std::string linker_;
    std::string default_std_ = "c++23";
};

}
```

Tools only. No flags, no defines, no system libs. Composed inside `cxx::Platform` — not a standalone extension on `build::Platform`.

### 4.7 `build::cxx::Platform`

```cpp
namespace build::cxx {

class Platform {
public:
    auto toolchain() -> Toolchain&;
    auto toolchain() const -> const Toolchain&;

    auto define(std::string value) -> Platform&;
    auto compile_flag(std::string value) -> Platform&;
    auto link_flag(std::string value) -> Platform&;
    auto system_lib(std::string value) -> Platform&;

    auto defines() const -> const std::vector<std::string>&;
    auto compile_flags() const -> const std::vector<std::string>&;
    auto link_flags() const -> const std::vector<std::string>&;
    auto system_libs() const -> const std::vector<std::string>&;

private:
    Toolchain toolchain_;
    std::vector<std::string> defines_;
    std::vector<std::string> compile_flags_;
    std::vector<std::string> link_flags_;
    std::vector<std::string> system_libs_;
};

inline auto platform(build::Platform& p) -> Platform& {
    if (!p.extensions().has<Platform>()) {
        return p.extensions().add<Platform>();
    }
    return p.extensions().get<Platform>();
}

inline auto find_platform(const build::Platform& p) -> const Platform* {
    return p.extensions().has<Platform>() ? &p.extensions().get<Platform>() : nullptr;
}

}
```

Replaces today's `cxx::PlatformSettings` and folds the toolchain inside. `system_lib` is here (per-platform — `vulkan`, `m`) and not on `cxx::Configuration`.

### 4.8 `build::cxx::Configuration`

```cpp
namespace build::cxx {

class Configuration {
public:
    auto define(std::string value) -> Configuration&;
    auto compile_flag(std::string value) -> Configuration&;
    auto link_flag(std::string value) -> Configuration&;

    auto defines() const -> const std::vector<std::string>&;
    auto compile_flags() const -> const std::vector<std::string>&;
    auto link_flags() const -> const std::vector<std::string>&;

private:
    std::vector<std::string> defines_;
    std::vector<std::string> compile_flags_;
    std::vector<std::string> link_flags_;
};

inline auto configuration(build::Configuration& c) -> Configuration& {
    if (!c.extensions().has<Configuration>()) {
        return c.extensions().add<Configuration>();
    }
    return c.extensions().get<Configuration>();
}

inline auto find_configuration(const build::Configuration& c) -> const Configuration* {
    return c.extensions().has<Configuration>() ? &c.extensions().get<Configuration>() : nullptr;
}

}
```

Replaces today's `cxx::ConfigurationSettings`. Today's `OptLevel` enum, `debug_info` bool, and `default_linkage` field are gone — opt level / debug info are
expressed as raw `compile_flag("-O0")` / `compile_flag("-g")`, and `default_linkage` was unused by the emitter (the `cxx::Target`'s `Kind` is the source of truth
for static vs shared).

### 4.9 `build::cxx::Target` (kept; ngen-extras retained)

Today's `cxx::Target` (the fluent wrapper that owns `unique_ptr<build::Target>` and attaches itself to the base's `ExtensionMap`) keeps its existing surface.
The high-level fields are `sources`, `includes`, `public_include`, `defines`, `link`, `std`. Plus the per-target raw escapes ngen actually uses: `warning_off`,
`compile_flag` (renamed from `flag_raw`), `link_flag` (renamed from `link_raw`), `lib_search`, `rpath`. Optimize / debug / pic become typed sugar that compose
into compile flags, or get dropped — see §7 open questions.

The move ctor invariant changes only mechanically: `base_->extensions().attach(*this)` instead of `base_->register_extension(*this)`.

---

## 5. What goes away

- `Platform::cxx` field on `build::Platform`.
- `Configuration::cxx` field on `build::Configuration`.
- `cxx::PlatformSettings` (renamed to `cxx::Platform`, refactored).
- `cxx::ConfigurationSettings` (renamed to `cxx::Configuration`, refactored).
- `cxx::OptLevel` enum, `debug_info` bool, `default_linkage` field on the configuration extension.
- Hard-coded `variant.platform->cxx.toolchain` / `variant.platform->cxx.defines` reads inside `cxx::ninja::emit`. Replaced by extension lookups.
- The `add_platform(Platform)` / `add_config(Configuration)` overloads on `Project` (designated-initializer construction). Replaced by fluent
  `p.platform("name")` / `p.config("name")`.

---

## 6. Backend dispatch

Generic orchestration (`build/framework/backendninja.hpp`) is unchanged in shape. The cxx emitter (`build/framework/cxx/backendninja.hpp`) replaces its
hardcoded reads with extension lookups:

```cpp
// inside cxx::ninja::emit
auto* platform_ext = cxx::find_platform(*variant.platform);
auto* config_ext   = cxx::find_configuration(*variant.config);

if (!platform_ext) {
    return std::unexpected(Error{"platform " + variant.platform->name() + " has no cxx extension"});
}

const auto& tc = platform_ext->toolchain();
auto compile_flags = platform_ext->compile_flags();
if (config_ext) {
    append_unique_str(compile_flags, config_ext->compile_flags());
}
append_unique_str(compile_flags, target.compile_flags_data);
// ... similarly for defines, link_flags, system_libs
```

Missing `cxx::Platform` is a fatal error (caught at emit, reported with the platform name). Missing `cxx::Configuration` is fine — empty overrides.

---

## 7. Open questions

**`cxx::Target` typed sugar coexists with raw flags.** `.optimize(OptLevel)`, `.debug(bool)`, `.pic(bool)` stay — they're self-documenting at the call site and
catch typos that raw strings wouldn't. `.compile_flag(...)` and `.link_flag(...)` are the escape hatch for anything not modeled. Per-target typed methods take
precedence over per-config raw flags when both are set (existing v3 behavior via `target.opt_data.value_or(config_opt)`).

**`flag_raw` vs `compile_flag` naming on `cxx::Target`.** Today: `flag_raw`, `link_raw`, `link_raw_many`. With `cxx::Platform` and `cxx::Configuration` using
`compile_flag` and `link_flag`, mirroring those names on `cxx::Target` is consistent. Migrating means renaming three methods. Worth it.

**`include` vs `public_include`.** `cxx::Target` has both. `include` is target-private (only used compiling that target's sources). `public_include` propagates
to dependents. Both stay — they have distinct semantics — but the listing in §4.9 should be explicit.

**`ExtensionMap::add` over an existing entry.** Spec says idempotent (return existing). `attach` says replace. Document the asymmetry on `ExtensionMap` and pick
behavior carefully — getting it backward leads to silent stale-extension bugs.

**`find_platform(const build::Platform&)` vs `platform(build::Platform&)` const-correctness.** Lazy-create needs non-const. Const access uses `find_platform`
returning nullable. v3 had this issue and v4 inherits it; pattern is consistent with the project-level `find_*` accessors.

**Where do `Tool` and `Alias` fit?** Same as v3: as `Target` subclasses, dispatched via `dynamic_cast` in the backend. Not extensions. v4 doesn't change this.

---

## 8. Migration steps

The build must stay green at each step.

1. **Add `build/framework/extensionmap.hpp`.** No callers yet. Compile-only.

2. **Reroute `build::Target` extension storage through `ExtensionMap`.** Internal change; public surface (`extension<T>`, `has_extension<T>`,
   `register_extension`) preserved by aliasing `register_extension` to `extensions_.attach`. No call-site changes anywhere else.

3. **Convert `build::Platform` to fluent class with `ExtensionMap`.** Add the new `os(...)` / `graphics_api(...)` / `exe_suffix(...)` setters, the
   `extensions()` accessor, and `unique_ptr` storage in `Project::platforms_`. Keep the existing `cxx` field for now (don't delete). Add
   `Project::platform(name)` returning `Platform&`. Existing `add_platform(Platform)` stays, just heap-allocates and pushes.

4. **Convert `build::Configuration` similarly.** Add `out_dir(...)` setter, `extensions()` accessor, `unique_ptr` storage. Add `Project::config(name)`. Keep
   `cxx` field for now.

5. **Add the new `cxx::Toolchain` (trimmed)** alongside the existing struct. Co-exist via temporary alias.

6. **Add `cxx::Platform`** (new class, fluent, owns a `cxx::Toolchain`) and the `cxx::platform()` / `cxx::find_platform()` accessors. Co-exist with
   `cxx::PlatformSettings`.

7. **Add `cxx::Configuration`** (new class, fluent) and the `cxx::configuration()` / `cxx::find_configuration()` accessors. Co-exist with
   `cxx::ConfigurationSettings`.

8. **Teach `cxx::ninja::emit` to read extensions if present**, falling back to the legacy structs. Dual-source step.

9. **Rewrite `build/build.cpp` against the v4 API.** `p.platform("linux-vulkan").os(...).graphics_api(...)`, `cxx::platform(linux).compile_flag(...)`,
   `cxx::platform(linux).toolchain().compiler("clang++")`, `p.config("debug").out_dir(...)`, `cxx::configuration(debug).compile_flag("-O0")`. Drop the
   designated-initializer `Platform{}` / `Configuration{}` calls.

10. **Rename `cxx::Target::flag_raw` → `compile_flag`, `link_raw` → `link_flag`, `link_raw_many` → `link_flags`.** Update `build/build.cpp`.

11. **Confirm `cxx::Target` typed sugar still works** alongside the new raw `compile_flag` / `link_flag`. `optimize` / `debug` / `pic` stay as-is; existing
    `target.opt_data.value_or(config_opt)` precedence preserved.

12. **Drop dual-source paths in `cxx::ninja::emit`.** Now reads from extensions only. Delete `Platform::cxx` field, delete `Configuration::cxx` field, delete
    `cxx::PlatformSettings`, delete `cxx::ConfigurationSettings`, delete the legacy `cxx::Toolchain` struct (the new one took over the name).

13. **Delete `Project::add_platform(Platform)` and `add_config(Configuration)`.** Only the fluent `platform(name)` / `config(name)` remain.

14. **Update `build/prebuild.cpp`'s heredoc** to list `extensionmap.hpp`, the new/renamed cxx headers in `_out/ngen-build-graph.ninja`'s dep list. Confirm a
    `touch` on each triggers a rebuild.

15. **Update `docs/context_build_system.md`** to reflect the v4 framework surface.

Each step is a separate commit. Steps 8 and 12 are the only ones with real complexity; the rest are mechanical.

---

## 9. Risks

**ExtensionMap lifetime confusion.** Two attachment modes (`add` owning, `attach` non-owning) coexist on the same map. A future contributor could
`attach` a stack-local extension and then forget. Mitigation: comment on `ExtensionMap` explaining the two modes; add an `attach<T>(T&)` that explicitly stores
a no-op deleter so the asymmetry is visible at the call site, not silently auto-derived.

**Move ctor invariant on `cxx::Target`.** Same as v3 — move ctor must re-attach. v4 doesn't change this; just renames `register_extension` to
`extensions().attach`. Comment on the class explains the invariant.

**Bootstrap stage's narrow dep list.** `_out/ngen-build-graph` rebuilds when any framework header changes. Adding `extensionmap.hpp` and reshuffling
`build/framework/cxx/*.hpp` rewrites means updating `prebuild.cpp`'s heredoc. Forgetting it is silent — stale `ngen-build-graph` against new headers. Test by
`touch`-ing each new file and confirming a rebuild picks it up.

**Missing extension at emit.** A platform with no `cxx::Platform` extension is a project-level setup bug, not framework state. `cxx::ninja::emit` must surface
it as a fatal error with the platform name, not silently use defaults. Same for missing `cxx::Toolchain::compiler()` (empty string).

**`cxx::Target` rename churn.** Renaming `flag_raw` → `compile_flag` collides with the local `compile_flag` field name in `cxx::Platform` /
`cxx::Configuration`. Different namespaces and class scopes — no actual conflict — but readers may stumble. Trivial; mention in commit message.

---

## 10. Acceptance

After v4 lands:

- `build::Platform` and `build::Configuration` carry zero references to `cxx`.
- `cxx::PlatformSettings` and `cxx::ConfigurationSettings` do not exist; their replacements are `cxx::Platform` and `cxx::Configuration`, both fluent.
- `cxx::Toolchain` holds tools only (compiler / archiver / linker / default_std), composed inside `cxx::Platform`.
- `Project::platform(name)` and `Project::config(name)` return references for fluent chaining; `add_platform` / `add_config` are gone.
- ngen's `build/build.cpp` uses the new fluent surface throughout. No designated-initializer construction of `Platform` / `Configuration` remains.
- `make` builds `ngen-view` end-to-end with no functional change.
- `_out/build.ninja` content is byte-identical to pre-v4 (no behavior drift).
- `docs/context_build_system.md` is updated.

A hypothetical `build::csharp::Platform` could be added without editing any file in `build/framework/` outside `build/framework/csharp/`.
