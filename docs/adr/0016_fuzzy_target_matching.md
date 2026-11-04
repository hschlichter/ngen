# 16. Targets resolved by exact name, then fuzzy substring

Date: 2026-05-03

## Status

Accepted

## Context

Once every translation unit and library had a unique graph name ([ADR 13](0013_language_agnostic_build_core.md),
[ADR 14](0014_translation_units_as_graph_nodes.md)), people wanted to type a fragment such as `render` or `rhivu` instead of an exact edge name or
output path. Internal libraries were no longer reachable by name at all.

## Decision

`build::ir::resolve_target` is the single source of truth for turning a query into targets:

1. an exact edge name wins;
2. otherwise, a case-insensitive substring match on object-file source names, expanding to every hit;
3. otherwise, a substring match on the other edge names;
4. otherwise, the query is passed on, and the runner reports an unknown target.

Resolution is silent.

Alternatives considered:

- Picking a single best match and asking when it is ambiguous.
- A manifest of target names written by the graph stage.

## Consequences

- `ngen-build … render` compiles every `render*.cpp`, and internal libraries can be built by name again.
- A broad query can quietly build much more than intended.

## Verification

- Sources: [plan_fuzzy_target_matching.md](../plan_fuzzy_target_matching.md), [src/build/README.md](../../src/build/README.md).
- Code:
  - `src/build/ir/resolve.hpp:74` (`resolve_target`).
  - `src/build/bootstrap.cpp:306-315`: positional targets go through it.
- Differs from plan:
  - The plan resolved a query to one best match and wrote a `targets.txt` manifest per variant. The code expands to every match with a pure function
    over the IR; the plan's status header says so.
