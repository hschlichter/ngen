# ngen quick reference

A cheat sheet of the commands, flags, script verbs, RPC methods and records, for humans and agents. Every row says what it does in one line;
the library READMEs linked at the end explain how and why. The live sources of truth are `./ngen-cli introspect describe view` (every
method and its parameters) and the `config` record (every configuration value).

## Quick start

```sh
# Once per clone
mkdir -p _out && c++ -std=c++23 -O0 -g -pthread -o _out/ngen-build src/build/bootstrap.cpp
./_out/ngen-build -p linux-vulkan -c debug ngen-cli && ./_out/linux-vulkan/debug/ngen-cli set linux-vulkan debug

# Build and run
./ngen-cli build
./ngen-cli asset-server > /tmp/asset-server.log 2>&1 &     # leave it running; the view exits at start-up without it
./ngen-cli view assets/three_cubes.usda

# Headless verification run
./ngen-cli introspect trace --until-exit=view --output=/tmp/trace.jsonl & trace=$!
SDL_VIDEODRIVER=offscreen ./ngen-cli view assets/three_cubes.usda --frames=200 --script=/tmp/script.txt
wait $trace
jq -c 'select(.level != "info") | {process, level, type, text}' /tmp/trace.jsonl   # empty on a clean run
```

A script file holds `<frame> <verb> [args]` per line, for example `120 record render /tmp/records.jsonl`. Frames count from when the scene is
first resident.

Test scenes: `assets/three_cubes.usda` (smoke test), `assets/layered/root.usda` (composition), `assets/main_sponza/NewSponza_Main_USD_Zup_003.usda`
(materials, git-ignored), `assets/caldera/caldera.usda` (large, git-ignored).

## ngen-cli

The entry point for humans and agents. Runs the set variant's binaries.

| Command | Does |
|---|---|
| `ngen-cli set` | Print the set variant. |
| `ngen-cli set <platform> <config>` | Make a variant the set one (prefixes accepted). Agents don't change it unless asked: `_out/set` is shared. |
| `ngen-cli build [args]` | Run `ngen-build`; `-p`/`-c` default to the set variant, other arguments pass through. |
| `ngen-cli view [args]` | Run `ngen-view` (replaces the process, so signals and exit codes are the view's). |
| `ngen-cli asset-server [args]` | Run `ngen-asset-server` in the foreground. |
| `ngen-cli introspect [args]` | Run `ngen-introspect`: no arguments opens the window; `list`, `get`, `describe`, `call`, `trace` for the command line. |
| `ngen-cli pack <scene>...` | Run `ngen-pack`: pack everything a scene can reach, so the view opens it from the cache. |
| `ngen-cli help` | Usage, the set variant and which tools are built. |

After changing `src/apps/cli.cpp`: `./ngen-cli build ngen-cli`.

## ngen-build

`./_out/ngen-build -p <platform> -c <config> [options] [target...]`. Platforms: `linux-vulkan`. Configs: `debug`, `release`, `gamerelease`
(no RPC, no introspection).

| Option | Does |
|---|---|
| `-p`, `--platform <name>` | Platform (required for builds). |
| `-c`, `--config <name>` | Config (required for builds). |
| `--clean` | Remove the variant's build outputs and exit. |
| `--rebuild` | Run every step of the requested targets again. |
| `--compile-commands` | Write `compile_commands.json` for the variant and the merged top-level union. |
| `-v`, `--verbose` | One line per edge. |
| `-vv` | Echo each shell command. |
| `-l`, `--list` | Show platforms, configs and targets, and exit. |
| `--dump-graph` | Print the project IR as JSON and exit. |
| `-h`, `--help` | Usage. |

No target builds every target except the global tools. A target matches by exact name, then by substring of a source file stem, then by
substring of a library, program, tool or alias name.

| Target | Is |
|---|---|
| `ngen-view`, `ngen-asset-server`, `ngen-introspect`, `ngen-cli` | The main programs. |
| `ngen-pack`, `ngen-packdump`, `ngen-worldcheck`, `ngen-usdcheck` | Asset and scene check tools. |
| `ngen-packer-usd`, `-texture`, `-exr`, `-shader`, `-copy` | Packers, run by the asset server. |
| `examples` | Every RHI example (`ngen-example-triangle` … `ngen-example-mipchain`). |
| `format` | clang-format the tree (only when named). |
| `tidy` | clang-tidy (only when named). |

## ngen-view

`ngen-view [flags] <scene>`. The scene is an asset id: a path relative to the working directory, which must be the asset server's.

| Flag | Does |
|---|---|
| `--frames=N` | Quit after N frames (counted from the scene first being resident). |
| `--script=FILE` | Run a script: `<frame> <verb> [args]` per line. |
| `--screenshot=PATH` | Write the presented frame as PNG on the last frame (frame 3 without `--frames`). The UI is left out. |
| `--show-ui` | Keep the UI (menu bar, windows, status bar) in screenshots. |
| `--camera=x,y,z,yaw,pitch` | Start camera pose, set before the scene loads. |
| `--camera=/camera/prim` | Start at a camera the scene authors. |
| `--camera-frame=scene\|/prim` | What the camera frames while the scene arrives. |
| `--select=/prim` | Select a prim (full path, instances expanded). |
| `--view=<mode>` | Buffer view: `lit`, `albedo`, `normals`, `depth`, `shadowfactor`, `shadowmap`, `shadowuv`, `worldpos`, `miplevel`, `cascades`, `lodlevel`, `hzb`. |
| `--overlay=name=on\|off,...` | Overlays: `grid`, `origin`, `gizmo`, `aabbs`, `lightgizmos`, `buffer`, `shadow`, `aa`, `cascadefrusta`. |
| `--no-wait-resident` | Count script frames from start-up instead of from residency, to look at loading. |
| `--validation` | Turn on the Vulkan validation layer. |
| `--fail-on-validation` | Validation on; exit code 2 if it reported anything. |
| `--geometry-limit=<MiB>` | Cap each geometry pool buffer, to exercise a full pool. |
| `--render-debug` | Keep the render debug snapshot and draw log on without the window. |
| `--renderdoc` | Load RenderDoc before the device so `renderdoc-capture` works. |
| `--no-rpc` | No RPC endpoint and no discovery file. |

Headless: set `SDL_VIDEODRIVER=offscreen`.

### Keys

| Key | Does |
|---|---|
| W A S D, E / Q, Shift | Move, up and down, fast. |
| Right mouse drag | Look. |
| Mouse wheel | Move speed; while orbiting, dolly toward the pivot. |
| Left click | Pick: select the prim under the cursor. |
| Left Alt + right drag | Orbit around the point under the cursor. |
| Z / R / F | Deselect / select the parent / frame the selection. |
| G | Camera follow (keep the eye a height above the ground) on or off. |
| C | With the debug camera on: hand the controls between debug and main camera. |
| Ctrl+D | Debug camera on or off. |
| Ctrl+P / C / S / L / A | Performance / Camera / Streaming / LOD / Assets window. |
| 1 to 5 | Debug view: wireframe, triangle size, overdraw, instance, streaming. Press again for off. |
| Ctrl+1 / Ctrl+2 | Buffer overlay / shadow overlay. |
| Ctrl+3 / Ctrl+4 | Mip level view / LOD level view. |
| Ctrl+5 | Every view and overlay off for the plain lit image; again restores them. |
| F12 | Screenshot. |

## Script verbs

A script line is `<frame> <verb> [args]`. Every verb runs an RPC method with the same code, so the method column also tells you what to call
live.

### Scene and selection

| Verb | Method | Does |
|---|---|---|
| `select /prim` | `view.select` | Select a prim; an empty path deselects. |
| `pick <x> <y>` | `view.pick` | Select what is drawn at a framebuffer pixel (GPU readback; the `Picked` event names it). |
| `translate /prim dx,dy,dz` | `view.translate` | Offset a prim's local position: the view's own opinion, never saved. |
| `purposes guide=on\|off,proxy=on\|off` | `view.purposes` | Draw guide and proxy prims. Holds the script until drawn. |
| `repack <id>...\|scene` | `view.repack` | Force the asset server to repack, and rebuild the scene. Holds the script until resident. |
| `goto-asset <id> [all]` | `view.gotoAsset` | Move the camera to an asset's next draw (nearest first) and select it; `all` frames every draw. |
| `assets-of [/prim]` | `view.assetsOf` | The assets drawn at or under a prim, or the selection. |
| `wait-resident` | `view.waitResident` | Hold the script until the scene is resident. |
| `quit` | `view.quit` | Stop the view after this frame. |

### Camera

| Verb | Method | Does |
|---|---|---|
| `camera x,y,z,yaw,pitch [debug]` | `view.camera.set` | Place the main camera, or the debug camera (turn it on first: `debug-camera on`). |
| `camera-use /camera/prim` | `view.camera.use` | Move to a camera the scene authors. |
| `camera-frame [scene\|/prim]` | `view.camera.frame` | Frame the scene or a prim. |
| `camera-orbit x,y,z yaw pitch [debug]` | `view.camera.orbit` | Turn around a pivot by angles, the distance kept. |
| `camera-follow on\|off`, `camera-follow height <m>` | `config.set` | Keep the eye a height above the highest drawn surface under it. |
| `fly <path prefix> <m/s>` | `view.camera.fly` | Fly through the scene's cameras with that prefix, in path order. Holds the script while flying. |
| `fly-stop` | `view.camera.fly.stop` | Stop the flight. |
| `debug-camera on\|off\|place` | `view.camera.debug` | Debug camera on, off, or placed 15 m behind and 10 m above the main camera. |
| `debug-camera control main\|debug` | `view.camera.debug` | Which camera the controls move. |
| `debug-camera lod\|cull\|shadows main\|debug` | `view.camera.debug` | Which camera drives LOD, culling or shadow cascades. |
| `debug-camera trail on\|off\|clear` | `view.camera.debug` | The main camera's trail. |

### LOD and streaming

| Verb | Method | Does |
|---|---|---|
| `lod <root> <level>` | `view.lod.set` | The level a LOD root wants; loads it. Holds the script until drawn. |
| `lod-reset <root>` | `view.lod.reset` | Back to the default level. |
| `lod-heuristic on\|off` | `view.lod.heuristic` | The LOD heuristic (on by default). |
| `lod-prefer distance\|screen\|authored` | `view.lod.heuristic` | Which heuristic a root with several measures by. |
| `lod-thresholds <root> [h=<i>] <t>...` | `view.lod.override` | Override one heuristic's thresholds for every placement of that authored root. Without `h=`, the heuristic in use; no values clears. Distances ascend, screen sizes descend. |
| `lod-blend-thresholds <root> [h=<i>] <t>...` | `view.lod.override` | Override blend thresholds, same form. |
| `lod-unload-thresholds <root> [h=<i>] <t>...` | `view.lod.override` | Override a streaming heuristic's unload radii, one per load threshold. Without `h=`, the streaming heuristic. |
| `lod-override-reset [<root>]` | `view.lod.override.reset` | Drop overrides for one root, or all. |
| `lod-bias <f>` | `view.lod.tuning` | Pick every root as if this many times closer. |
| `lod-hysteresis <fraction>` | `view.lod.tuning` | Hysteresis at a boundary without a blend band. |
| `lod-blending on\|off` | `view.lod.tuning` | LOD blending. |
| `lod-show labels\|gizmo\|gizmo-all\|flash on\|off` | `config.set` | LOD tool viewport layers. |
| `lod-show color level\|depth` | `config.set` | What the `lodlevel` view colours by. |
| `streaming-show bounds\|rings\|rings-all\|labels on\|off` | `config.set` | Streaming tool viewport layers. |
| `streaming-reset` | `view.streaming.reset` | Zero the `streaming` record's measured totals. |

### Rendering settings

These set configuration values. `config <name> <value>` sets any value; `config-reset [<name or prefix>]` puts values back to their defaults.

| Verb | Does |
|---|---|
| `config <name> <value>` | Set any configuration value (`config render.shadow.cascades 1`). |
| `config-reset [<name or prefix>]` | Reset one value, a group, or all. |
| `view <mode>` | Buffer view, same modes as `--view`. |
| `debugview <mode>` | `off`, `wireframe`, `trianglesize`, `overdraw`, `instance`, `mesh`, `material`, `primitive`, `uv`, `streaming`, `meshlet`, `meshletcull`. |
| `overlay name=on\|off,...` | Overlays, same names as `--overlay`. |
| `cull on\|off\|freeze\|unfreeze\|show\|hide` | Frustum culling, frozen frustum, AABB overlay. |
| `cull view N` | Colour the AABB overlay by view N's visibility (0 camera, 1+ cascades). |
| `occlusion on\|off` | Two-phase occlusion culling (on by default). |
| `hzb-level <n>` | The depth pyramid level the `hzb` view shows. |
| `prepass on\|off` | Depth prepass. |
| `aa on\|off` | Anti-aliasing; off to compare images pixel for pixel. |
| `ambient sky=r,g,b ground=r,g,b\|reset` | Hemisphere ambient. |
| `sampler aniso=8,bias=0,minlod=0,mip=linear\|nearest` | Material sampler. |
| `shadow cascades=3,tile=1024,lambda=0.5,pcf=on\|off,distance=200` | Cascaded shadows. |
| `gpuscene budget=<MiB>,limit=<MiB>` | Upload budget, geometry pool limit. |

### Output, capture and UI

| Verb | Method | Does |
|---|---|---|
| `record <record> <file>` | `introspect.record` | Append a record to a file as one JSON line: `{record, requested_frame, frame, ts_ns, value}`. |
| `screenshot <path>` | `view.screenshot` | Write the presented image as PNG. |
| `inspect <texture> <level>\|off` | `view.inspect` | Texture inspector capture. `<texture>` is a texture handle, the `render` record's `textures[].texture`: not a list position, 0 is "no texture", ids are never reused. |
| `dump-texture <texture> <level> <path>` | `view.dumpTexture` | One mip level of a material texture as PNG; same texture id. |
| `capture <pass\|-\|+> <resource> <path> [x y w h]` | `capture.request` | A frame-graph resource after a pass (`-` end of frame, `+` after its last writer). Buffers as JSON, textures as PNG + JSON. |
| `dump-frame <dir>` | `introspect.frame` | Every pass, barrier, command and descriptor, plus captures of what each pass writes. |
| `dump-gpuscene <dir>` | `introspect.gpuscene` | GPU scene tables, culling buffers and `instances_joined.json`. |
| `dump-profile <path>` | `introspect.profile` | Profiler history as a Chrome trace (Perfetto). |
| `renderdoc-capture` | `renderdoc.capture` | RenderDoc capture of the next frame (needs `--renderdoc`). |
| `window <name> on\|off` | `view.window` | Open or close a window: `memory`, `capture`, `framedebugger`, `gpuscene`, `counters`, `shaders`, `culling`, `assets`, `lod`, `streaming`, `camera`, `renderdebug`, `framegraph`, `performance`, `config`. |

## RPC methods without a verb

Every method above is also callable live: `./ngen-cli introspect call view <method> '<json params>'`. These ngen-view methods have no verb.
The asset server serves only its own methods (next section), and no `rpc.*`.

| Method | Does |
|---|---|
| `rpc.describe` | Every method with its parameter schema. |
| `rpc.ping` | Liveness and latency. |
| `rpc.version` | Protocol version and endpoint kind. |
| `introspect.list` | The records this endpoint gives. |
| `introspect.get` | One record as JSON. |
| `config.get` | One value by name, or every one under a prefix. |
| `config.set` | One value, or several as `{values: {name: value}}`; nothing is set when one fails. |
| `config.reset` | Reset a value, a prefix, or all. |
| `view.camera.get` | A camera's position and yaw/pitch. |
| `view.camera.list` | The cameras the scene authors. |
| `view.transform` | A prim's local and world transform. |
| `view.lod.inspect` | One LOD root as the LOD window's inspector shows it. |
| `view.streaming.inspect` | One streaming root as the Streaming window shows it. |
| `view.renderlod` | Render-LOD groups under a path, with what the GPU wants and draws (answers frames later). |
| `trace.subscribe`, `trace.unsubscribe` | Trace stream (answered by the endpoint itself; not in `rpc.describe`). |

Errors: `-32601` unknown method, `-32602` invalid params (names the field), `-32603` internal, `-32000` engine failure with a message.

## ngen-asset-server

Run from the directory that holds the assets; the cache is `.ngen-assets/` there. Stop it with Ctrl-C. Restart it after changing `pack.cpp`
or `src/asset/server/`.

| Flag | Does |
|---|---|
| `--watch=inotify` | Watch sources with inotify (default). |
| `--watch=poll` | Poll the sources of what clients received once a second. |
| `--delay=<id pattern>:<ms>` | Test aid: hold answers for matching ids (`*` matches anything) that long after packing. Repeatable. |

| Method | Does |
|---|---|
| `asset.request {ids, have, force}` | Request assets; answers `{request}`, then streams `asset.data` chunks and `asset.ready` or `asset.failed`. |
| `asset.delay.set {pattern, ms}` | Change a test delay at runtime; 0 ms removes it. |
| `asset.changed` (notification) | Sent by the server when the sources of held assets change. |
| `introspect.list`, `introspect.get` | Its records. |

Records: `status` (pid, variant, clients, pack tasks, packer runs), `rules` (pack rules from `pack.cpp`), `cache` (every packed asset),
`requests` (in flight and recent), `clients` (bytes queued, assets waiting).

## ngen-introspect

Finds processes through `.ngen-discovery/` in the working directory. A target is `<kind>`, `<kind>:<pid>` or `<pid>` (`view`, `asset`,
`view:12345`). Exit codes: 0 ok, 1 the call returned an error, 2 no endpoint.

| Command | Does |
|---|---|
| `ngen-introspect` | The window. |
| `list` | Live endpoints and their records. |
| `get <target> <record>` | One record as JSON. |
| `describe <target>` | Every method and its parameters. |
| `call <target> <method> [params-json]` | Call a method. |
| `trace [options]` | Record the trace of every process, merged by time. |

| Trace option | Does |
|---|---|
| `--output=FILE` | Write the trace to a file (sorted, when done). |
| `--until-exit=TARGET` | Stop once that process has exited. Without it, run until Ctrl-C. |
| `--process=A,...` | Only these processes. |
| `--category=A,...` | Only these categories. |
| `--type=A,...` | Only these event types. |
| `--level=warning\|error` | Only this level and above. |
| `--history` | Include what the processes' rings still hold. |

| Window option | Does |
|---|---|
| `--select=<target>/<record>` | Show a record once its process is there. |
| `--trace` | Open the Trace tab. |
| `--frames=N` | Quit after N frames. |
| `--screenshot=PATH` | Write the last frame as PNG. |

## ngen-view records

Read live with `./ngen-cli introspect get view <record>`, or in a script with `record <record> <file>`. "Next frames" records answer a few
frames later; record them at steady state (frame 120 or later).

| Record | Holds |
|---|---|
| `status` | Frame counter, scene, selection, main camera, camera modes. |
| `config` | Every configuration value: type, value, default, description, range or choices. |
| `scene` | Layer stack, up axis and units, prims, prototypes, draws, lights, LOD levels set. |
| `environment` | The dome light, its image and the light it casts. |
| `scenethread` | Requests, composes, placement passes, assets in flight and held, residency. |
| `gpuscene` | Change lists, upload queue and budget, geometry pool. |
| `instancers` | Every PointInstancer: points, those shown, prototypes. |
| `variants` | Every prim with variant sets: the variant composed, counts per variant. |
| `payloads` | Every payload placement: prim, asset ids, loaded, extentsHint. |
| `assets` | What the view asked the asset server for: per asset state, size, times. |
| `culling` | Per view: instances drawn and culled, meshlets culled by frustum, cone and depth, triangles. |
| `pipelines` | Every pipeline: state, shaders and versions, last error. |
| `render` | Device, swapchain, scene tables, per-pass stats, draw log (next frames). |
| `memory` | Every allocation and memory heap (next frames). |
| `counters` | GPU zones and pipeline statistics per pass (next frames). |
| `profile` | CPU and GPU time per frame. |
| `zones` | Every profiler zone over the last 60 frames: count, total, median, longest. |
| `lod` | Every LOD root: levels, default, wanted, drawn, loaded, set by hand. |
| `streaming` | Per streaming root: selected level and why, radii, state, resident, late; measured totals. |
| `renderlod` | Render-LOD groups, the GPU's pick and its check against the heuristic (next frames). |
| `lodtool` | The LOD tool's inspection of the root at or above the selection, tuning, layers, pops. |
| `streamingtool` | The Streaming tool's inspection of the root at or above the selection, totals, map view. |
| `debugcamera` | Debug camera position, yaw, pitch and the main camera's trail. |
| `windows` | The UI's windows and whether each is open. |

```sh
jq 'select(.record == "render") | .value.passes' /tmp/records.jsonl
```

## Configuration values

Set with `config <name> <value>` or `config.set`; list with the `config` record or `config.get`. A prefix selects a group (`render.shadow`).

| Group | Values |
|---|---|
| `render.view` | `debug`, `buffer`, `hzblevel` |
| `render.overlay` | `buffer`, `shadow` |
| `render.shadow` | `cascades`, `tile`, `distance`, `splitLambda`, `pcf` |
| `render.sampler` | `anisotropy`, `lodBias`, `minLod`, `nearestMip` |
| `render.culling` | `enabled`, `frozen`, `occlusion` |
| `render.ambient` | `sky`, `ground` |
| `render` (other) | `antialiasing`, `prepass`, `sky.on`, `upload.budgetMiB`, `geometry.limitMiB` |
| `scene.purposes` | `guide`, `proxy` |
| `view.camera` | `follow`, `followHeight` |
| `tools.debugcamera` | `on`, `controlMain`, `lod`, `cull`, `shadows`, `trail` |
| `tools.overlay` | `grid`, `origin`, `gizmos`, `aabbs`, `selectedAabb`, `culled`, `culledView`, `lightGizmos`, `cascadeFrusta` |
| `tools.lod` | `labels`, `gizmo`, `gizmoAll`, `popFlash`, `colorByDepth` |
| `tools.streaming` | `bounds`, `rings`, `ringsAll`, `labels` |

## Other programs

Run from the asset server's directory; most fetch from the set variant's server. Exit code 3 means usage.

| Program | Usage | Does |
|---|---|---|
| `ngen-pack` | `ngen-pack <scene>...` | Pack every asset a scene can reach (every LOD level and variant), so the cache is full. |
| `ngen-packdump` | `ngen-packdump [--vertices] <file>...` or `[--vertices] --id <id>...` | Print a scene pack as JSON, from a file or from the server. |
| `ngen-worldcheck` | `[--lod <root>=<level>]... [--transforms] [--instancing] [--instancers] [--lod-heuristics] [--lod-authoring] [--output <file>] <scene id>` | Compose the World from packs and print its fingerprint. |
| `ngen-usdcheck` | `[--lod <root>=<level>]... [--transforms] [--instancing] [--instancers] [--lod-heuristics] [--lod-picks <file>] [--output <file>] <scene file>` | Open the scene with OpenUSD and print its fingerprint, to compare with `ngen-worldcheck`. |
| `ngen-packer-*` | Run by the asset server | `usd` (layer packs), `texture` (PNG/JPG to mipped RGBA8), `exr` (sky HDR + irradiance), `shader` (GLSL to SPIR-V), `copy` (bytes unchanged). |
| `ngen-example-*` | `--frames=N --check --validation [--size=WxH] [--screenshot=PATH] [--resize-at=N]` | RHI examples; nonzero exit on a pixel mismatch or validation error. |

```sh
./_out/ngen-build -p linux-vulkan -c debug examples
for t in triangle quad texture uniforms depth rendertarget pushconstants blend lines mipcube compute timestamps gpuzones bindless indirect meshshader mipchain; do
  SDL_VIDEODRIVER=offscreen ./_out/linux-vulkan/debug/ngen-example-$t --frames=10 --check --validation >/dev/null 2>&1; echo "$t=$?"
done
```

## Where to read more

[`src/rpc/README.md`](src/rpc/README.md) (protocol, adding a method), [`src/introspect/README.md`](src/introspect/README.md) (records, the
tool), [`src/trace/README.md`](src/trace/README.md) (events), [`src/view/README.md`](src/view/README.md) (commands, camera, frame),
[`src/config/README.md`](src/config/README.md) (configuration), [`src/asset/README.md`](src/asset/README.md) (asset server, packers),
[`src/build/README.md`](src/build/README.md) (build system), [`src/rhi/README.md`](src/rhi/README.md) (RHI examples), and the `run-headless`
skill (`.claude/skills/run-headless/SKILL.md`).
