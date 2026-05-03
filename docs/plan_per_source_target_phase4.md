# Plan — phase 4: per-TU fluent surface

Follow-up to `docs/plan_per_source_target.md`. Phases 1–3 are landed: every `.cpp` is a `cxx::ObjectFile` node in the graph, the emitter merges per-TU
override fields on top of parent state, and `cxx::Target::sources_data` is gone. Phase 4 adds the *user-facing API* to set those overrides. No graph or
emitter changes.

## 1. Goal

Let a user reach a single ObjectFile from the parent's fluent chain and apply per-TU compile knobs (defines, warning suppressions, compile flags, C++ std)
without dropping out of fluent style and without affecting siblings.

## 2. End-state API

### 2.1 Methods added to `cxx::ObjectFile`

Mirroring the per-TU-relevant subset of `cxx::Target`. Each returns `ObjectFile&` for chaining.

```cpp
auto define(std::string macro) -> ObjectFile&;
auto defines(std::vector<std::string> values) -> ObjectFile&;
auto warning_off(std::string_view name) -> ObjectFile&;
auto compile_flag(std::string token) -> ObjectFile&;
auto compile_flags(std::vector<std::string> values) -> ObjectFile&;
auto std(std::string_view value) -> ObjectFile&;
```

These mutate the existing `defines_data` / `warning_suppressions_data` / `compile_flags_data` / `std_data` fields the emitter already reads.

Skipped intentionally: `optimize(OptLevel)`, `debug(bool)`, `pic(bool)`. Those are platform/config concerns; if someone wants per-TU `-O3` they can
`.compile_flag("-O3")` directly. Less is more here.

### 2.2 Single new method on `cxx::Target`: `for_source`

```cpp
template <typename Fn>
auto for_source(const Path& path, Fn&& fn) -> Target&;
```

Looks up the ObjectFile whose `source() == path`, calls `fn(obj)`, returns `*this`. If the path doesn't match any source in this target, throws (or returns
an error via the same expected-style channel the rest of the framework uses — we'll match whatever the surrounding code does at implementation time).

Lookup is a linear scan over `objects_data`. Targets have <50 sources in practice; not worth a map.

## 3. What real call sites become

### 3.1 Targeted warning suppression instead of library-wide

Today (overly broad — every TU in `sceneusd` skips deprecation warnings):

```cpp
auto sceneusd =
    cxx::static_library("sceneusd")
        .std("c++20")
        .sources(glob({.include = "src/scene/usd*.cpp"}))
        .warning_off("deprecated-declarations");
```

After phase 4 (only the file that actually trips the warning):

```cpp
auto sceneusd =
    cxx::static_library("sceneusd")
        .std("c++20")
        .sources(glob({.include = "src/scene/usd*.cpp"}))
        .for_source("src/scene/usdscene.cpp", [](cxx::ObjectFile& obj) {
            obj.warning_off("deprecated-declarations");
        });
```

### 3.2 Per-TU `std`

If `sceneusd`'s C++20 requirement turns out to be one or two USD-touching TUs (and the rest could compile as C++23), you'd write:

```cpp
auto sceneusd =
    cxx::static_library("sceneusd")
        .sources(glob({.include = "src/scene/usd*.cpp"}))
        .for_source("src/scene/usdscene.cpp", [](cxx::ObjectFile& obj) {
            obj.std("c++20");
        });
```

The library no longer needs `.std("c++20")` at all. Other TUs in `sceneusd` go through the toolchain default (`c++23`).

### 3.3 Per-TU define for a debug-only file

```cpp
auto view =
    cxx::program("ngen-view")
        .sources({"src/main.cpp", "src/camera.cpp", "src/debugdraw.cpp", "src/jobsystem.cpp"})
        .for_source("src/debugdraw.cpp", [](cxx::ObjectFile& obj) {
            obj.define("DEBUG_DRAW_VERBOSE=1");
        });
```

### 3.4 Multiple knobs on one file

```cpp
.for_source("src/scene/usdscene.cpp", [](cxx::ObjectFile& obj) {
    obj.warning_off("deprecated-declarations");
    obj.compile_flag("-Wno-shadow");
    obj.define("USD_HEAVY_TU=1");
});
```

Reads top-to-bottom; no `.end()` ceremony.

## 4. What we explicitly don't add

- **No `.source(path)` proxy returning a builder that requires `.end()`**. Considered and rejected — it adds an extra type and an end-of-scope ritual for
  no expressive gain over the lambda form.
- **No `.for_each_source(fn)`**. The library-level methods (`cxx::Target::warning_off`, `.define`, `.compile_flag`) already cover "apply to all". Adding
  per-source iteration would just be a slower re-spelling.
- **No glob-based `.for_sources({.include = "..."}, fn)`**. Not impossible, but no concrete use case has surfaced. Easy follow-up if one does.
- **No `.source(path)` that adds-if-missing**. Conflates configure-existing with add-new; the `sources(...)` family already handles adding.
- **No per-TU link/include changes**. Includes are inherited from parent at compile time. Per-TU includes would be a semantic bigger than this plan and
  there's no real demand.

## 5. Implementation sketch

Tiny — almost all the work was done in phases 1–3.

In `build/framework/cxx/objectfile.hpp`, add the six fluent methods. Each is a one-liner appending to the existing data field:

```cpp
auto define(std::string macro) -> ObjectFile& {
    defines_data.push_back(std::move(macro));
    return *this;
}
auto warning_off(std::string_view name) -> ObjectFile& {
    warning_suppressions_data.emplace_back(name);
    return *this;
}
// ...etc
```

In `build/framework/cxx/target.hpp`, add `for_source`:

```cpp
template <typename Fn>
auto for_source(const Path& path, Fn&& fn) -> Target& {
    for (auto& obj : objects_data) {
        if (obj->source() == path) {
            std::forward<Fn>(fn)(*obj);
            return *this;
        }
    }
    // error: path not in this target's sources — surface via the project's
    // existing error pathway. Concrete shape decided at implementation.
    return *this;
}
```

Emitter: no change. `emit_object_file` already merges `obj.defines_data` after `parent->defines_data`, `obj.warning_suppressions_data` after the
parent's, etc.

## 6. Files touched

- `build/framework/cxx/objectfile.hpp` — six fluent methods.
- `build/framework/cxx/target.hpp` — one `for_source` template.
- `build/build.cpp` — only if we want to actually narrow `sceneusd`'s warning_off / std at the same time. Otherwise unchanged.

Untouched: `backendninja.hpp` (already merges overrides correctly), framework core, every other file in `build/framework/cxx/`.

## 7. Verification

Phase 4 has no exit criterion of "byte-identical output" because the surface is brand-new — nothing in the current `build.cpp` calls `for_source` or the
new ObjectFile methods. The verification is:

- New code compiles.
- Existing build is byte-identical (the new APIs are additive; nothing changes if no one calls them).
- A throwaway test call like `lib.for_source("src/x.cpp", [](auto& o) { o.define("FOO=1"); })` produces the expected `-DFOO=1` only on `x.cpp`'s compile
  edge in `build.ninja`, and not on its siblings. Verified by inspecting the diff.
