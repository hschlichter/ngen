# 36. One endpoint per process, discovery files, no broker

Date: 2026-09-29

## Status

Accepted

Amended by [ADR 44](0044_no_project_root.md): discovery files live in `.ngen-discovery/` in the working directory, not `_out/run/` under a project root.

Amended by [ADR 46](0046_records_and_ngen_introspect.md): the planned `ngen-rpc` command line was folded into `ngen-introspect`.

## Context

With several processes (ADR 34), each has to find the others without a central service. Engine data belongs to the main thread, which must never be
stalled by a client, and script verbs and live calls had separate code paths (ADR 24).

## Decision

- Every long-running process runs one symmetric endpoint; either side of a connection can call the other.
- An endpoint announces itself with a discovery file (kind, pid, port, label, protocol, start time) and is reached directly. There is no broker.
- An I/O thread queues incoming requests; the thread that owns the data runs them with `drain()`, for at most 2 milliseconds per frame (at least one
  call). Handlers receive an `RpcResponder` and may complete later.
- Methods are named `area.verb`, with typed parameter schemas checked before the handler runs. `rpc.describe` lists every method with its schema.
- `--script` verbs are parsers onto these same methods: one implementation per command, used live and offline.
- `--no-rpc` turns ngen-view's endpoint off.

Deferred: a broker, until several engines or several machines are involved. Rejected later: a separate outgoing-connection class (`RpcServer::connect`
on the same I/O loop instead).

## Consequences

- Easier: a call takes effect within one frame, and a flood of calls cannot starve a frame.
- Easier: agents discover the whole surface through `rpc.describe`.
- Easier: verbs and methods cannot diverge.
- Harder: records are polled; the trace is the only subscription (ADR 47).
- Harder: processes find each other only when started in the same directory (ADR 44).

## Verification

- Sources: [plan_rpc.md](../plan_rpc.md) decisions 3–9, [plan_tool_architecture.md](../plan_tool_architecture.md) decisions 3 and 10,
  [plan_asset_server.md](../plan_asset_server.md) decision 9, [src/rpc/README.md](../../src/rpc/README.md) "Discovery".
- Code: `src/rpc/core/rpcdiscovery.cpp:23` (`.ngen-discovery` under the working directory); `src/rpc/rpcendpoint.h:40` (`drainBudgetNs = 2'000'000`);
  `src/rpc/rpcregistry.cpp:151` (`rpc.describe`); `src/rpc/rpcverbs.h:16` (`RpcVerbs`); `src/apps/view.cpp:206` (`--no-rpc`).
- Differs from plan: the plan's `_out/run/<kind>-<pid>.json` with a project-root field and the `ngen-rpc` tool (`src/apps/rpc.cpp`) no longer exist.
