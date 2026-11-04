# 62. Runtime access and visibility as a standing rule

Date: 2026-10-15

## Status

Accepted

Supersedes [ADR 63](0063_engine_map_and_scene_flow_windows.md): since 2026-10-31, where a part sits in the engine's flow is stated in its library's
README, not as a node in the Engine Map.

## Context

ngen is a learning and platform-building exercise as much as an engine, and its work is shared between a human and AI agents. The roadmap's
objective (as of 2026-10-14) is that everything is reachable and visible: agents verify changes by reading the engine's state, and the human learns
and debugs by looking at it. Settings that only a launch flag or a constant in code could reach were gaps, and each feature decided for itself
whether it had a record, a method or a window.

## Decision

Two requirements hold for every change, planned or not, written into `AGENTS.md` ("Runtime access and visibility"):

1. Everything is reachable at runtime by an agent: state is readable as records, and behaviour is drivable through RPC methods and script verbs.
2. Everything is visible to a human in the UI: a window, view or overlay shows what a part does and holds, and says where in the engine's flow it
   sits.

Every plan has a Visibility section ("None" with its reason, never left out). A change says what records, methods, verbs and windows it adds, and
names a gap rather than leaving it silent. Each vertical slice step ends with a visibility pass. Where a part sits in the flow is written in its
library's README (since 2026-10-31; first it was a node in the Engine Map).

## Consequences

- Easier: agents can verify almost anything headless, from records and the trace ([ADR 46](0046_records_and_ngen_introspect.md),
  [ADR 48](0048_trace_is_flow_data_is_record.md)).
- Easier: it drove the work that made state reachable: configuration values ([ADR 74](0074_configuration_values.md)), tool state records, the LOD
  and Streaming windows.
- Harder: every change costs a record, a method, a verb and a window or overlay.
- Harder: the UI grows; that is accepted as long as the UI stays a layer that can be cut ([ADR 75](0075_engine_layering.md)).

## Verification

- Sources: `AGENTS.md` ("Runtime access and visibility", "Plan documents"), [roadmap.md](../roadmap.md),
  [plan_lean_pass.md](../plan_lean_pass.md) (step 0).
- Code: `AGENTS.md:48` (the rule); records registered per layer (`src/introspect/README.md`); records such as `config`, `debugcamera`,
  `lodtool` and `streamingtool` exist.
- Differs from plan: none for the rule itself. `docs/roadmap.md:31` and `:210` still mention Engine Map nodes.
