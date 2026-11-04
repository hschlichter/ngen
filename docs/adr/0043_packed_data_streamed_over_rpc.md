# 43. Packed data streamed over RPC; the view needs a server

Date: 2026-10-01

## Status

Accepted

## Context

With packing in its own server ([ADR 42](0042_separate_asset_server.md)), clients needed a way to get packed data. Reading the server's cache
files directly couples every client to the server's disk layout and to running on the same machine. Over a shared connection, large assets must
not hold back small ones, and a slow client must not fill the RPC server's queue — it drops connections past 256 megabytes queued.

## Decision

- A client sends `asset.request {ids, have, force}`; the server answers at once.
- The packed bytes then arrive as `asset.data` notifications carrying chunks of at most one megabyte as RPC attachments
  ([ADR 35](0035_json_rpc_over_loopback_tcp.md)), interleaved across assets, followed by `asset.ready` or `asset.failed` per asset.
- At most eight megabytes are queued per connection; more is sent as the connection drains.
- Clients never read the server's files.
- ngen-view connects to its variant's server at start-up and exits with code 1 when there is none. Every asset it uses — shaders, scene packs,
  meshes, materials, textures — comes through that connection.

## Consequences

- Easier: the view does not care where the server runs or how it stores its cache; a view on another machine is possible in principle.
- Easier: one transport for every asset kind, with no shared-file coupling.
- Harder: there is no offline or shipping run without a server.
- Harder: every view start streams everything again; there is no client-side cache.
- Harder: throughput is bounded by the connection — about three quarters of a gigabyte per second was measured when the texture packer landed.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_asset_server.md](../plan_asset_server.md) (decisions 8 and 9), [plan_tool_architecture.md](../plan_tool_architecture.md)
  (decision 4), [src/asset/README.md](../../src/asset/README.md).
- Code: `src/asset/server/assetserver.h:47` (`packChunk`, one megabyte) and `:48` (`packWindow`, eight megabytes),
  `src/apps/view.cpp:277` (the view connects to its variant's server and returns 1 when it can't), `build.cpp:485` (ngen-view depends on the
  asset server target), `src/rpc/core/rpcserver.h:35` (256 megabytes queued at most per connection).
- Differs from plan: none found. [plan_pack_rules.md](../plan_pack_rules.md), before this decision, had the view read packed files from disk.
