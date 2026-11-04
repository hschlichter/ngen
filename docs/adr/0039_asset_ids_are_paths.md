# 39. Asset ids are paths; the version is the content hash

Date: 2026-09-29

## Status

Accepted

## Context

Packing needed a stable identity for every asset: something a scene can name, a cache can key, and a client can use to ask "do I have this,
and which version?". The identity must survive a repack, so that a scene referring to a texture does not itself need repacking when the texture
changes. Things packed out of one file (the meshes and materials of a USD file) need identities of their own too.

USD already names its assets by path, and the engine reads USD.

## Decision

- An asset id is the source path with forward slashes, relative to the asset server's root, plus `#<fragment>` for things packed out of one
  file: `assets/main_sponza/NewSponza_Main.usda#/Meshes/arch_01`.
- Ids that are absolute or climb above the root (`..`) are refused.
- The version of an asset is the FNV-1a 64-bit hash of its packed file. It is never part of the identity.
- The server's cache records, per id, the job key (rule, rule version, parameters, packer), the packer binary, the source, every file in the
  packer's depfile and the packed file, each with size, modification time and hash. A file whose size and time are unchanged is not re-hashed.

The root was first the project directory; [ADR 44](0044_no_project_root.md) made it the server's working directory.

## Consequences

- Easier: ids are readable in traces, records and USD files; a referring asset survives a content change of what it refers to.
- Easier: "is this up to date" is one check against the cache record, with a cheap stat fast path.
- Harder: renaming or moving a file breaks every reference to it.
- Harder: intent that isn't in the path cannot be expressed in the id yet — for example linear vs sRGB for the same image.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_pack_rules.md](../plan_pack_rules.md) (decision 3), [plan_asset_server.md](../plan_asset_server.md) (decisions 6 and 7),
  [src/asset/README.md](../../src/asset/README.md).
- Code: `src/asset/assethash.h:12` (FNV-1a 64 as the version), `src/asset/assetid.h:5` (`assetIdForPath`, refuses absolute and climbing paths),
  `src/asset/server/assetserver.cpp:60` (`validId`), `src/asset/server/assetcache.h:13` (what a cache record holds).
- Differs from plan: the plans give ids as project-relative; they are relative to the server's working directory.
- Rejected alternative: GUIDs — USD already chose paths.
