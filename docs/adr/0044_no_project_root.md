# 44. No project root: tools are rooted at their working directory

Date: 2026-10-02

## Status

Accepted

## Context

The first asset server and RPC code found their paths from the binary's location: the cache in `_out/<platform>/<config>/assets/`, discovery files
in `_out/run/`, and a `rpcProjectRoot` that walked up to the repository. That tied every tool to the repository layout. A binary copied elsewhere,
or a scene in another folder, did not work.

## Decision

- The asset server's working directory is its root: asset ids are paths relative to it ([ADR 39](0039_asset_ids_are_paths.md)), packers run in
  it, its cache is `.ngen-assets/<platform>/<config>/` in it, and its discovery file is in `.ngen-discovery/` in it.
- Every RPC endpoint writes its discovery file in its own working directory; readers look in theirs. A view finds the server running in the same
  directory.
- The only thing taken from the binary's location is the variant: its folder names the platform and configuration.

## Consequences

- Easier: binaries work copied anywhere. A debug build copied to an unrelated folder rendered the baseline image.
- Easier: two projects, or two scene folders, can each run their own server without interfering.
- Harder: a view and its server must be started in the same directory, or the view finds no server and exits.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_asset_server.md](../plan_asset_server.md) and [plan_asset_copy.md](../plan_asset_copy.md) (the "changed after landing" notes at
  the end of their Results), [src/asset/README.md](../../src/asset/README.md), [src/rpc/README.md](../../src/rpc/README.md) ("Discovery").
- Code: `src/asset/server/assetserver.cpp:123` (`.ngen-assets / platform / config`), `src/apps/assetserver.cpp:3` (the working directory is the
  root), `src/rpc/core/rpcdiscovery.cpp:23` (`current_path() / ".ngen-discovery"`), `src/apps/view.cpp:278` (the variant from `/proc/self/exe`).
  `rpcProjectRoot` no longer exists.
- Differs from plan: [plan_tool_architecture.md](../plan_tool_architecture.md) (decisions 3 and 4), [plan_rpc.md](../plan_rpc.md) (decision 7) and
  [plan_pack_rules.md](../plan_pack_rules.md) (decision 7) still give the `_out/` paths.
