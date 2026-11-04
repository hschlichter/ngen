# 38. One packer program per asset type, with a fixed contract

Date: 2026-09-29

## Status

Accepted

## Context

Packers need heavy dependencies — OpenUSD for scenes, stb for textures, shaderc or glslc for shaders — that must not leak into the build system, the
asset server or the engine. A packer that crashes on a bad file must not take the server down with it. And the cache needs to know exactly what a
packed file was made from, including the packer itself.

## Decision

Each asset type is packed by its own program — `ngen-packer-shader`, `ngen-packer-texture`, `ngen-packer-usd`, `ngen-packer-copy` — run by the asset
server with `posix_spawn` and no shell, under one command line:

```
<packer> --rule <name> --rule-version <n> --asset <id> --source <path> --out <file> --depfile <file> [--param key=value]...
```

- A packer reads `--source`, writes the packed asset to `--out`, and writes a Make-format depfile listing every file it read. Its exit code is the
  result; a failure message goes to stderr.
- Secondary outputs — assets the same source defines, with ids `<asset>#<fragment>` — are written beside `--out` and listed in `<out>.outputs`. This
  was added for the USD packer, which writes a layer pack plus one asset per mesh and material (ADR 52).
- The packer binary's size, time and content hash are part of the cache record, so a rebuilt packer repacks what it made.

Rejected: packers as plugins inside the server (would pull OpenUSD into it). Deferred: long-running packer workers, until process start-up shows in
pack times.

## Consequences

- Easier: isolation — a packer's dependencies and crashes stay in its own process.
- Easier: exact invalidation, since every input including the packer is hashed.
- Easier: a new asset type is a new program and a rule in `pack.cpp`.
- Harder: a process start per asset.

## Verification

- Sources: [plan_pack_rules.md](../plan_pack_rules.md) decision 2, [plan_asset_server.md](../plan_asset_server.md) decision 5,
  [plan_texture_packer.md](../plan_texture_packer.md) decision 5, [src/asset/README.md](../../src/asset/README.md).
- Code: `src/asset/pack/packer.h:12`–`22` (the contract), `src/asset/server/packjobs.cpp:83` (`posix_spawn`), `src/asset/server/assetcache.h:15`–`17`
  (packer binary in the record), `build.cpp:332`–`334` and `:347` (packer programs), `pack.cpp:11`–`46` (rules naming them).
