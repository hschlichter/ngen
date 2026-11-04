# 35. RPC: JSON-RPC 2.0 over loopback TCP with binary attachments

Date: 2026-09-29

## Status

Accepted

## Context

Nothing could reach a running ngen-view. Agents drove it offline through flags, `--script` verbs, observation files and dump files (ADR 24). The tool
architecture (ADR 34) splits the engine across processes and wants everything a human can do to be callable by an agent, so the processes need one
protocol.

The core had to be usable from programs that are not the engine — at the time the planned build server, today the asset server and tools — so it
could depend on nothing but the standard library and one JSON library.

## Decision

- Messages are JSON-RPC 2.0: requests, notifications and responses; either side of a connection may send requests.
- Frames are `u32 jsonLength`, `u32 attachmentLength` (little-endian), the JSON text, then the attachment bytes. Attachments were fixed in the frame
  format from the start, so bulk data never needs a format change.
- TCP on 127.0.0.1 only, on a port the OS picks, behind a small transport layer. No authentication.
- JSON is nlohmann/json, vendored as a submodule; only `.cpp` files include it, headers use `json_fwd.hpp`.
- Two libraries: `rpccore` (frames, protocol, sockets, server, client, discovery, records — standard library and JSON only) and `rpc`, the engine
  layer (method registry, verbs, endpoint, trace stream).

Rejected: hand-written JSON (about 500 lines to own); picojson, RapidJSON, glaze and jsmn (nlohmann won as header-only and standard-library-only).

## Consequences

- Easier: any process or agent, in any language, can drive the engine.
- Easier: the asset server streams packed bytes as attachments with no protocol change (ADR 43).
- Harder: no security; any local process can call any endpoint.
- Harder: the view's bulk results (screenshots, captures) still go as files or base64, not attachments.

## Verification

- Sources: [plan_rpc.md](../plan_rpc.md) decisions 1–2, [plan_tool_architecture.md](../plan_tool_architecture.md) decisions 1, 2 and 11,
  [src/rpc/README.md](../../src/rpc/README.md).
- Code: `src/rpc/core/rpcsocket.h:7` and `src/rpc/core/rpcsocket.cpp:21` (loopback only); `src/rpc/README.md:19`–`25` (frames, messages, errors);
  `build.cpp:107` (`rpccore`), `:126` (`rpc`).
- Differs from plan: `src/rpc/README.md:14` still says the core stays engine-free "so the build server (`src/build/`) can use it"; the build server
  was never built (ADR 42).
