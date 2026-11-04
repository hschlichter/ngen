# 45. The asset system is a lower layer, used through `AssetClient`

Date: 2026-10-02

## Status

Accepted

## Context

Once USD layers and textures came from the asset server, something in the scene code had to reach the server. At the time ngen-view still opened
a USD stage ([ADR 6](0006_usd_stage_is_the_scene.md)), so USD's own loading had to fetch through the server without being changed. The question
was how the scene and the asset system relate: does the scene call the asset system, or does the asset system call back into code the scene
provides?

## Decision

- The asset system is a lower layer than the scene, the renderer and the view. They take an `AssetClient&` and call it directly; there is no
  callback indirection and no interface the asset system calls back into.
- `AssetClient` depends only on the RPC core.
- First mechanism (superseded): a USD `ArResolver` inside the scene library called `AssetClient`, with asset ids as resolved identifiers, so
  USD loading went through the server unchanged. A copy packer delivered source files as packed assets, using a reflink (`FICLONE`) instead of a
  hard link. A scene's textures were requested in one batch so the server packed them in parallel.
- When the view stopped linking OpenUSD ([ADR 52](0052_view_renders_layer_packs.md)), the resolver left with it; the view now asks `AssetClient`
  for packs, meshes, materials and textures by id.

## Consequences

- Easier: one direction of dependency; the asset system can be understood and tested without any engine code.
- Easier: the renderer's pipeline registry and the view's scene thread use the same client the same way.
- Easier: reflinks cost no disk space on btrfs or XFS, and an in-place edit of a source cannot change a packed file behind the cache's back.
- Harder: the resolver path cost 4 to 13 percent in load time while it existed.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_asset_copy.md](../plan_asset_copy.md), [plan_texture_batch.md](../plan_texture_batch.md),
  [plan_texture_packer.md](../plan_texture_packer.md), [src/asset/README.md](../../src/asset/README.md).
- Code: `build.cpp:133` (`assetclient` links only `rpccore`), `src/view/viewscene.h:279` (`ViewScene(AssetClient&)`),
  `src/renderer/pipelineregistry.h:89` (`init` takes an `AssetClient*`), `src/apps/packercopy.cpp:70` (`FICLONE`, falling back to a byte copy).
- Differs from plan: the resolver now survives only in the unbuilt editor code, `src/editor/scene/usdassetresolver.*`. The copy packer is left for
  `.hdr` files only (`pack.cpp:40`).
- Rejected alternatives: callbacks between scene and asset system; a separate resolver shared library; hard links.
