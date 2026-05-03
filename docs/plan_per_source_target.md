# Plan — per-source targets in `build::cxx`

## 1. Goal

Make every C++ translation unit a first-class node in the build graph. Today a library/program owns a flat list of `Path` entries in
`cxx::Target::sources_data`; sources have no identity, no per-TU configurability, and no presence in `Project::build_all()`. After this change, each `.cpp`
becomes its own `build::Target` (carrying a `cxx::ObjectFile` extension) that the parent library/program depends on. The framework graph then treats compile
edges the same way it already treats archive/link edges — uniform identity, uniform caching, uniform dep walking.

The work is contained in `build::cxx` and `backendninja.hpp`. The framework core (`build::Target`, `Project`, `Platform`, `Configuration`, `ExtensionMap`) is
untouched. Frontends (`build/build.cpp`, future build files) keep the same fluent API surface.

## 2. Non-goals

- No new top-level concepts. ObjectFile is a `cxx::Target`-style extension on a plain `build::Target`; same registration model as today.
- No new addressing/UX layer. Whatever sits above (listing, dispatch, name resolution) is independent and out of scope here. This plan only changes what nodes
  exist in the graph and how the emitter walks them.
- No change to the on-disk shape of `_out/build.ninja` rules (`cxx`, `archive`, `link_exe`, `link_shared`, `tool`). The set of build edges shifts in *origin*
  (each compile edge is now emitted from a TU node rather than from inside the parent's emit) but the edges themselves look the same.
- No change to `compile_commands.json` content or ordering guarantees.
- Per-TU fluent API (`object.define(...)`, `object.warning_off(...)`) is structurally enabled but the surface is added on demand, not as part of this work.

## 3. Current state (one-paragraph recap)

`cxx::Target` (build/framework/cxx/target.hpp) wraps one `build::Target` and stores `sources_data: vector<Path>` plus all the C++ knobs. The ninja emitter
(build/framework/backendninja.hpp) walks `Project::build_all()` × platform × config; for each cxx target it calls `emit_cxx_objects`, which loops
`sources_data` and writes a `build .../foo.cpp.o: cxx src/.../foo.cpp` edge using a compile command merged from {platform, config, parent target, parent's
linked deps' public_includes}. The library/program emit then archives/links the returned object list. Sources never enter the framework graph.

## 4. End-state design

### 4.1 New type: `cxx::ObjectFile`

A cxx-side extension that mirrors how `cxx::Target` is structured: it owns a `shared_ptr<build::Target>` and attaches itself to that target's `ExtensionMap`.

```cpp
namespace build::cxx {

class ObjectFile {
public:
    ObjectFile(cxx::Target& parent, Path source);

    // Inheritance pointer — read-only, used by the emitter to fetch
    // the merged compile context (includes, defines, std, flags, link-deps).
    auto parent() const -> const cxx::Target& { return *parent_; }

    auto source() const -> const Path& { return source_; }
    auto owner() -> build::Target& { return *base_; }
    auto owner() const -> const build::Target& { return *base_; }

    // Per-TU overrides. Empty by default; merged on top of parent values
    // at emit time. Not exposed via fluent surface initially — hooks for
    // later (.define / .warning_off / .compile_flag / .std).
    std::vector<std::string> defines_data;
    std::vector<std::string> warning_suppressions_data;
    std::vector<std::string> compile_flags_data;
    std::string std_data;     // empty → inherit from parent / toolchain

private:
    std::shared_ptr<build::Target> base_;
    cxx::Target* parent_;     // raw — parent owns lifetime via dep edge
    Path source_;
};

} // namespace build::cxx
```

Identity: the `build::Target` name is `parent_target_name + "/" + source_path` (e.g. `renderer/src/renderer/renderthread.cpp`). This guarantees uniqueness
across the project even if two libraries list the same source — each gets its own ObjectFile node, matching today's behaviour where the same source compiled
into two libs produces two distinct `_out/.../obj/<lib>/...` outputs.

Gating: ObjectFile delegates `enabled_for(platform, config)` via its parent — it's enabled exactly when the parent is. We do *not* copy the only/except sets
into the child; we ask the parent at query time.

### 4.2 Library / program ownership

`cxx::Target::sources(...)` no longer pushes paths into `sources_data`. It constructs `ObjectFile` children, registers a dep edge from the parent's
`build::Target` onto each child's `build::Target`, and stores the children in a new field:

```cpp
std::vector<std::shared_ptr<ObjectFile>> objects_data;   // replaces sources_data
```

Why `shared_ptr`: matches `cxx::Target`'s existing `shared_ptr<build::Target>` lifetime model. The parent owns the children; the framework's `Project` only
sees the underlying `build::Target*` via deps.

The fluent overloads (`sources(vector<Path>)`, `sources(initializer_list<Path>)`) keep the same shape — internally each path materializes an ObjectFile child.

### 4.3 Project visibility

No code change in `Project`. Today `build_all()` walks every registered target and its transitive deps; ObjectFiles are deps of their parent libraries, so
they appear in `build_all()` automatically once the parent is registered. `Project::roots()` continues to return only what was explicitly registered via
`p.target(...)` — ObjectFiles are not roots, which is what we want for any consumer that surfaces top-level targets.

### 4.4 Emitter changes (`backendninja.hpp`)

`emit_target` already dispatches by extension. Add a third arm:

```cpp
if (auto* tool = target->extension<Tool>()) {
    output = emit_tool(*tool, variant, order_only);
} else if (auto* obj = target->extension<cxx::ObjectFile>()) {
    output = emit_object_file(*obj, variant);            // NEW
} else if (auto* cxx_t = target->extension<cxx::Target>()) {
    output = emit_cxx(*cxx_t, variant, order_only);
}
```

`emit_object_file` is the body of today's per-source loop, lifted out of `emit_cxx_objects`. It pulls the merged compile context from `obj.parent()`:
includes via the existing `collect_includes(parent, variant)`, defines via platform → config → parent → ObjectFile (last wins for any TU overrides), std via
ObjectFile's `std_data` falling back to parent → toolchain. The output path stays `object_path(variant, parent_name, source)` so on-disk paths don't move.
The compile_commands entry is appended exactly as today.

`emit_cxx_objects` collapses to a gather:

```cpp
auto gather_object_outputs(cxx::Target& target, const BuildVariant& variant)
    -> std::expected<std::vector<Path>, Error> {
    std::vector<Path> out;
    for (auto& child : target.objects_data) {
        auto cached = outputs_.find(child->owner().name() + "|"
            + variant.platform->name() + "|" + variant.config->name());
        if (cached == outputs_.end() || cached->second.empty()) {
            return std::unexpected(Error{
                "object " + child->owner().name() + " not emitted before "
                + target.owner().name()});
        }
        out.push_back(cached->second);
    }
    return out;
}
```

This works because `emit_target` recurses into deps before emitting the parent (the existing `for (auto* dep : target->deps)` loop in `emit_target`). By the
time we archive/link, every child ObjectFile is in `outputs_`.

`emit_cxx_library` and `emit_cxx_program` change from "compile then archive/link" to "gather then archive/link" — the compilation already happened during the
dep walk.

### 4.5 What stays identical

- Public-include propagation rules, link semantics, alias resolution, platform/config gating logic, `compile_commands.json` shape.
- Per-variant caching key in the emitter (`name|platform|config`). ObjectFile slots into the same scheme — one identity, many variants.
- The ninja rules block at the top of `build.ninja`.
- The `cxx`, `static_library`, `shared_library`, `program` factories and their fluent surface as called from `build/build.cpp`.

## 5. Migration phases

### Phase 1 — introduce the type, keep behaviour

1. Add `cxx::ObjectFile` (header only, parallel to `cxx::Target`).
2. Add `objects_data` to `cxx::Target` alongside the existing `sources_data`. Keep `sources_data` populated as well for one phase, so the emitter can fall
   back if anything goes wrong.
3. `cxx::Target::sources(...)` constructs ObjectFiles into `objects_data` *and* keeps pushing paths into `sources_data` for now. Add the dep edge.
4. Emitter unchanged — still iterates `sources_data`. Verify nothing regresses; the new nodes are inert.

Exit criterion: `./_out/ngen-build` produces a byte-identical `build.ninja` and `compile_commands.json` to baseline.

### Phase 2 — flip the emitter

1. Add `emit_object_file`. Move the per-source body out of `emit_cxx_objects`.
2. Switch library/program emit to `gather_object_outputs`.
3. Add the dispatch arm in `emit_target` for `cxx::ObjectFile`.
4. Drop the dep walk's "skip empty output" silence for object files — they must always emit.

Exit criterion: same `build.ninja` / `compile_commands.json` content, same `_out/.../obj/<lib>/...` paths. Edge ordering inside `build.ninja` may shift
because compile edges now interleave with link edges by graph order rather than by parent grouping; that's acceptable as long as the rule contents match.

### Phase 3 — drop the legacy storage

1. Remove `cxx::Target::sources_data` and any code that reads it.
2. Remove the legacy fall-back from phase 1.
3. Confirm `cxx::Target` copy/move ctors only carry `objects_data` (and re-attach to base, as today).

### Phase 4 — optional, on demand

Expose per-TU fluent surface on ObjectFile (`define`, `warning_off`, `compile_flag`, `std`) and a way to reach an ObjectFile from a `cxx::Target` builder
chain (e.g. `lib.source("src/x.cpp").warning_off("...")`). Not done speculatively — wait until the engine actually needs per-TU overrides. The structural
fields exist either way.

## 6. Trade-offs and known costs

- **Graph size.** `Project::build_all()` grows from O(libs+programs) to O(translation units). The current project is small (low hundreds of TUs) and the
  emitter is already O(TUs × variants); this is the same big-O with one extra dep-walk hop per TU.
- **Copy semantics.** `cxx::Target`'s copy constructor today re-attaches to the base. With `objects_data` of `shared_ptr<ObjectFile>`, copying a partially
  configured `cxx::Target` (e.g. through factory return) keeps shared children — same lifetime model the rest of the framework uses.
- **Parent back-pointer.** ObjectFile holds `cxx::Target*`, not `shared_ptr`. Lifetime is fine (parent outlives child via the dep edge holding the child
  alive, and the parent's `cxx::Target` extension is owned by the parent's `build::Target`'s `ExtensionMap`). Worth a comment at the field.
- **Name length.** `parent/src/foo/bar.cpp` is verbose in error messages and ninja edge names. Acceptable — ninja already handles long names; the
  alternative (filename-stem only) collides on common names like `main.cpp`, `device.cpp`.
- **Source-shared-across-libs.** Already works today (different `obj/<lib>/` dirs). Continues to work because each parent constructs its own ObjectFile with
  a parent-prefixed name. No project-level uniqueness assertion needed.
- **Public-include propagation cost per TU.** `collect_includes` runs once per TU per variant today; that doesn't change. If it ever shows up in profiles,
  cache it on the parent per variant — but that's an optimization, not a design constraint.

## 7. Open questions deferred

- Per-TU output naming when multiple variants want a single addressable name. Currently we cache `name|platform|config`; if someone wants
  `<source>:<platform>:<config>` phony aliases like roots get today, that's a small follow-up emit pass over `objects_data`. Not part of this plan.
- Whether `cxx::Target::sources_data` deserves to live on as a read-only view (e.g. for tooling that wants the raw list). Cheap to add a getter that walks
  `objects_data` — defer until something actually asks for it.

## 8. Touch list

Files expected to change:

- `build/framework/cxx/target.hpp` — add `objects_data`, change `sources(...)` impl, drop `sources_data` (phase 3).
- `build/framework/cxx/objectfile.hpp` — new, hosts `cxx::ObjectFile`.
- `build/framework/backendninja.hpp` — split `emit_cxx_objects`, add `emit_object_file`, add dispatch arm.
- `build/build.cpp` — no changes; the fluent API at the call site is unchanged.

Files that stay untouched: `build/framework/target.hpp`, `build/framework/project.hpp`, `build/framework/platform.hpp`, `build/framework/configuration.hpp`,
`build/framework/extensionmap.hpp`, `build/framework/alias.hpp`, `build/framework/tool.hpp`, `build/framework/inspect.hpp`,
`build/framework/cxx/{platform,configuration,toolchain,backendninja}.hpp`, `build/bootstrap.cpp`, `build/prebuild.cpp`.
