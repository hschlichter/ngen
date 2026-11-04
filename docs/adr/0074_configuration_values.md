# 74. Configuration: typed values declared by their owner

Date: 2026-10-31

## Status

Accepted

## Context

The engine's settings lived in UI state (`ViewUI`, `CameraWindowState`, `LodWindowState`) or as constants in code. The frame loop and the RPC
methods read them from the UI's structs, and each setting had its own `view.*` method, argument parser and record fields. The lean pass
([ADR 75](0075_engine_layering.md)) was going to make the UI a layer that can be cut, and cutting it would have taken the settings with it. The
visibility rule ([ADR 62](0062_runtime_access_and_visibility.md)) counts a setting only a flag or a constant reaches as a gap.

## Decision

- `src/config/`, a standard-library-only library beside `profile` and `trace`.
- Value types `Bool`, `Int`, `Float`, `Enum` and `Vec3`, each with a dotted name, a default, a description and an optional range. They are
  declared as variables in the system they configure and register themselves in a function-local static registry.
- Values are read and set on the main thread only, asserted in debug builds. Other threads get them through the render snapshot or requests.
- No callbacks: a value counts its changes, and code that acts on one compares the count.
- One `config` record, the methods `config.get`, `config.set` and `config.reset`, the verbs `config` and `config-reset`, and a Configuration
  window.
- The bespoke setters were removed; their verbs stay as short aliases that map onto `config.set`.

Rejected: describing the existing settings structs by field offset (descriptions drift from fields), keeping the bespoke setters beside
`config.set` (two paths to one value), and callbacks on set (code running inside `set` breaks the frame order).

## Consequences

- Easier: one way to read and set any setting, the same for agents and the UI.
- Easier: windows edit values by name without including the library that declares them.
- Easier: the names are ready to be keys in an init file.
- Harder: no persistence yet, and no string or list types.
- Harder: a name declared twice asserts at start-up; values crossing threads still need an explicit hand-off.

## Verification

- Sources: [plan_configuration.md](../plan_configuration.md), [plan_lean_pass.md](../plan_lean_pass.md) (step 1), `src/config/README.md`.
- Code: `build.cpp:98-103` (the `config` library); `src/config/config.h:13` (main thread only), `:34` (`Value`), `:44` (`changes()`), `:65-142`
  (`Bool` to `Vec3`); `src/view/configcommands.cpp:261-275` (`config.get`, `config.set`).
- Differs from plan: the plan's status still reads "In progress" although the system is built; the init file is deferred.
