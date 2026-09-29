# Pack

Engine-ready data is packed ahead of time. Source assets (shaders today, later USD scenes, textures, meshes) are packed by one packer program per
asset type. A packed asset is its packer's output file, at a path derived from its asset id, and the engine reads it directly.

## Asset ids

An asset's id is its project-relative source path with forward slashes, as USD names assets: `shaders/gbuffer.vert`, `assets/textures/brick.png`.
The id never changes when the asset's content does, so nothing that refers to an asset needs repacking when that asset changes.

A packed asset is written to `<out_dir>/packs/<asset id>`, e.g. `_out/linux-vulkan/debug/packs/shaders/gbuffer.vert` holds SPIR-V.

## Rules, packers and pack targets

How assets are packed is declared in the root `build.cpp` with the build framework's `pack_rule` and `pack` (`src/build/framework/packrule.hpp`):

```cpp
auto shaderRule = pack_rule("shader")
                      .match({"shaders/*.vert", "shaders/*.frag", "shaders/*.comp", "shaders/*.geom"})
                      .packer(packerShader)
                      .param("optimize", per_config({{"debug", "0"}, {"release", "1"}, {"gamerelease", "1"}}))
                      .version(1);
auto corePack = pack("core").assets(glob({.include = "shaders/*.vert"}));
p.pack_rule(shaderRule);
p.target(corePack);
```

- **A rule** covers asset ids by glob pattern and names the packer program and its parameters. Parameters are fixed, or one value per configuration.
  An asset is packed by the first matching rule.
- **A pack target** lists assets to pack. Each asset becomes a pack job edge that runs its rule's packer; the target is a phony edge over the jobs,
  so other targets can depend on it (ngen-view depends on `core`).

## The packer contract

A packer is a standalone program (`src/apps/packer<type>.cpp`) run with the same arguments by every job:

```
<packer> --rule <name> --rule-version <n> --asset <id> --source <path> --out <file> --depfile <file> [--param key=value]...
```

It reads `--source`, writes the packed asset to `--out`, and writes a Make-format depfile listing every file it read to `--depfile`. It exits
nonzero, with a message on stderr, when it fails. `packer.h` parses the arguments.

## Caching and invalidation

A pack job is an ordinary build edge, so the build log decides when it reruns. It reruns when any of these change:
- the source's content hash
- any file the packer read, through the depfile
- the packer binary, which is an input
- the rule's name, version or parameters, all of which are in the command

**Reverse index.** `<out_dir>/packs/.ngen-packdeps` maps each file a pack job read to the asset ids that read it (`<file>\t<asset id>`). It's
rewritten after every build, for invalidating packed assets when a file changes.

## Packers

| Program | Packs | Output |
|---|---|---|
| `ngen-packer-shader` | GLSL with `glslc`: `optimize` (0 = `-O0`, 1 = `-O`) and `debug_info` (1 = `-g`); glslc's depfile gives the `#include`s | SPIR-V |

## The core pack

The `core` pack target lists everything ngen-view needs at start-up; today, every shader. The renderer's
`loadShaderModule(device, stage, "shaders/gbuffer.vert")` reads `<packs root>/shaders/gbuffer.vert` (`src/renderer/shaderloader.*`).

## Adding a packer

1. Write `src/apps/packer<type>.cpp` on `packer.h`: parse the arguments, read the source, write the packed asset to `--out` and the files read to
   `--depfile`.
2. Add a program target linking `packLib` and a `pack_rule` for its assets in `build.cpp`, and register both.
3. List the assets in a `pack(...)` target.
