# 34. Process split: USD-free view, editor, asset server, introspection tool

Date: 2026-09-29

## Status

Accepted

The editor process is not built yet; its design is [ADR 58](0058_editor_process_owns_usd.md), still proposed.

## Context

One process, ngen-view, held everything: the engine, the USD stage, editing and undo, and every introspection window. USD's API leaked into the
runtime, loads were synchronous, and nothing outside the process could reach it.

The goals behind the tool architecture: an AI builds games on engine components without changing the engine; humans and AI edit together; and
everything is async.

## Decision

ngen becomes several processes that talk over RPC (ADR 35):

- **ngen-view** consumes only packed, engine-ready data. It knows four concepts that come from USD — layer packs, sub-packs, variant sets and
  components — and links no OpenUSD.
- **ngen-editor** owns the USD stage, edits and undo (ADR 58).
- **ngen-asset-server** alone packs (ADR 42). No other process has a packing fast path; the view applies gizmo edits locally first instead.
- **ngen-introspect** shows records and traces from every process (ADR 46). ngen-view keeps its own in-depth windows.
- Library boundaries are enforced by `build.cpp`'s link graph.

Rejected: resolving variants in the packer instead of switching them live in the view (switching would need editor → repack → delta); moving the
view's in-depth windows into the introspection tool (reversed: data is best looked at where it is used).

## Consequences

- Easier: the engine libraries link no pxr, and an agent can reach every process over one protocol.
- Easier: the view could be rebuilt on packs (ADR 52) without touching editing code.
- Harder: until `ngen-editor` exists there is no USD editing at all; the in-view editor was removed when the view moved to packs, ahead of this plan's
  own order.
- Harder: how an unsaved edit becomes packed data is still open.

## Verification

- Sources: [plan_tool_architecture.md](../plan_tool_architecture.md), [plan_editor_split.md](../plan_editor_split.md), [plan_scene_pack.md](../plan_scene_pack.md).
- Code: `build.cpp:418` (`ngen-asset-server`), `:438` (`ngen-view`, whose links at `:468`–`483` include no `-lusd*`), `:497` (`ngen-introspect`). No
  `ngen-editor` target exists; `src/editor/` is kept unbuilt.
- Differs from plan: the plan is still marked Draft, still names `ngen-rpc` (folded into `ngen-introspect`) and the `_out/run/` and
  `_out/.../assets/` paths (now working-directory based, ADR 44).
