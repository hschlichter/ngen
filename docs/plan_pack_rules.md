# Pack rules, packers and the pack database

**Status. Draft.**

Part of step 2 of [plan_tool_architecture.md](plan_tool_architecture.md). This plan builds the machinery that turns a source asset into packed data:
- rules per asset type, declared in `build.cpp`
- one packer program per type
- the pack container
- stable asset ids
- dependency tracking for caching and invalidation
- static pack targets

It proves the machinery on shaders and a core pack. The USD scene packer and the layer-pack contents are the next step 2 plan. Async requests through
`ngen-build --serve` come after that.

## Current state

- **Shaders are a build tool.** `build.cpp` has a `shaders` tool that runs `glslc` per file into `_out/<platform>/<config>/shaders/*.spv`, with flags
  per config. Renderer passes load them by path through `loadShaderModule` (`src/renderer/shaderloader.cpp`, 18 call sites).
- **Everything else loads from source at runtime.** USD through `USDScene`, and textures through stb in `MaterialLibrary`'s load path.
- **The runner's build log** (`build/run/buildlog.hpp`) already records per edge:
  - a command hash
  - input and output content hashes, with a stat fast path
  - dependencies discovered through a depfile

  An edge is dirty when any of them change. What the build system lacks for packing:
  - jobs created at runtime, not at graph time
  - packers that request further assets
  - stable asset ids
  - a reverse index from each dependency to its dependents

## Scope

**In**

- **`pack_rule` in the build framework** (`build/framework/packrule.hpp`): name, file patterns, packer target, parameters per variant, version.
  Registered with `Project`, emitted into the IR.
- **The packer contract:** the command line in, and packs plus a manifest out. The manifest lists outputs with content hashes, the files read (the
  dependency record) and the assets the packer requests from other rules.
- **Stable asset ids:** the project-relative source path, plus a fragment for things packed out of one file.
- **The pack container:** memory-mappable, a table of chunks keyed by asset id, each with type, version (content hash) and bytes.
- **Pack jobs as edges.** A static pack target expands to one edge per asset, and requests found in a manifest expand the job set before the run
  completes. They reuse the runner's dirty detection and build log; pack jobs are logged in `_out/<platform>/<config>/packs/.ngen-packlog`.
- **The reverse index:** from each dependency path to the asset ids that read it, kept next to the pack log.
- **The shader packer** (`ngen-packer-shader`), and a **core pack** static target that ngen-view depends on. The renderer loads shaders by asset id
  from the core pack instead of by `.spv` path.

**Out**

- **The USD packer, layer packs, sub-packs, variants, components**: the next step 2 plan (the pack format for scenes).
- **The texture packer.** It follows the same contract once the scene format says what a material references. It's listed here only as the second rule
  in the examples.
- **Async requests** (`pack.request`, `pack.ready` per asset), the server holding the graph resident, and file watching: the build server plan.
  This plan runs packing one-shot, through `ngen-build`.
- **Async shader loading and hot reload**: `plan_async_shaders.md`, with the renderer-level pipeline registry, after the server exists (see
  [plan_tool_architecture.md](plan_tool_architecture.md), step 2).
- **Shader reflection** (deferred from [plan_introspection.md](plan_introspection.md)). The shader packer's manifest is where
  reflection data will go.

## Decisions

Locked with Henrik.

1. **Rules live in `build.cpp`, on a generic `pack_rule` type in `build/`.** `build/` stays free of project knowledge and `build.cpp` states the
   project's rules. A separate rules file loaded by the server was the alternative; it would split the project description in two.
2. **One packer program per type**, built as ordinary targets and run as job commands. They are not plugins inside the server: that would break
   `build/`'s rule of no project code and pull pxr into `ngen-build`. It also keeps a packer crash from taking the server down. If process start-up
   costs show up, long-running packer workers are the follow-up.
3. **Asset ids are paths, as USD does it.**
   - The id is the project-relative source path with forward slashes, for example `shaders/gbuffer.vert` or `assets/textures/brick.png`.
   - Things packed out of one file get the path plus a `#` fragment, for example `assets/sponza.usda#/root/Mesh_01`, following USD's asset path
     plus `SdfPath`.
   - The content hash is the id's version, never its identity. A changed texture keeps its id, and nothing that references it needs repacking.
   - GUIDs were the alternative; USD already chose paths.
4. **Everything is async** (from the umbrella). A scene loads fast, and sub-assets arrive as they finish. This plan's part is the manifest's request
   list, which is what lets a later server report each asset separately. The one-shot path here runs all requests before returning.
5. **Core pack plus async for everything else.**
   - **The core pack** holds what ngen-view needs to render anything: the shaders of the always-present passes, the fallback texture, and default
     materials. It's built statically as a dependency of the ngen-view target, so the view starts, and headless runs work, without a server.
   - **Every other asset, other shaders included,** is requested at runtime. In this plan every current shader goes into the core pack, so the
     output can be compared byte for byte with today's `.spv` files. The async shader plan moves the optional ones out, for example `debugview.*`.

6. **Chunk types are strings**, written by the packer (`"spirv"`, `"texture"`, …). A new packer needs no container change. A closed enum in
   `src/pack/` was the alternative.
7. **One pack cache per variant**, `_out/<platform>/<config>/packs/`. A shared content-addressed store across variants is an optimisation for later.

### Why content hashing alone isn't enough, and what covers each part

| A pack is valid only if unchanged | Covered by |
|---|---|
| the primary source | the job edge's input content hash (runner) |
| every file the packer read (includes, sublayers, sidecars) | the manifest's dependency record, turned into a depfile for the edge (runner) |
| the packer itself | the packer binary is an input of the job edge (runner) |
| the rule parameters and version, per variant | part of the job's command line, so of the command hash (runner) |
| the ids of what it references | stable path ids (Decision 3), so a referenced asset's new version doesn't change the referrer |
| who depends on a changed file | the reverse index, for invalidation when a file changes outside a request (used by the server plan's watcher) |

## Steps

1. **`build/framework/packrule.hpp`: the rule type.**

   ```cpp
   auto shaderRule = pack_rule("shader")
                         .match({"shaders/*.vert", "shaders/*.frag", "shaders/*.comp", "shaders/*.geom"})
                         .packer(shaderPacker)
                         .param("optimize", per_config({{"debug", "0"}, {"release", "1"}, {"gamerelease", "1"}}))
                         .param("debug_info", per_config({{"debug", "1"}, {"release", "1"}, {"gamerelease", "0"}}))
                         .version(1);
   p.pack_rule(shaderRule);
   ```

   A rule resolves, per variant, to the packer's output path and a parameter list. Rule lookup for an asset id is the first matching rule in
   registration order; overlapping patterns are an emit-time error.
2. **`build/framework/packtarget.hpp`: static pack targets.** `pack("core").assets(glob(...))` expands, at emit time, to one job edge per asset:
   - command: `<packer> --asset <id> --source <path> --out <dir> --manifest <file> [--param k=v]…`
   - inputs: the source and the packer binary
   - depfile: written by the job from the manifest's dependency record
   - plus one assemble edge that writes the pack container (`_out/<platform>/<config>/packs/core.pack`) from the job outputs
3. **IR and emitter:** a `pack_rules` section in the IR (name, patterns, packer path, parameters and version per variant), written and read like the
   rest of the IR. The rules have to be in the IR because the server resolves runtime requests against them without re-running `build.cpp`.
4. **Runner, requests during a run:** after a pack job succeeds, the runner reads its manifest. Each requested asset id that isn't in the job set yet
   becomes a new job edge, from its matching rule, appended to the plan.
   - This is the one runner change: edges added during a run, with the dirty checks and the log exactly as for static edges.
   - Requests for an id with no matching rule are an error in the requesting job.
5. **The pack log and reverse index:**
   - pack job entries go in `_out/<platform>/<config>/packs/.ngen-packlog`, in the build-log format, keyed by `pack:<asset id>`
   - the reverse index (`packs/.ngen-packdeps`) maps each dependency path to its asset ids, rewritten when a job succeeds
6. **The pack container** (`src/pack/packfile.h/.cpp`, no engine dependencies, readable by any tool):
   - a header, then a chunk table sorted by asset id: id, type (string), version (content hash), offset and size
   - aligned chunk bytes
   - read by memory-mapping, with lookup by id through a binary search
   - written by `ngen-packer-assemble` (main in `src/apps/packerassemble.cpp`), the program the assemble edge runs
7. **`ngen-packer-shader`** (main in `src/apps/packershader.cpp`, the packing code in `src/pack/shader/`): runs `glslc` with the rule's flags and `-MD`, and writes:
   - the SPIR-V as the chunk
   - the manifest, with the `#include`s from the depfile as the dependency record
   - no requests
8. **Core pack and renderer:**
   - `build.cpp` gets `pack("core")` with every shader and the fallback texture; ngen-view depends on it; the `shaders` tool goes away
   - `loadShaderModule(device, stage, id)` looks the id up in the core pack, which is memory-mapped at start-up by a small `PackSet` in the renderer
   - the call sites change from `"shaders/gbuffer.vert.spv"` to `"shaders/gbuffer.vert"`
9. **Docs:** `src/pack/README.md` (ids, container, packer contract, manifest), `build/build_system.md` (rules, static pack targets, requests during a
   run), `src/renderer/README.md` (the core pack).

## Verification

- **Byte-identical SPIR-V.** Every shader chunk in `core.pack` is byte-identical to the `.spv` the old `shaders` tool produced, for each of `debug`,
  `release` and `gamerelease`.
- **The six headless screenshots** (three_cubes and Sponza, prepass on and off) are byte-identical to the baseline, and validation is clean.
- **Dirty rules**, each checked by what reruns:
  - editing one shader repacks that shader and the assemble edge only
  - editing a file included by a shader repacks every shader that includes it, and only those
  - changing a rule parameter in `build.cpp` repacks every asset of that rule
  - relinking the shader packer with a code change repacks every shader
  - changing nothing runs nothing
- **Requests:** a test packer requesting another asset (`src/pack/test/`) adds that job in the same run. A request with no matching rule fails the
  requesting job with the id in the message.
- **Reverse index:** after a build, the index maps an included file to exactly the shaders that include it.
- **Ids:** chunk ids in `core.pack` are the project-relative paths, identical across variants. Only the versions differ.

## Deferred / follow-ups

- **The texture packer.** Trigger: the scene format plan defines material references.
- **Long-running packer workers.** Trigger: process start-up shows up in pack times, for example many small textures.
- **A shared content-addressed store across variants.** Trigger: disk use or pack times from duplicate variant packs matter.
- **Async shader loading and hot reload (the pipeline registry), and moving optional shaders out of the core pack**: `plan_async_shaders.md`.
  Trigger: the build server takes requests.
