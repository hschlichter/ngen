# 12. Ninja as executor behind a multi-stage bootstrap

Date: 2026-04-25

## Status

Superseded by [ADR 15](0015_custom_runner_and_binary_ir.md) and [ADR 17](0017_one_runner_one_command_seed.md)

## Context

The C++ build description ([ADR 11](0011_build_described_in_cpp.md)) needed something to schedule work, run it in parallel and decide what was out
of date. Delegating that to Ninja meant the framework did no timestamp or hash tracking of its own. The build programs also had to rebuild themselves
when `build.cpp` or the framework changed, and each stage was meant to be a separate debugging boundary.

## Decision

- A hand-written `bootstrap.ninja` builds a `prebuild` program, which builds `ngen-build`.
- The second plan grew this to four stages: `bootstrap.ninja` → `ngen-build` → `ngen-build-pre` → `ngen-build-graph` → `_out/build.ninja`.
- Every invocation runs the whole chain; Ninja makes the no-change case fast.
- An in-process backend was planned for a later phase.

Alternatives considered:

- Two stages, with the self-compile rules inside `build.cpp`: rejected, because it merges the debugging boundaries.
- Timestamp heuristics in ngen's own code.

## Consequences

- A fast path to parity with the Makefile.
- The project depended on Ninja.
- Four binaries and several generated `.ninja` files.
- The boundary between "produce the graph" and "run the graph" was Ninja syntax, a format ngen did not own.
- A change to `prebuild` needed a second run to take effect.

Ninja was replaced for the project graph in May 2026 ([ADR 15](0015_custom_runner_and_binary_ir.md)) and removed from the bootstrap shortly after
([ADR 17](0017_one_runner_one_command_seed.md)).

## Verification

- Sources: [plan_build_system.md](../plan_build_system.md) §6–7, [plan_build_system_v2.md](../plan_build_system_v2.md) §6–7,
  [plan_build_system_v3.md](../plan_build_system_v3.md) §12.
- Code: no `.ninja` files and no `prebuild.cpp` exist. The only mention of Ninja under `src/build/` is a comment in `src/build/ir/emit.hpp:570`
  that mirrors one of its naming conventions.
