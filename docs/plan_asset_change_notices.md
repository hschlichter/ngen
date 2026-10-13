# Asset change notices

**Status. Landed.** 2026-10-13.

The asset server tells its clients when an asset they hold has changed at its source, and the view acts on it: shaders reload in place, scene
assets come back through the view's rebuild. Edit a shader, a texture or a USD file while ngen-view runs, and the view shows the edit without a
restart or a Repack click.

## Current state

- **Nothing packs without a request, and nothing is watched** (`src/asset/README.md`, "The server"): an edit is picked up by the next request
  for the asset. The view requests each asset once, so it never sees an edit unless the user clicks Repack in the Assets window
  (`ViewScene::repack`, `view.repack`).
- **The server already knows every asset's inputs.** `AssetCache::Record` (`src/asset/server/assetcache.h`) holds, per job, the packer binary,
  every input (the source and every file in the packer's depfile) with size, mtime and hash, and the job's secondary outputs.
- **RPC carries notifications both ways** (`src/rpc/README.md`, "Messages"); the server already sends `asset.data`, `asset.ready` and
  `asset.failed` as notifications on the client's connection.
- **Shaders are fixed for the view's lifetime.** `PipelineRegistry` (`src/renderer/pipelineregistry.*`) creates a module once per shader id
  and keeps it; `src/view/README.md`: "a shader's repack refreshes only the server's cache: the view takes new shaders at its next start".
  Hot reload was deferred by [plan_async_shaders.md](plan_async_shaders.md).
- **Scene changes block.** `ViewScene::repack` and the LOD and purpose changes fetch with a blocking `AssetClient::wait` and rebuild the
  renderer's inputs, and `Renderer::uploadRenderWorld` re-uploads every texture (about 1.7 s on Sponza). The async scene that removes this
  is not planned yet (roadmap, "Everything async").

## Scope

**In**
- The server watches the inputs of every asset a connected client has received, and sends `asset.changed` to those clients when an input's
  content changes.
- `AssetClient` receives the notices: the changed ids are marked stale in its log and queued for the program to take.
- Shader hot reload in `PipelineRegistry`: a changed shader is requested again and every pipeline using it is recreated; a shader that fails
  keeps the old pipeline.
- The view's other assets (layer packs, meshes, materials, textures) after a change: requested again and rebuilt through today's path.
- The Assets window, the `assets` record and the server's `clients` record show what is stale and what each client is watched for.

**Out**
- Non-blocking scene updates and incremental GPU uploads: the interim rebuild stalls, as Repack does today (decision 4). The async scene plan
  takes the same notices later.
- A changed `pack.cpp` rule or a rebuilt server: the server reads its rules at start-up; restarting it is still the way.
- Watching sources nobody has received (a new file referenced by an edit is found by the next composition, through an ordinary request).
- Windows file watching: Linux only for now, behind an interface (decision 1).

## Decisions

Locked 2026-10-13, all four as proposed.

1. **The server notices changes through inotify, behind a `FileWatcher` interface, with polling as the fallback.** inotify gives changes at
   once and costs nothing while idle; it is Linux only and its watches are limited per user. The watcher watches **directories**, not files,
   so a save that replaces a file through a rename is seen, and a few hundred input directories cost a few hundred watches, not one per file.
   When a watch can't be added (`ENOSPC`), that directory is polled (mtime and size, once a second) instead, with a warning. Rejected:
   polling only (a stat per input per second, up to a second's delay; it stays as the fallback and as `--watch=poll` for testing).
2. **Notify only; the client requests again.** The server sends `asset.changed` and packs nothing; a client that still wants the asset
   requests it, and the request packs it because the cache is out of date. This keeps "nothing packs without a request"; the one new rule is
   "the server tells you when something you received has changed". Rejected: repacking at once and then notifying (one round trip less, but it
   packs what nobody may want, and two clients' requests would race with it).
3. **Every client that received an asset is told.** The server keeps, per connection, the ids it sent or answered as held; a connection is
   watched for them until it closes. Nothing to opt into; a client that doesn't care ignores the notice. Rejected: an explicit `asset.watch`
   (an API to get right for every client, for no client that wants fewer notices today).
4. **Scene assets take the blocking rebuild until the async scene lands.** A changed mesh, material, texture or layer pack goes through the
   same path as Repack (fetch, rebuild the inputs, full re-upload), so the view is correct after an edit, with a stall. Rejected: limiting
   notices to shaders until then (a stale view is worse than a stall, and the whole path gets exercised now).

## Steps

1. **`FileWatcher`** (`src/asset/server/filewatcher.h`, `filewatcherlinux.cpp`): watches directories and reports changed files in them.

   ```cpp
   // Reports files that changed in watched directories. Linux: inotify on each directory, polling (mtime and size, once a second) for a
   // directory inotify can't watch. Thread-safe; changes are reported on the watcher's own thread.
   class FileWatcher {
   public:
       using Changed = std::function<void(std::vector<std::filesystem::path> files)>;

       auto start(Changed changed, bool pollOnly) -> void;
       auto stop() -> void;

       // Watches the directory of each file; a directory is watched once however many files are in it.
       auto watch(std::span<const std::filesystem::path> files) -> void;
       // Stops reporting these files; a directory with no file left is unwatched.
       auto unwatch(std::span<const std::filesystem::path> files) -> void;
   };
   ```

   Only files asked for are reported. Changes are collected for 100 ms after the first one before they are reported, so an editor's
   truncate-write-rename or a packer writing many files is one report. inotify events used: `IN_CLOSE_WRITE`, `IN_MOVED_TO`, `IN_CREATE`,
   `IN_DELETE`.

2. **The server watches what it has sent** (`assetserver.*`, `assetcache.*`):
   - Per connection, `std::unordered_set<std::string> received`: every id answered to it as sent or held. `AssetCache::filesOf(id)` returns
     the record's packer and input paths; the server keeps a reverse map, file to ids, and asks the watcher to watch them. Closing the
     connection drops its ids (and unwatches files nobody holds).
   - On a report, each file's ids are checked with the cache's existing up-to-date test (size and mtime, then the hash), so a touch or a save
     of the same bytes sends nothing. For a changed file, every id of its record (secondary outputs too) held by a connection goes into one
     notification per connection: `asset.changed {"ids": [...]}`.
   - A packer's own writes (its output and depfile, under `.ngen-assets/`) are never inputs, so they don't trigger notices. (A source edited
     while its job packs can be recorded as packed with the new hash; see Deferred.)
   - Trace: `AssetChanged` per report (files, ids, clients told). Records: `clients` gains `received` (count) and `watched`; `status` gains
     `watchedDirectories` and `polledDirectories`. `--watch=poll` forces polling.
   - `src/asset/README.md`: the rule becomes "nothing packs without a request; the server tells a client when something it received has
     changed", with the notice in the protocol section.

3. **`AssetClient` takes notices** (`assetclient.*`):
   - `asset.changed` marks each id's log entry stale (`AssetLogEntry` gains `bool stale` and `uint32_t changes`) and queues it.
   - `auto takeChanged() -> std::vector<std::string>`: the ids changed since the last call, each once.
   - The arrived asset stays found (`find`) until a new version is ready: a re-request replaces it at its `asset.ready`, not at its first
     `asset.data`, so a reader never sees a missing or half-arrived asset. A failed re-request keeps the old asset and records the errors.
   - The Assets window shows stale rows (and their change count), and the `assets` record carries `stale` per asset.

4. **Shader hot reload** (`pipelineregistry.*`, `renderer.*`, `renderthread.*`, `src/apps/view.cpp`):
   - `PipelineRegistry::reload(std::span<const std::string> shaderIds) -> void`: for each id it has a module for, requests it again and marks it
     reloading. `update()` sees the new version once `find` returns it, creates the module, and recreates every ready pipeline using it,
     within the per-frame budget. The handle keeps its index; the old pipeline and module go to the deletion queue (`init` takes the
     renderer's `DeletionQueue*`), since frames in flight may use them.
   - A shader that fails (the packer's compile error, or module creation) keeps the old module and pipelines; the error goes to the trace
     (`ShaderReloadFailed`, a warning) and the `pipelines` record; the next good version is taken as usual.
   - Trace: `ShaderReloaded` (id, old and new version, pipelines recreated). The registry runs on the render thread: the view takes the changed
     ids each frame (`AssetClient::takeChanged`), passes the shader ids through `RenderThread` with the next snapshot, and the renderer calls
     `reload` before `update`.
   - `src/view/README.md` loses "the view takes new shaders at its next start". (Not as planned: a forced repack of an unchanged shader sends
     no notice, since its source didn't change, so Repack still refreshes only the server's cache for a shader. An edit is what reloads it.)

5. **Scene assets after a change** (`viewscene.*`, `src/apps/view.cpp`):
   - The changed ids that aren't shaders, kept to those `ViewScene` uses (`assetIds`), go to `ViewScene::refresh(ids)`: today's `repack`
     without `force` (the request packs them because the cache is out of date). `repack(ids)` becomes `refresh(ids, force = true)`.
   - Ids that change within a frame are refreshed together; a change during a refresh is taken the frame after.
   - Trace: the existing `AssetsRepacked` (renamed `AssetsRefreshed`, with `forced` true or false).

## Verification

- **A change is noticed, a touch isn't**: with the view on `assets/three_cubes.usda`, `touch shaders/gbuffer.frag` sends no `asset.changed` (no
  `AssetChanged` in the server's trace); appending a comment to it sends one, to the view only, and the view's `assets` record shows it stale
  and then received with a new version.
- **Shader reload**: a run with a script taking screenshots at two frames, with an edit to `shaders/debugview.frag` (a constant colour in the
  normals mode) in between, by the shell: the second screenshot shows the edit; reverting the edit makes the next screenshot byte-identical to
  the first. The trace has `ShaderReloaded` for each edit, the `pipelines` record the new versions, no validation messages.
- **A broken shader**: a syntax error in `shaders/gbuffer.frag` gives `ShaderReloadFailed` with the compiler's message; the frames go on
  byte-identical to before the edit; fixing it reloads.
- **Scene assets**: changing a cube's `displayColor` in `assets/three_cubes.usda` while the view runs gives `AssetsRefreshed` with the file's
  ids, and the next screenshot shows the new colour; reverting it gives a screenshot byte-identical to the first.
- **Several outputs, one file**: the change notifies every id of the file the view holds (layer pack and its mesh and material packs) in one
  `asset.changed`.
- **Atomic saves**: an edit written to a temporary file and renamed over the source (`mv`) is noticed like an in-place write.
- **Polling**: the shader reload and scene checks pass with the server on `--watch=poll`, within about a second of the edit.
- **Clients**: a second view on the same server is told too; after a view exits, the server's `clients` record no longer lists it and an
  edit to a file only it held sends nothing.
- **Nothing else changes**: without edits, three_cubes and Sponza screenshots are byte-identical to `_out/cutout-baseline`; debug and release
  build.

### Results (2026-10-13)

Every criterion above was checked headless on three_cubes against a release server, plus the regression run on Sponza.
- **Touch versus edit.** A touch, and an atomic rename back to the cached bytes, both trace "content unchanged, nothing to tell". An edit to
  `shaders/gbuffer.frag` sends one notice.
- **Shader reload.** The edit tints the frame red, and the 4 gbuffer pipelines are recreated (`ShaderReloaded`). The revert reloads again, and
  the next screenshot is byte-identical to the first and to `_out/cutout-baseline/three_cubes.png`. The `assets` record shows `changes: 2`,
  not stale; no warnings.
- **Broken shader.** `ShaderReloadFailed` traces glslc's message, the frames stay identical, and the `pipelines` record carries the error.
  Fixing it clears the error.
- **Scene asset.** A `displayColor` edit, saved through `mv`, turns the cube magenta. The notice carries the file's 5 ids (layer pack and 4
  meshes) and gives `AssetsRefreshed` with a 55–106 ms rebuild. The revert is byte-identical.
- **Polling and clients.** On `--watch=poll`, two views are both told and both reload within a second. After they exit, nothing is watched
  and an edit tells no one.
- **Regressions.** Sponza and three_cubes are byte-identical to `_out/cutout-baseline`. Debug and release build.

Added on the way:
- **A failed answer counts as news.** A client whose last answer for an id was a failure is told when the source goes back to what the
  cache holds. Without that, undoing a broken edit left the view showing a stale reload error.
- **`librariesReplaced` on `RenderUpload` and `uploadRenderWorld`.** A rebuild that keeps the same mesh and material handles was skipped by
  the renderer, so a colour edit (and before it, a Repack) never showed. New libraries now force a full upload.
- **Client changes.** `AssetClient::pending(id)` added. `find` returns a `shared_ptr`, since a new version can replace an asset while the
  render thread reads it. `wait` counts answers, so it waits for the new version, not the old one.

## Open questions

None left; the decisions are locked (2026-10-13).

## Deferred / follow-ups

- **An edit while its file's job packs.** The cache hashes the inputs when the job records them, after the packer ran. An edit in between
  is recorded as packed even though the output came from the older content, and the notice that went out leads to a request that finds the
  cache up to date. Fix: hash the inputs before running the packer, and record those hashes. Trigger: a stale asset seen after a quick
  double save.

- **The async scene and incremental GPU updates**: changes applied without a stall; it consumes the same notices. Trigger: this plan landing,
  as the next plan in the roadmap's "Everything async".
- **A changed `pack.cpp` rule or packer binary** while the server runs: re-reading rules on change, and notifying every asset of a changed
  rule. Trigger: rule edits becoming frequent.
- **Windows** (`ReadDirectoryChangesW`) and macOS (FSEvents) watchers. Trigger: a second platform.
- **Notices for the editor** (`ngen-editor` saving a layer and the view seeing it): falls out of this plan when the editor saves through the
  file system. Trigger: the editor split.
