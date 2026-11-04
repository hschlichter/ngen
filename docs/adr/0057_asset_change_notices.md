# 57. Asset change notices: the server notifies, the client requests again

Date: 2026-10-13

## Status

Accepted

## Context

Edits to source files — a shader, a texture, a USD layer — did not reach a running view; it had to be restarted. Any fix had to keep the asset
server's rule that nothing is packed without a request ([ADR 42](0042_separate_asset_server.md)): a server that repacks on its own packs work nobody
wants, and races with requests that arrive meanwhile.

## Decision

- The server watches the input directories of everything each connection has received, with inotify behind a `FileWatcher` interface. It falls
  back to polling once a second when inotify isn't available or runs out of watches; `--watch=poll` forces polling.
- Changes are gathered for 100 milliseconds and checked against the cache's up-to-date test, so a touched but unchanged file is not reported.
- Each connection holding a changed asset gets one `asset.changed {ids}` notification. The server packs nothing; a client that still wants the
  asset requests it again.
- Every connection is subscribed by receiving; there is no explicit watch call.
- Shaders reload through the pipeline registry ([ADR 41](0041_pipeline_registry.md)); scene assets go through the view's rebuild.

## Consequences

- Easier: shader hot reload with no stall, and the request-only rule holds.
- Easier: clients decide what is worth repacking.
- Harder: inotify is Linux-only; other platforms fall back to polling.
- Harder: a scene asset change goes through the scene rebuild, which was a full re-upload when this landed.
- Harder: a source edited while it is being packed can be recorded as up to date with the old content.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_asset_change_notices.md](../plan_asset_change_notices.md), [src/asset/README.md](../../src/asset/README.md).
- Code: `src/asset/server/filewatcherlinux.cpp:47` (inotify, falling back to polling) and `:102` (`ENOSPC`), `src/apps/assetserver.cpp:28` (`--watch=poll`),
  `src/asset/server/assetserver.cpp:874` (`asset.changed` sent), `src/asset/assetclient.cpp:83` (the client takes the notice and requests nothing by
  itself).
- Differs from plan: none found.
- Rejected alternatives: polling only; repack then notify (packs unwanted work, races with requests); an explicit `asset.watch` method; notices
  for shaders only (a stale view is worse than a stall).
