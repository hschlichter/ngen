# Assets

The asset system gets engine-ready data to the engine. Something requests an asset by id from `ngen-asset-server`; the server packs it if its cached
packed form is out of date, and streams the packed bytes over RPC to whoever asked. No client reads the server's files.

**Packing** is the process inside it: one packer program per asset type turns a source file (shaders today, later USD scenes, textures, meshes) into
its engine-ready form, under the rules in the root `pack.cpp`.

| | |
|---|---|
| `assetclient.*` | `AssetClient`, the engine side of the stream |
| `assethash.h` | the content hash behind asset versions |
| `server/` | `ngen-asset-server`: requests, the cache (`assetcache.*`), the stream, and running packers (`packjobs.*`) |
| `pack/` | packing: `packrule.h` (the rule type `pack.cpp` fills in), `packer.h` (what every packer program shares), the packed texture format (`packedtexture.*`) and the mip filter (`mipchain.*`), which the renderer uses too |

The asset system is separate from the build system. `ngen-build` builds the asset server and the packers as ordinary programs, and knows nothing
else about assets.

## Asset ids

An asset's id is its source path relative to the asset server's working directory, with forward slashes, as USD names assets: `shaders/gbuffer.vert`,
`assets/textures/brick.png`.
The id never changes when the asset's content does. The asset's **version** is the content hash of its packed file (FNV-1a 64, `assethash.h`), sent as
16 hex digits.

## Rules: `pack.cpp`

The root `pack.cpp` holds the project's pack rules, as `build.cpp` holds the build. It is compiled into `ngen-asset-server`, so a rule change takes
effect when the server is rebuilt and restarted. A rule (`pack/packrule.h`) names the file extensions it packs, the packer program, the parameters it
passes, and a version to bump when the output format changes:

```cpp
auto packRules(const std::string& config) -> std::vector<PackRule> {
    std::vector<PackRule> rules;
    rules.push_back(PackRule{
        .name = "shader",
        .extensions = {".vert", ".frag", ".comp", ".geom"},
        .packer = "ngen-packer-shader",
        .params = {
            {"optimize", config == "debug" ? "0" : "1"},
            {"debug_info", config == "gamerelease" ? "0" : "1"},
        },
        .version = 1,
    });
    return rules;
}
```

No asset is listed anywhere: any file with a rule's extension can be requested. An extension belongs to one rule; two rules claiming the same one
stop the server at start-up.

## The asset server

`ngen-asset-server` (`src/apps/assetserver.cpp`, `server/`) is one binary per variant, `_out/<platform>/<config>/ngen-asset-server`. It is started
by hand and runs until Ctrl-C or SIGTERM:

```sh
./ngen-cli asset-server &                          # the set variant's
./_out/linux-vulkan/debug/ngen-asset-server &      # any variant's
```

**Its working directory is the root.** Asset ids are relative to it, packers run in it, the cache is `.ngen-assets/` in it, and it registers in
`.ngen-discovery/` in it, as kind `asset` with its variant as the label; a view running in the same directory finds it. Nothing depends on where
the binaries are, except that the packers sit next to the server. It takes its variant (and so the rules' configuration) from the folder its binary
is in, `_out/<platform>/<config>/`. It uses nothing from `src/build/`: its own cache, its own job runner, and the RPC core.

- **`asset.request`** `{ids: [...], have: {id: version}}` answers at once with `{request}`. Then each asset is answered on its own as it finishes:
  - `asset.data` `{request, id, version, offset, total}` notifications, each with up to 1 MiB of the packed file as the frame's attachment, in order
  - then `asset.ready` `{request, id, version, size, sent, packed}`; `packed` is true when a packer ran for it, false when the cache was up to
    date. An asset whose held version (`have`) is current gets `asset.ready` with `sent: false`
    and no data.
  - or `asset.failed` `{request, id, errors}`: no rule for the extension, no such file, an id outside the project, or the packer's output when it
    failed
- **Bulk and shared.** A request's assets are packed in parallel on one worker per hardware thread. An id already being packed, for any client, is
  not packed twice; the result goes to every request that asked.
- **Streaming and flow control.** Chunks of different assets interleave, so a large asset doesn't hold back small ones requested with it. At most
  8 MiB of pack data is queued on a connection; the next chunk goes out as the queue drains, so a slow client is never dropped for a full queue.
- **Nothing packs without a request.** The server doesn't watch files and packs nothing at start-up. An edit is picked up by the next request for
  the asset.
- **The trace.** The server prints one timestamped line per event on stdout (`server/assettrace.h`): its rules and cache at start-up, clients
  connecting and leaving, each request with its asset count, per asset whether it was up to date, packed (time, size, inputs, version) or failed
  (with the packer's output), and per request a summary when it is done (time, assets sent and bytes, held, failed). Redirect it to a file to keep
  it: `./ngen-cli asset-server > /tmp/asset-server.log &`.
- **`server.status`**: pid, variant, clients with their queued and peak queued bytes, tasks running and queued, and `packerRuns`, the number of
  packer processes run so far.

## The cache

`.ngen-assets/<platform>/<config>/` in the server's working directory is its cache: packed files at `<asset id>`, and `.ngen-assetcache`, which records
per asset what it was packed
from (`server/assetcache.*`):
- the job key: the rule's name, version, parameters and packer name
- the packer binary
- the source and every file in the packer's depfile
- the packed file

Each file is recorded with its size, modification time and content hash. An asset is up to date when its job key matches and every recorded file
still has the same contents; unchanged size and modification time skip re-hashing. So a request repacks an asset when its source, a file it
includes, the packer binary or its rule changed, and otherwise streams the cached file.

## The packer contract

A packer is a standalone program (`src/apps/packer<type>.cpp`) run with the same arguments by every job, with no shell:

```
<packer> --rule <name> --rule-version <n> --asset <id> --source <id> --out <cache>/<id> --depfile <cache>/<id>.d [--param key=value]...
```

It reads `--source`, writes the packed asset to `--out`, and writes a Make-format depfile listing every file it read to `--depfile`. It exits
nonzero, with a message on stdout or stderr, when it fails. `pack/packer.h` parses the arguments.

| Program | Packs | Output |
|---|---|---|
| `ngen-packer-shader` | GLSL with `glslc`: `optimize` (0 = `-O0`, 1 = `-O`) and `debug_info` (1 = `-g`); glslc's depfile gives the `#include`s | SPIR-V |
| `ngen-packer-texture` | `.png .jpg .jpeg`: decoded with stb_image, the full mip chain built with `pack/mipchain.h` (2×2 box filter, colour averaged in linear space), written as a packed texture (`pack/packedtexture.h`): a 24-byte `NGTX` header (version, format, width, height, mip levels) and every level, level 0 first, tightly packed | RGBA8 sRGB, all mips |
| `ngen-packer-copy` | `.usda .usdc .usd .hdr`: the source's bytes, unchanged, for assets the engine still reads in their source format. A clone (`FICLONE`) where the filesystem has one (btrfs, XFS), so the cache shares the source's blocks; a byte copy elsewhere | the source |

## The client

`AssetClient` (`assetclient.h`, the `assetclient` library) is the engine side. `connect(variant)` finds the asset server of that variant and project
through discovery, and fails when none is running. `request(ids)` sends one `asset.request`; `wait(ids)` blocks until each has arrived or failed;
`find(id)` returns the bytes and version, `take(id)` moves them out of the client (a later request streams them again), `stats()` counts what
the client requested, received (packed or cached), failed and has in flight, and the bytes received (ngen-view's status bar shows them), and
`errors(id)` gives
the reasons it failed. A new request for an id clears an earlier failure.

ngen-view connects at start-up and exits with an error naming the server command when there is none. It requests every shader on
`startupShaderIds()` in one request, loads the scene meanwhile, and waits for them just before the renderer needs them (`src/renderer/shaderloader.*`).
The scene's USD layers and textures come through the same client, by USD's asset resolver (`src/scene/README.md`).

`assetid.h` turns a relative path into an asset id (normalised; empty for an absolute path or one that climbs above the server's directory). The
view uses it for its scene argument, which is an id, and the USD resolver for every asset path.

## Adding a packer

1. Write `src/apps/packer<type>.cpp` on `pack/packer.h`: parse the arguments, read the source, write the packed asset to `--out` and the files read to
   `--depfile`.
2. Add a program target linking the `packer` library in `build.cpp`, and make `ngen-asset-server` depend on it.
3. Add a rule for its extensions in `pack.cpp`.
