# 17. One runner for self-build and project build, one `c++` seed

Date: 2026-05-13

## Status

Accepted

Supersedes [ADR 12](0012_ninja_executor_and_staged_bootstrap.md) for the bootstrap.

## Context

After the custom runner replaced Ninja for the project graph ([ADR 15](0015_custom_runner_and_binary_ir.md)), Ninja still drove the build system's
own bootstrap through `bootstrap.ninja` and `prebuild`. Two execution engines meant two sets of rules for deciding what is out of date.

## Decision

- `ngen-build` builds a two-edge IR in memory (`self_build_ir()`): compile `build.cpp` into `ngen-build-graph`, and the runner's `main.cpp` into
  `ngen-build-run`.
- It executes that IR in-process with the runner library (`ngen::run::execute()`), tracking headers through `-MMD` depfiles, with its own build log at
  `_out/.system/.ngen-buildlog`.
- It then runs the graph stage and the runner as subprocesses for the project.
- The bootstrap is one documented `c++` command that compiles `src/build/bootstrap.cpp`. There is no bootstrap script, Makefile or Ninja file.

Alternatives considered:

- An intermediate stat-and-compile loop inside `ngen-build` (`plan_remove_ninja_from_bootstrap.md`): superseded before it was implemented, because
  the runner had become reliable enough to use directly.

## Consequences

- One scheduler, one build-log format and one process abstraction.
- A fresh clone needs one command to start.
- The runner cannot rebuild the bootstrap itself: a change to `bootstrap.cpp` or to the IR schema means re-running the seed command by hand.

## Verification

- Sources: [plan_unified_runner.md](../plan_unified_runner.md), [plan_remove_ninja_from_bootstrap.md](../plan_remove_ninja_from_bootstrap.md),
  [plan_custom_build_backend.md](../plan_custom_build_backend.md) §14, [src/build/README.md](../../src/build/README.md), `AGENTS.md` "Build".
- Code:
  - `src/build/bootstrap.cpp:10-13` (the self-build described), `:177` (`self_build_ir`), `:212-224` (in-process `execute`).
- Differs from plan:
  - The plan had a two-command seed and handed the self-build IR to the `ngen-build-run` binary; the code needs one command and runs the runner
    library in-process.
  - The plan had one build log with separate key ranges; the code keeps a separate log under `_out/.system/`.
