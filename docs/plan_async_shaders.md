# Async shaders and hot reload

**Status. Draft.**

Part of step 2 of [plan_tool_architecture.md](plan_tool_architecture.md). It depends on [plan_pack_rules.md](plan_pack_rules.md) (the shader packer
and the core pack). Phase B also needs RPC (step 1) and the build server's `pack.request`, `pack.ready` and file
watching ([plan_build_server.md](plan_build_server.md)).

## Current state

- **Every pass builds its pipelines once**, in `init()`:
  - it loads its modules with `loadShaderModule(device, stage, path)` (18 calls in `src/renderer/passes/`)
  - it creates its pipelines directly on the device: 18 graphics and compute pipelines across `GeometryPass`, `DepthPrepass`, `ShadowPass`,
    `LightingPass`, `AAPass`, `InstanceCullPass`, `DebugRenderer`, `GizmoPass` and `DebugViewPass`
  - it keeps the pointers for the program's lifetime

  Nothing can replace a module or pipeline while running, and a pass can't exist before its shaders do.
- **After [plan_pack_rules.md](plan_pack_rules.md)**, shaders are packed assets with path ids (`shaders/gbuffer.vert`), read at start-up from
  `<out_dir>/packs/<asset id>`, where the core pack target writes them. Nothing notices when a packed shader changes.
- **Retiring GPU objects already works.** `DeletionQueue` destroys an object once the fence of the frame that last used it has passed.
- **One pipeline isn't ours.** ImGui's pipeline belongs to the ImGui backend (`src/imguibackendvulkan.cpp`), with shaders compiled into it.

## Scope

**In**

- **`PipelineRegistry`** (`src/renderer/pipelineregistry.h/.cpp`), the only place pipelines are created.
  - A pass registers each pipeline once, as a request: the pipeline description without modules, plus shader ids.
  - It gets back a stable `PipelineHandle`, and asks `registry.get(handle)` at record time.
  - The registry creates, rebuilds and retires the `RhiPipeline`s behind the handles.
- **A shader module cache** in the registry, keyed by shader id and version. A module shared by several pipelines, such as `shaders/shadow.frag`
  (shadow pass and depth prepass), is created once per version.
- **Pending pipelines.** A pipeline whose shader version isn't available yet is pending, and `get` returns null. The pass decides what that means:
  skip, or fall back. Only optional passes may use shaders outside the core pack, so the frame always renders.
- **New versions rebuild.**
  - When a shader id gets a new version, the registry rebuilds every pipeline that uses it at the next update point.
  - It swaps the handle to the new pipeline and retires the old one through `DeletionQueue`.
  - If the rebuild fails, the old pipeline stays and the failure is reported.
- **The core/optional split.** The core pack keeps the shaders of the always-present passes. `debugview.*` moves to a second pack target, `optional`, and
  becomes the first feature whose pipelines are created on first use.
- **Phase B:**
  - optional shaders are requested from the build server (`pack.request`) when first needed
  - hot reload: the server's watcher repacks an edited shader and sends `pack.ready` with the new version; the registry rebuilds
  - a failed pack (`pack.failed`) keeps the running version and reports glslc's errors
- **Observability.**
  - Events: `ShaderVersionLoaded` (id, version, source: core, optional or server), `PipelineRebuilt`, `PipelineRebuildFailed` (reason),
    `PipelinePending`.
  - A `pipelines` record, for the introspection model: name, shader ids and versions, state (ready, pending or failed), rebuild count and the last
    error. It shows in the Render Debug window.

**Out**

- **Reflection.** Detecting that an edit changed a shader's bindings, push constants or inputs needs SPIR-V reflection. That is deferred (see Gaps).
- **ImGui's pipeline**, which stays inside the ImGui backend.
- **The RHI examples** (`src/rhi/examples/`), which compile their shaders with shaderc at runtime and don't use packs.
- **Pipeline creation off the render thread.** See Deferred.
- **A pipeline cache on disk** (`VkPipelineCache`). See Deferred.

## Decisions

1. **The registry lives in the renderer; the RHI is unchanged.** Locked with Henrik, as option b in the umbrella.
   - The alternative was an RHI call that rebuilds pipelines in place. That would make the backend keep copies of every pipeline description,
     engine-level state the RHI deliberately doesn't keep (`src/rhi/README.md`, principles).
   - The registry owns the descriptions it needs, and uses only public RHI calls.
2. **Pipelines are requested by description plus shader ids.** A request is `RhiGraphicsPipelineDesc` or `RhiComputePipelineDesc` with the module
   pointers replaced by `ShaderRef { std::string id; RhiShaderStage stage; }`. Every span becomes an owned `std::vector`, so the registry can rebuild
   at any time. Descriptor set layouts stay owned by the passes, which outlive their registry entries.
3. **Rebuilds happen at one point in the frame.** `PipelineRegistry::update()` runs on the render thread before the frame graph is built.
   - It creates pending pipelines whose shaders arrived, and rebuilds pipelines whose shaders changed.
   - It has a budget per frame (four pipelines to start), so a hot reload of a widely used shader spreads over a few frames instead of stalling one.
   - Within a frame, `get` returns the same pipeline for the whole recording.
4. **What an optional pass does while pending: skip.** The debug view pass isn't added to the graph until its pipelines are ready, and the lit image
   shows until then. A pass that must draw something has to use core shaders. That's the rule that keeps "the frame always renders" true without
   fallback shaders.
5. **Before the server exists, "async" means lazy creation from the optional pack.** Phase A packs the optional shaders statically, next to the core
   pack, so offline and headless runs keep working. The registry creates their pipelines on first use. Phase B adds requests to the server, with
   the static optional pack as the fallback when no server is connected.
6. **Versions come to the render thread through one queue.** `pack.ready` arrives on the main thread (RPC). The main thread hands shader versions to
   the render thread through a `RenderThread::submitShaderVersions` queue, which the registry drains at its update point. Reading packed files is done
   on the main thread; only module and pipeline creation is on the render thread.

## Steps

### Phase A: the registry, and lazy optional pipelines

1. **`PipelineRegistry`**: `registerGraphics(name, GraphicsPipelineRequest)`, `registerCompute(name, ComputePipelineRequest)`,
   `get(PipelineHandle) -> RhiPipeline*`, `update(frame)`, `setShaderVersion(id, version, spirv)`, `destroy()`.
   - It keeps a module cache keyed by id and version, and a reverse map from shader id to pipelines.
   - Retirement goes through `DeletionQueue::defer` with the frame of the last use.
   - Debug names stay the ones the passes use today, for example `geometry.pipeline.cullback.less`.
2. **Migrate the passes**, one at a time: `GeometryPass`, `DepthPrepass`, `ShadowPass`, `LightingPass`, `AAPass`, `InstanceCullPass`,
   `DebugRenderer`, `GizmoPass`, `DebugViewPass`.
   - `init()` registers requests instead of creating pipelines, and `addPass`/`execute` call `registry.get(handle)`.
   - `loadShaderModule` goes away; the registry reads packed shaders from the packs directory.
3. **The optional pack**: a second static pack target (`pack("optional")`) holding `debugview.*`, which ngen-view depends on. It writes into
   the same packs directory as the core pack. A `debugview` request stays pending until first use: the renderer asks for the debug view pipelines when a debug view is turned on.
4. **The `pipelines` record, and the four events.**

### Phase B: requests and hot reload (after the build server)

5. **Requests:** when a shader id hasn't been packed and a build server is connected, `pack.request` is sent. On `pack.ready` the packed
   file is read, and the version goes to the render thread (Decision 6).
6. **Hot reload:** a `pack.ready` for an id that already has a version is handled the same way, since a newer version wins. That includes an id in
   the core pack.
7. **Failures:**
   - `pack.failed` for a shader emits `ShaderPackFailed` with glslc's message and changes nothing.
   - A `PipelineRebuildFailed` keeps the old pipeline, and the handle keeps pointing at it.

## Verification

**Phase A**
- The six headless screenshots are byte-identical to the baseline, and validation is clean.
- The `pipelines` record lists the same 18 pipelines with the same debug names. After the first frame every core pipeline is `ready`, and the debug
  view pipelines are `pending`.
- `debugview wireframe` on three_cubes:
  - the first frame after the verb has no debug view pass (the frame graph shows it absent) and is the lit image
  - within the update budget, `PipelineRebuilt` events appear for the debug view pipelines, and the next screenshot is the wireframe view,
    byte-identical to the stage 6 wireframe screenshot
- A frame's `get` for a handle returns the same pointer throughout the frame's recording (debug assertion in the registry).

**Phase B**
- With a build server running, the first `debugview wireframe` sends `pack.request` for the four `debugview.*` ids. The view appears after
  `pack.ready`, and there are no requests on later toggles.
- **Hot reload:** changing a constant in `shaders/lighting.frag` while Sponza runs gives:
  - a `pack.ready` with a new version
  - `ShaderVersionLoaded`, then `PipelineRebuilt` for the lighting pipeline only
  - a changed image, with no restart and validation clean
  - the old pipeline destroyed after its fence (a `DeletionQueue` flush event)
- **Includes:** editing a file included by several shaders rebuilds exactly the pipelines using those shaders.
- **A syntax error:** `pack.failed` with glslc's line, and the image and pipelines unchanged. Fixing the error reloads normally.
- **Without a server,** the same run in Phase B mode behaves as Phase A.

## Gaps

- **Interface changes.** An edit that changes bindings, push constants or vertex inputs is rebuilt against the pass's existing layouts. Pipeline
  creation may fail (reported, old kept), or succeed and produce validation errors at draw time. Reflection data written by the shader packer, checked by
  the registry before swapping, closes this. It's deferred from [plan_introspection.md](plan_introspection.md).
- **Stalls.** Pipeline creation runs on the render thread. A large shader can still cost a frame noticeably, even within the budget.
- **ImGui's pipeline** doesn't hot reload.
- **Shipping builds** don't connect to a server, so they never hot reload or request. Everything they use must be in their static packs.

## Deferred / follow-ups

- **Shader reflection and interface checks.** Trigger: an interface-changing edit causes a validation error during hot reload, or the introspection
  pipeline state tables are picked up.
- **Pipeline creation on worker threads.** Trigger: rebuilds or first-use creation show up as frame spikes in the Performance window.
- **A disk pipeline cache.** Trigger: start-up pipeline creation time matters.
- **More optional features on the pending path**, such as material shader permutations. Trigger: material variants exist.
