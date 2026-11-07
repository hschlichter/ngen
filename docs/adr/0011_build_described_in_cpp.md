# 11. The build is a C++ program written against a reusable framework

Date: 2026-04-25

## Status

Accepted

## Context

The engine was built by an 80-line `Makefile`. It was about to need several programs, static and shared libraries, code-generation tools (`glslc`),
and a matrix of platforms and configurations: linux-vulkan with debug, release and gamerelease, and possibly a Wasm/WebGPU platform later.

A configuration language or interpreter (Make, CMake, Meson) adds a second language and a runtime dependency. The build description should be
debuggable with an ordinary debugger, and the framework should be a product that can move to another project, not something specific to ngen.

## Decision

- The project's build description is the root `build.cpp`, an ordinary C++ program.
- It builds an in-memory target graph through a fluent API (`cxx::program(...).sources(...).link(...)`) from a header-only framework that knows
  nothing about the engine.
- Platform and configuration are separate, orthogonal axes. Each variant's outputs go to `_out/<platform>/<config>/`.
- The build programs are debugged by attaching gdb; there is no separate introspection surface.

Alternatives considered:

- Keeping the Makefile.
- CMake or Meson: rejected — no new language and no interpreter dependency.
- A generic raw-rule language inside C++: rejected in favour of typed targets and a `Tool` type.
- The minimal tuple-based sketch in `build_concept.md`: not adopted.

## Consequences

- Loops, functions, type checking and IDE support work in the build description, and it can be stepped through in gdb.
- One `build.cpp` describes every variant.
- Every change to `build.cpp` recompiles the graph program (`ngen-build-graph`).
- Abstracting the toolchain stayed hard; it ended as a small `cxx::Toolchain` that only names the tools.
- The framework supports C++ only. Other languages are possible as extensions ([ADR 13](0013_language_agnostic_build_core.md)), but none exist.

## Verification

- Sources: [build_concept.md](../build_concept.md), [plan_build_system.md](../plan_build_system.md), [plan_build_system_v2.md](../plan_build_system_v2.md),
  [src/build/README.md](../../src/build/README.md).
- Code:
  - `build.cpp:43-87`: the platform, the three configurations and their registration.
  - `build.cpp:587`: `p.target(view)`; a build without targets builds every registered target ([ADR 80](0080_build_without_targets_builds_everything.md)).
  - `src/build/framework/configuration.hpp:8`: `variant.out_dir = root / platform / config`.
  - The `Makefile` no longer exists.
- Differs from plan:
  - v1's class hierarchy (`Program`, `StaticLibrary`, `SharedLibrary`, `Graph`) does not exist; v3 and v4 replaced it
    ([ADR 13](0013_language_agnostic_build_core.md)).
