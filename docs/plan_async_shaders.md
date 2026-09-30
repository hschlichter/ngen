# Async shaders

**Status. Draft.**

Part of step 2 of [plan_tool_architecture.md](plan_tool_architecture.md). It builds on [plan_asset_server.md](plan_asset_server.md): shaders are
requested with `asset.request` and arrive streamed over the connection through `AssetClient`, and a view doesn't start without an asset server.

## Current state

- **Every pass builds its pipelines once**, in `init()`:
  - it loads its modules with `loadShaderModule(device, stage, id)` (18 calls in `src/renderer/passes/`)
  - it creates its pipelines directly on the device: 18 graphics and compute pipelines across `GeometryPass`, `DepthPrepass`, `ShadowPass`,
    `LightingPass`, `AAPass`, `InstanceCullPass`, `DebugRenderer`, `GizmoPass` and `DebugViewPass`
  - it keeps the pointers for the program's lifetime

  Nothing can replace a module or pipeline while running, and a pass can't exist before its shaders do.
- **After [plan_asset_server.md](plan_asset_server.md)**, shaders are packed assets with path ids (`shaders/gbuffer.vert`), requested from the asset
  server. The view requests the 17 ids on `startupShaderIds()` in one batch, waits, and `loadShaderModule` takes the bytes from `AssetClient`. Every
  asset arrives with a version, the content hash of its packed file, but nothing uses it.
- **Retiring GPU objects already works.** `DeletionQueue` destroys an object once the fence of the frame that last used it has passed.
- **One pipeline isn't ours.** ImGui's pipeline belongs to the ImGui backend (`src/imguibackendvulkan.cpp`), with shaders compiled into it.

## Scope

**In**

- **`PipelineRegistry`** (`src/renderer/pipelineregistry.h/.cpp`), the only place pipelines are created.
  - A pass registers each pipeline once, as a request: the pipeline description without modules, plus shader ids.
  - It gets back a stable `PipelineHandle`, and asks `registry.get(handle)` at record time.
  - The registry creates and destroys the `RhiPipeline`s behind the handles.
- **A shader module cache** in the registry, keyed by shader id. A module shared by several pipelines, such as `shaders/shadow.frag`
  (shadow pass and depth prepass), is created once per version.
- **Start-up from the registrations.** Pipelines are registered as start-up or on first use. After the passes' `init()`, the registry requests the
  shader ids of every start-up pipeline in one `asset.request`, and the view waits for them before its first frame. `startupShaderIds()` goes away.
- **Pending pipelines.** A pipeline whose shaders haven't arrived is pending, and `get` returns null. The pass decides what that means: skip, or fall
  back. Only optional passes may register pipelines on first use, so the frame always renders.
- **Debug view on first use.** `DebugViewPass` registers its pipelines on first use, the first feature to do so. Its four `debugview.*` shaders are
  requested when a debug view is first turned on, not at start-up.
- **Observability.**
  - Events: `ShaderLoaded` (id, version), `PipelineCreated`, `PipelineCreateFailed` (reason), `PipelinePending`, `ShaderPackFailed` (id, errors).
  - A `pipelines` record, for the introspection model: name, shader ids and versions, state (ready, pending or failed), and the last error. It shows in the Render Debug window.

**Out**

- **Hot reload**: new versions of a running shader, rebuilding its pipelines, and how a source edit reaches the view. See Deferred.
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
   pointers replaced by `ShaderRef { std::string id; RhiShaderStage stage; }`. Every span becomes an owned `std::vector`, so the registry can create the
   pipeline at any time. Descriptor set layouts stay owned by the passes, which outlive their registry entries.
3. **Pipelines are created at one point in the frame.** `PipelineRegistry::update()` runs on the render thread before the frame graph is built.
   - It creates pending pipelines whose shaders arrived.
   - It has a budget per frame (four pipelines to start), so a feature with many pipelines spreads their creation over a few frames instead of
     stalling one.
   - Within a frame, `get` returns the same pipeline for the whole recording.
   - At start-up the budget doesn't apply: every start-up pipeline is created before the first frame.
4. **What an optional pass does while pending: skip.** The debug view pass isn't added to the graph until its pipelines are ready, and the lit image
   shows until then. A pass that must draw something registers its pipelines as start-up. That's the rule that keeps "the frame always renders" true
   without fallback shaders.
5. **Shaders reach the render thread through one queue.** `AssetClient` assembles streamed assets on its connection's I/O thread and pushes each
   finished one (id, version, bytes) to a queue the registry drains at its update point. The registry sends its requests with `AssetClient::request`,
   which only queues bytes, so the render thread never waits on the network after start-up.
## Steps

1. **`PipelineRegistry`**: `registerGraphics(name, GraphicsPipelineRequest, PipelineUse)`, `registerCompute(name, ComputePipelineRequest,
   PipelineUse)`, `get(PipelineHandle) -> RhiPipeline*`, `requestStartup()`, `update(frame)`, `destroy()`, with
   `enum class PipelineUse { Startup, FirstUse }`.
   - It keeps a module cache keyed by id and version, and a reverse map from shader id to pipelines.
   - Retirement goes through `DeletionQueue::defer` with the frame of the last use.
   - Debug names stay the ones the passes use today, for example `geometry.pipeline.cullback.less`.
2. **Migrate the passes**, one at a time: `GeometryPass`, `DepthPrepass`, `ShadowPass`, `LightingPass`, `AAPass`, `InstanceCullPass`,
   `DebugRenderer`, `GizmoPass`, `DebugViewPass`.
   - `init()` registers requests instead of creating pipelines, and `addPass`/`execute` call `registry.get(handle)`.
   - `loadShaderModule`, `setShaderSource` and `startupShaderIds()` go away; the registry takes shaders from the queue (Decision 5).
3. **Start-up** (`src/renderer/renderer.cpp`, `src/apps/view.cpp`): after the passes' `init()`, `requestStartup()` sends one `asset.request` for the
   start-up pipelines' shader ids, and the view waits on `AssetClient::wait` for them before the first frame. A failed start-up shader stops the view
   with the packer's errors.
4. **The debug view on first use:** `DebugViewPass` registers its pipelines with `PipelineUse::FirstUse`. The first `get` of one sends the request
   for its shaders; the pass is skipped until they are ready.
5. **The `pipelines` record, and the events.**

## Verification

- The six headless screenshots are byte-identical to the baseline, and validation is clean.
- At start-up the view sends one `asset.request` with the 13 start-up shader ids, all but the four `debugview.*`.
- The `pipelines` record lists the same 18 pipelines with the same debug names. After the first frame every start-up pipeline is `ready`, and the
  debug view pipelines are `pending`.
- `debugview wireframe` on three_cubes:
  - the first frame after the verb has no debug view pass (the frame graph shows it absent) and is the lit image
  - a `asset.request` for the four `debugview.*` ids goes out; after they arrive, `PipelineCreated` events appear for the debug view pipelines, and
    the next screenshot is the wireframe view, byte-identical to the stage 6 wireframe screenshot
  - turning the debug view off and on again sends no further request
- `startupShaderIds` and `loadShaderModule` no longer exist (`rg`).
- A frame's `get` for a handle returns the same pointer throughout the frame's recording (debug assertion in the registry).

## Gaps

- **Stalls.** Pipeline creation runs on the render thread. A large shader can still cost a frame noticeably, even within the budget.
- **No hot reload:** a shader edit shows after a restart of the view.
- **No view without an asset server,** so no shipping build yet (asset server plan).

## Deferred / follow-ups

- **Hot reload.** A new version of a shader rebuilds every pipeline using it at the update point, swaps the handle, and retires the old pipeline
  through `DeletionQueue`; a failed rebuild keeps the old one. How a source edit reaches the view is the asset server plan's deferred edit handling.
  Trigger: the architecture is up and running.
- **Shader reflection and interface checks**, for edits that change bindings, push constants or vertex inputs. Trigger: hot reload lands, or the
  introspection pipeline state tables are picked up.
- **Pipeline creation on worker threads.** Trigger: first-use creation shows up as frame spikes in the Performance window.
- **A disk pipeline cache.** Trigger: start-up pipeline creation time matters.
- **More optional features on the first-use path**, such as material shader permutations. Trigger: material variants exist.
