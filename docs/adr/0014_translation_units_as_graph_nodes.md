# 14. Every translation unit is a build graph node

Date: 2026-05-03

## Status

Accepted

## Context

A target's sources were a flat list of paths inside `cxx::Target`. They had no identity, could not be configured one by one, and did not exist in the
graph. Some files needed different flags — the files that include OpenUSD must build as C++20 ([ADR 3](0003_openusd_vendored_not_tinyusdz.md)) —
and building a single file by name was impossible.

## Decision

- Each `.cpp` becomes a `build::Target` carrying a `cxx::ObjectFile` extension, and the library or program that owns it depends on it.
- Compile edges are emitted from the object-file node; archive and link edges from the owner.
- Per-file overrides — `define`, `warning_off`, `compile_flag`, `std` — are applied through `cxx::Target::for_source(path, fn)`.
- Optimisation, debug info and position-independent code are deliberately not per file; they belong to the platform and configuration.

Alternatives considered:

- Keeping sources as a list.
- Per-file optimisation, debug and PIC settings: rejected as platform or configuration concerns ("less is more").

## Consequences

- Compile edges get the same identity, caching and dependency walking as every other target.
- Single files can be built by name, which fuzzy matching builds on ([ADR 16](0016_fuzzy_target_matching.md)).
- The graph has many more nodes.

## Verification

- Sources: [plan_per_source_target.md](../plan_per_source_target.md), [plan_per_source_target_phase4.md](../plan_per_source_target_phase4.md),
  [src/build/README.md](../../src/build/README.md).
- Code:
  - `src/build/framework/cxx/objectfile.hpp:37` (`class ObjectFile`).
  - `src/build/framework/cxx/target.hpp:135` (`for_source`).
  - Used in `build.cpp:363` and `build.cpp:396` to build the OpenUSD-facing files as C++20.
