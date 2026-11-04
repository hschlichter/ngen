# 3. Full OpenUSD as a vendored submodule, not tinyusdz

Date: 2026-04-02

## Status

Accepted

Supersedes [ADR 1](0001_gltf_as_first_scene_format.md) as the scene format.

Amended by [ADR 52](0052_view_renders_layer_packs.md): ngen-view no longer links OpenUSD; only the USD packer, the parity tool and the unbuilt editor
code do.

## Context

Once layering, composition, edit targets, session layers, payloads and variants became first-class requirements, a parser-only USD library was not
enough. The design notes put it as: tinyusdz "is suitable for USD-as-import; a USD-driven scene system strongly favors a fuller USD implementation".

OpenUSD is a large dependency, and its v26 headers do not compile with GCC 15's standard library in C++23 mode (`schemaRegistry.h` holds a
`unique_ptr` to an incomplete type).

## Decision

- Use OpenUSD (pxr), vendored as the git submodule `external/openusd` and built minimally with `build_usd.py` — no Python, no imaging, onetbb — into
  `external/openusd_build`. Engine tools stay free of libpython.
- Compile the files that include pxr headers as C++20 through a per-file override; the rest of the engine stays C++23.
- Keep pxr out of engine-facing headers (pimpl).

Alternatives considered:

- tinyusdz: rejected, because it is import-only, with no composition, edit targets or change notices.

## Consequences

- ngen has real USD semantics, and OpenUSD later became the reference that composition parity is checked against (`ngen-usdcheck`).
- A heavy dependency is built separately from the engine, once per machine.
- Per-file C++20 overrides are needed wherever pxr is included (see [ADR 14](0014_translation_units_as_graph_nodes.md)).

## Verification

- Sources: [usd_driven_scene_system_architecture_modern_c_engine.md](../usd_driven_scene_system_architecture_modern_c_engine.md) §21,
  [plan_usd_scene_system_implementation.md](../plan_usd_scene_system_implementation.md) §1, [usd_scene_system_status.md](../usd_scene_system_status.md) §1.
- Code:
  - `.gitmodules:13-14` (`external/openusd`).
  - `build.cpp:350-363`: ngen-packer-usd links the `-lusd_*` libraries from `external/openusd_build/lib` and builds `usdpacker.cpp` as C++20.
  - `build.cpp:390-396`: ngen-usdcheck, also C++20.
  - [src/editor/README.md](../../src/editor/README.md) describes the C++20 containment for the unbuilt editor code.
- Differs from plan:
  - The implementation plan speaks of prebuilt libraries linked through a `Makefile`; OpenUSD is a submodule built from source and linked by
    `build.cpp`.
