# 32. The build system lives in `src/build/`, standard library only

Date: 2026-09-29

## Status

Accepted

## Context

All source lived under `src/` except the build system, which sat in `build/` at the repository root with its documentation in
`build/build_system.md`. The then-planned build server was to depend on `src/rpc/` like the other tools, and moving the framework had to keep the
goal it was written for: a fluent C++ build framework that can be lifted into another project (ADR 11).

## Decision

- `git mv build src/build`; `build_system.md` became `src/build/README.md`, following the per-library README rule.
- The root `build.cpp` stays at the root as the project's description, the way a `CMakeLists.txt` would.
- `framework/`, `ir/` and `run/` use only the standard library (plus POSIX and the vendored `ir/xxhash.h`). Nothing under `src/build/` includes engine
  code, and no engine target puts `src/build/` on its include path.
- Dependency direction inside: `ir/` depends on `framework/`, `run/` on both, `bootstrap.cpp` on `ir/` and `run/`; never the reverse.
- The README holds the architecture; each header's prose comment holds its details.
- `src/build/example/` is a separate small project built by the framework, proving it stands alone.

The build server that motivated part of the move was never built (ADR 42); the rule that the build system takes no engine dependency held anyway.

## Consequences

- Easier: lifting the build system is "copy `src/build/` and write a new `build.cpp`", and the example shows it working.
- Easier: the source tree has one rule — everything under `src/` — with no outlier.
- Harder: the build system has no RPC, records or trace, so it is the one part of the toolchain an agent cannot read at runtime (a gap against
  ADR 62, accepted by this decision and ADR 42).
- Harder: historical plans keep their stale `build/` paths, by design.

## Verification

- Sources: [plan_build_into_src.md](../plan_build_into_src.md), [plan_build_documentation.md](../plan_build_documentation.md).
- Code: `src/build/README.md:40`–`53` (layout, "copy `src/build/` and write a new `build.cpp`", dependency direction); `src/build/example/`;
  `rg '#include "(\.\./)+(rpc|renderer|asset|trace)' src/build` finds nothing.
