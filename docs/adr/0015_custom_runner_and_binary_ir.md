# 15. A custom runner fed by a binary IR replaces Ninja

Date: 2026-05-03

## Status

Accepted

Supersedes [ADR 12](0012_ninja_executor_and_staged_bootstrap.md) for the project build.

## Context

The project wanted to be fully self-hosted past a single seed command, with no Ninja or Make at any stage. The boundary between producing the graph
and executing it was a stream of Ninja syntax. Whatever replaced Ninja had to stay small enough for one person to understand.

## Decision

- The graph stage (`ngen-build-graph`) writes one versioned binary IR per variant to `_out/<platform>/<config>/build.ngenir`: a magic `NGIR`, a
  format version and a string table. Each edge carries a fully expanded shell command — no templates or variables — plus inputs, outputs, implicit
  and order-only dependencies, a pool, a depfile and a phony flag.
- A runner (`ngen-build-run`, header-only `src/build/run/`) executes the IR:
  - xxh3 content hashing behind a size, mtime and ctime fast path, so a build with nothing to do only stats files;
  - Make-format depfiles parsed directly;
  - `default` and `console` pools, `-j` and `-k`;
  - Ninja-style progress output.
- Graph and runner are separate processes.

Alternatives considered:

- Keeping Ninja.
- A single-process mode for the project build: deferred, then subsumed by the unified runner ([ADR 17](0017_one_runner_one_command_seed.md)).
- A watch or daemon mode: deferred.
- Reusing a Make implementation for depfiles: rejected — the format is simple, and Make brings too much with it.

## Consequences

- No Ninja dependency, and the IR is a clean unit-of-work boundary that can be dumped (`--dump-graph`).
- Content hashing means touching a file without changing it rebuilds nothing.
- The scheduler, build log and process spawning are maintained in-house, and only POSIX is implemented.
- A change to the IR format forces a re-bootstrap.
- A job runner, a content hash and a depfile parser later had to exist a second time in the asset server, which uses nothing from `src/build/`
  ([ADR 42](0042_separate_asset_server.md)).

## Verification

- Sources: [plan_custom_build_backend.md](../plan_custom_build_backend.md), [src/build/README.md](../../src/build/README.md).
- Code:
  - `src/build/ir/schema.hpp:21-22` (`kMagic`, `kFormatVersion = 3`).
  - `src/build/run/hash.hpp:8-12` (xxh3 with the stat fast path).
  - `src/build/run/depfile.hpp:1-12` (depfile parser and why Make is not reused).
- Differs from plan:
  - The plan placed the IR under `framework/ir/` with a backend IR header; the code has a top-level `src/build/ir/` with `emit.hpp`.
