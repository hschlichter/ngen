# 80. No project defaults; a build without targets builds everything

Date: 2026-11-07

## Status

Accepted

Supersedes [ADR 18](0018_no_project_defaults_in_build_system.md)

## Context

[ADR 18](0018_no_project_defaults_in_build_system.md) kept project knowledge out of `src/build/` by requiring the platform and configuration on
every call, but left one project-level default: `default_target()`, which `build.cpp` set to `ngen-view`. A build without a target therefore built
only the viewer. The cli, the asset server, the packers and the examples went stale unless named, which `src/rhi/README.md` and the root
`README.md` had to warn about, and `ngen-cli build -p <platform> -c <config>` did not build the variant it names.

## Decision

Locked by Henrik on 2026-11-07:

- `ngen-build` requires both `-p` / `--platform` and `-c` / `--config` on every call. If either is missing, it prints the help and the project's
  listing (platforms, configurations, top-level targets) and exits with an error. Unchanged from ADR 18.
- The build system has no project-level defaults. `Project::default_target` is removed; a project registers its entry points with `target` only.
- A build without targets builds every registered target except global tools (`format`, `tidy`). Global tools work on the source tree rather than
  produce the variant's outputs, so they run only when named; `--list` marks them `(only when named)`.
- Convenience belongs above the build system: `ngen-cli` remembers a chosen variant ([ADR 33](0033_ngen_cli_front_door.md)). Unchanged from ADR 18.

Rejected: keeping `default_target` as an optional override, a bare build building everything only when a project names none (the ninja
convention; it keeps a feature this project does not use); an `all` alias in `build.cpp` made the default (every new target would have to be
added to it by hand).

## Consequences

- Easier: a bare build leaves nothing in the variant stale; the cli, the tools and the examples are as current as the viewer.
- Easier: one fewer concept in the framework, and nothing project-specific in what a bare build means.
- Harder: a bare build takes longer, the examples and the OpenUSD tools included; building only the viewer means naming it
  (`ngen-cli build ngen-view`).
- The IR format is unchanged: its default-target list holds every non-global root instead of one.

## Verification

- Sources: no plan; decided in conversation. `src/build/README.md` (CLI, "When no positional target is given").
- Code:
  - `src/build/bootstrap.cpp:297`: both flags required; the listing is printed when either is missing.
  - `src/build/ir/emit.hpp:222-231`: the default-target list is every root except global tools.
  - `src/build/framework/project.hpp`: `target` is the only way to register an entry point.
  - `src/build/framework/inspect.hpp:64-66`: `--list` marks global tools `(only when named)`.
  - `build.cpp:587`: `p.target(view)`, registered like every other program.
