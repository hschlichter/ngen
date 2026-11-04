# 42. A separate `ngen-asset-server`; the build stays one-shot

Date: 2026-10-01

## Status

Accepted

Supersedes [ADR 37](0037_packing_inside_build_system.md)

## Context

Packing — turning a source file into engine-ready data — needed a long-running process: one that packs on request, caches the results and
streams them to the tools that asked. Assets are discovered at runtime (a file dropped into a folder, referenced from a scene), so they cannot be
listed in `build.cpp`. Packing inside the build system ([ADR 37](0037_packing_inside_build_system.md)) required exactly that list, and mixed
packing into a framework meant to stay standard-library-only and liftable ([ADR 32](0032_build_system_in_src_std_only.md)).

Two earlier designs were planned and not built. A cooked-asset cache with async loading inside the engine (`src/assetsystem/`, an `ngen-cook`
tool) would have pulled pxr, shaderc and stb into the engine process. A build server (`ngen-build --serve` as the default, request-driven packing as
runner edges, graph reload, a lock per variant) would have made `src/build/` depend on `src/rpc/` and turned the build system into a daemon.

## Decision

Builds and packing are separate concerns.

- `ngen-build` stays one-shot, with no server mode and no packing.
- `ngen-asset-server` is its own program, one binary per variant, started by hand. It uses nothing from `src/build/`: it has its own cache, its own
  job runner and the RPC core.
- The pack rules live in the root `pack.cpp`, compiled into the server. Each rule maps file extensions to a packer program
  ([ADR 38](0038_one_packer_program_per_type.md)), its parameters and a version. Two rules claiming one extension stop the server at start-up.
- Nothing is packed without a request: no packing at start-up, no repacking when a rule changes. Requests for the same id in flight share one job.
  Pack jobs run on one worker per hardware thread.
- `build.cpp` builds the server and the packer programs as ordinary targets and lists no assets.

## Consequences

- Easier: `src/build/` stays standard-library-only, and the build has no daemon.
- Easier: adding an asset needs no build change; any file with a known extension can be requested.
- Harder: a job runner, a content hash and a depfile parser now exist twice, in the build runner and in the asset server.
- Harder: changing a rule in `pack.cpp` means rebuilding and restarting the server; the server does not rebuild packers.
- Harder: nothing stops a second server for the same variant in the same directory.
- A unified build and asset server is kept as a possible end state, not a plan.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_asset_server.md](../plan_asset_server.md), [plan_build_server.md](../plan_build_server.md) (superseded),
  [plan_async_asset_system.md](../plan_async_asset_system.md) (superseded), [plan_tool_architecture.md](../plan_tool_architecture.md) (decision 7),
  [src/asset/README.md](../../src/asset/README.md).
- Code: `pack.cpp:7` (`packRules`: shader, texture, usd, copy), `src/asset/server/assetserver.cpp:110` (two rules for one extension stop the
  server), `src/asset/server/assetserver.cpp:137` (one worker per hardware thread), `build.cpp:330` ("Nothing here lists assets"),
  `build.cpp:418` (`ngen-asset-server` built from `pack.cpp` and `src/asset/server/*.cpp`, linking no build-system code).
- No `src/build/serve`, `ngen-build-serve` or `src/assetsystem/` exists, in the tree or in its history.
- Rejected alternatives: the build server; the in-engine cooker; the server compiling or loading `pack.cpp` at runtime (it would repeat what
  `ngen-build-graph` does for `build.cpp`).
