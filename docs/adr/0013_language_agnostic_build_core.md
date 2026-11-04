# 13. A language-agnostic build core with a `build::cxx` extension

Date: 2026-05-02

## Status

Accepted

## Context

The second build-system design mixed graph structure, build context and C++ behaviour inside `Graph`, `Target`, `Library` and the toolchain type, so
C++ details leaked everywhere. Adding a language or changing the shape of the toolchain meant editing the core. Internal libraries also cluttered the
set of target names a user could invoke.

## Decision

- The core types — `Target`, `Project`, `Platform`, `Configuration` — carry no language vocabulary. Each owns an `ExtensionMap` keyed by
  `std::type_index`.
- C++ mirrors them in a parallel module: `cxx::Target`, `cxx::Platform`, `cxx::Configuration` and `cxx::Toolchain` each wrap a core type and attach
  themselves to it as an extension.
- `cxx::Toolchain` only names the tools (compiler, archiver, linker, default standard). Compile and link flags live on the C++ platform and
  configuration.
- Only targets registered with `Project` are entry points; internal targets are found by walking dependencies from them.

Alternatives considered:

- The previous inheritance hierarchy.
- Toolchain shapes: a virtual base class, a struct of closures, or a fully data-driven design — rejected or deferred.
- Side tables or global lookups for extension data.

## Consequences

- Adding a language is purely additive, and toolchain decisions stay inside `framework/cxx/`.
- A wrapper must re-attach itself whenever it is moved or copied, or the extension points at a dead object.
- Internal libraries could no longer be built by name; fuzzy matching later made them reachable again
  ([ADR 16](0016_fuzzy_target_matching.md)).

## Verification

- Sources: [plan_build_system_v3.md](../plan_build_system_v3.md), [plan_build_system_v4.md](../plan_build_system_v4.md),
  [build_system_design_target_graph_project_model.md](../build_system_design_target_graph_project_model.md),
  [build_system_extension_type.md](../build_system_extension_type.md), [src/build/README.md](../../src/build/README.md).
- Code:
  - `src/build/framework/extensionmap.hpp:31-36` (lookup by `std::type_index`).
  - `src/build/framework/platform.hpp` and `configuration.hpp` carry identity only.
  - `src/build/framework/cxx/` holds `target.hpp`, `platform.hpp`, `configuration.hpp`, `toolchain.hpp`.
- Differs from plan:
  - The plans had wrappers hold the core `Target` through `unique_ptr`; the code uses `shared_ptr` (`src/build/framework/tool.hpp:132`).
  - v3 and v4 kept `Tool` and `Alias` as `Target` subclasses dispatched by `dynamic_cast`. In the code they are `ExtensionMap` wrappers like the
    C++ types, and a third wrapper, `Phony`, was added (`src/build/framework/phony.hpp:11`, `alias.hpp:3`, `tool.hpp:3`).
