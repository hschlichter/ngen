# Debug names and GPU memory

**Status. Landed.**

Stage 1 of [plan_introspection.md](plan_introspection.md).

## Current state

RHI objects have no names: validation messages and RenderDoc show raw handles. `VK_EXT_debug_utils` is already enabled when available, but only for
command buffer labels and the validation messenger. Nothing records what GPU memory exists: every buffer and texture is its own `vkAllocateMemory`
(`src/rhi/README.md`, Vulkan backend), and the only byte counts are the ones individual systems report (`geometry_pool_bytes`, `instance_buffer_bytes`).

## Scope

**In**

- **Debug names.** `debugName` on the creation descs of buffers, textures, samplers, shader modules, and graphics and compute pipelines. Plus
  `RhiDevice::setDebugName(RhiDebugObject, name)` for everything else (descriptor set layouts, pools and sets, command buffers, fences, semaphores,
  query pools) and for renaming. The Vulkan backend names the Vulkan objects behind each (memory and image views included).
- **Allocation registry** in the RHI:
  - `RhiDevice::allocations(out)` lists every live buffer and texture (name, kind, bytes allocated, memory usage, usage flags, creation sequence).
  - `RhiDevice::memoryHeaps(out)` lists heaps (size, device-local, and budget and usage from `VK_EXT_memory_budget` when available).
- **Names on every engine allocation and object** in `src/renderer/`, the ImGui backend and `main.cpp`.
- **Memory window** (Windows menu): heaps with budget bars, totals per memory usage and per category, and a sortable, filterable allocation table.
- **Dump and verb:** `--dump-memory=PATH` and `dump-memory PATH` write the same data as JSON.

**Out**

- A suballocator, and aliasing of transient memory. The registry shows what exists; changing the allocation strategy is a separate decision.
- Swapchain images. The swapchain owns them, and they are not allocated through `createTexture`.

## Decisions

Made while planning, following the umbrella's "go with the recommendations".

- **Names on descs for creation, one `setDebugName` for the rest.** Descs with a `debugName` field name the object at birth, when it matters most (the
  allocation registry sees the name immediately). A single `setDebugName` taking a small tagged pointer (`RhiDebugObject`) avoids one virtual per type.
- **Categories come from names.** An allocation's category is the part of its name before the first `.` or `:` (`gpuscene.instances` →
  `gpuscene`). This needs no extra field, and it keeps names meaningful.
- **Memory reaches the UI through the render debug snapshot.** The registry is read on the render thread into `RenderDebugSnapshot`, which the Memory
  window and the dump read. The snapshot is produced while either the Render Debug or the Memory window is open, or a dump is pending.

## Steps

1. RHI: `debugName` fields; `RhiDebugObject`; `setDebugName`; `RhiAllocationInfo`, `RhiMemoryHeapInfo`, `allocations()`, `memoryHeaps()`.
2. Vulkan:
   - Load `vkSetDebugUtilsObjectNameEXT` when debug utils is on, and name objects at creation.
   - Keep a mutex-guarded registry keyed by the RHI object, updated on create, destroy and rename.
   - Enable `VK_EXT_memory_budget` when present and read it in `memoryHeaps()`.
3. Engine: name every `createBuffer`/`createTexture`/sampler/shader/pipeline and the objects set by name.
   - `GpuUploader::uploadBuffer` takes a name.
   - `loadShaderModule` names modules after their file.
   - The frame graph renames pooled transient textures to the resource that acquires them.
4. `RenderDebugSnapshot` gets `allocations` and `heaps`; a `MemoryWindow` in `src/ui/`; the JSON writer; the flag and verb.

## Verification

- `--dump-memory` on Sponza:
  - lists the geometry pool buffers, and their sizes sum to at least `geometry_pool_bytes` (allocation alignment may add bytes)
  - lists every material texture (28 plus the fallback), the instance buffer, the per-slot culling buffers and the transient G-buffer textures,
    each with a name
- No allocation in the dump has an empty name.
- Heaps report a size, and a budget where `VK_EXT_memory_budget` exists (RADV has it).
- Validation still exits clean; screenshots unchanged.
- Names reach external tools: checked in stage 8, where a RenderDoc capture shows them.

## Results

- `--dump-memory` on Sponza: no allocation without a name; the geometry pool allocates 674,463,984 bytes for 674,463,960 requested (alignment).
- Heaps report size and budget (`VK_EXT_memory_budget`).
- Validation messages name the objects involved (this is how the missing `TransferSrc` usages in stage 2 were found).
- Screenshots byte-identical; validation clean.
- Material textures are named `material.N.basecolor`, so the Memory window groups them in one `material` category.
