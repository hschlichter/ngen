# Frame capture service

**Status. Landed.**

Stage 2 of [plan_introspection.md](plan_introspection.md): the backbone the frame debugger (stage 3), the GPU scene inspector (stage 4) and the
debug-view readout (stage 6) build on.

## Current state

Three separate ways read GPU data back:
- the culling readback in `DrawLists` (counts, visibility, commands)
- screenshots and texture dumps (copies recorded after the frame graph in `Renderer::render`)
- the frame-graph previews, which blit colour textures into thumbnails at their last use through the graph's debug capture hook

None of them can show an arbitrary resource at an arbitrary point in the frame. None of them shows buffers in a readable form. Depth and float
textures have no view.

## Scope

**In**

- **`CaptureService`** (`src/renderer/capture.h/.cpp`). Capture any frame-graph resource, texture or buffer, right after any pass, or a static scene
  buffer (mesh table, material table) at the end of the frame.
  - The graph provides the capture points: an `FgCaptureRequest { pass, resource, callback }` runs after that pass executes, with the resource's
    physical object and current state.
  - The service records a copy into a host-visible buffer, restoring the resource's state around it, and parses the bytes after the slot's fence.
- **Watches.** The main thread sends a list of watches (`CaptureWatch { id, pass, resource, live, trigger, display }`), latest-only through
  `RenderThread`. A watch captures when its trigger changes (one-shot) or every frame while `live`.
  - Results come back as `CaptureResult`: the metadata, the raw bytes (shared, no copy), per-channel min and max for textures, and a display
    preview.
- **Texture display.** The render thread converts the raw texels to RGBA8 with the watch's display settings (channels, range auto or manual,
  depth linearisation off or on) and uploads them into a preview texture registered with ImGui.
  - Formats: RGBA8 UNORM/sRGB, BGRA8, R8, RG8, RGBA16F, RG32F, RGB32F, RGBA32F, D32.
  - Pixel values under the cursor are decoded on the main thread from the raw bytes.
- **Typed buffer views.** `gpuschema.h/.cpp` describes every struct that crosses to the GPU: field name, offset and type (`u32`, `i32`, `f32`, `vec3`,
  `vec4`, `mat4`, `bits`). It covers `GpuInstanceRecord`, `GpuMeshEntry`, `GpuMaterial`, `RhiDrawIndexedIndirectCommand`, `CullParams`, the counter
  layout and plain `u32` arrays. A resource name maps to its schema, and the UI and the JSON dump decode rows with the same code.
- **Capture window** (Windows > Introspection > Capture):
  - pick a pass and one of the resources it accesses (or a static buffer); Capture, Live, Dump
  - textures: preview with zoom, channel toggles, range, value under cursor, stats
  - buffers: paged schema table, raw hex toggle, row jump
  - The Frame Graph window's resource details get a "Capture" button that opens it on that resource.
- **Dump and verb:** `capture <pass> <resource> <path>`.
  - Buffers: a JSON file with the schema and every decoded row, capped at 65,536 rows.
  - Textures: a PNG of the display conversion, plus a `.json` sidecar with format, size and per-channel min/max.
- **RHI:** `RhiBufferTextureCopy` gains `x` and `y` offsets (copy a sub-rectangle), used by stage 6's cursor readout.
- **Depth:** the renderer's depth texture gains `TransferSrc` usage so it can be captured.

**Out**

- Moving the culling readback and the frame-graph previews onto the service. The umbrella planned it; see Decisions.
- Mip levels other than 0 and array layers other than 0 in the capture view. The texture inspector already covers material mips.

## Decisions

Made while planning, following the umbrella's recommendations.

- **The existing readbacks stay as they are for now. This deviates from the umbrella.**
  - The culling readback runs every frame and feeds statistics, the editor overlay and the draw log. It has fixed per-slot buffers sized with the
    instance buffer, and it is verified byte for byte.
  - The previews are GPU blits into thumbnails, not readbacks.
  - Moving either gains no capability and risks the verified paths. New readbacks use the service; the old ones move when they next need to change.
- **Conversion to display happens on the render thread.** The main thread cannot create GPU textures, and ImGui needs one to draw.
  - The render thread keeps the last raw capture per watch, so changing display settings re-converts without capturing again.
  - The raw bytes still reach the main thread for value readout and dumps.
- **State restore around every copy.** A capture moves the resource to `TransferSrc` and back to its state at that point, so the graph's own barrier
  tracking stays true. This is how the preview hook already works.
- **Capture after, not before.** A pass's inputs are the previous pass's outputs, so "before pass P" is "after the pass before P". The frame debugger
  (stage 3) uses that to show before and after.

## Steps

1. RHI: `x`, `y` in `RhiBufferTextureCopy` (Vulkan `imageOffset`, both copy directions). Depth texture usage gains `TransferSrc`.
2. Frame graph: `FgCaptureRequest`, `FgCaptureSource`, `setCaptureRequests`; requests run after their pass and before transient release; an empty
   pass name means "after the last pass".
3. `gpuschema.h/.cpp`: schemas, `schemaForResource(name)`, `decodeField`.
4. `CaptureService`: watches, per-slot pending copies, parse after the fence, stats, display conversion, preview textures, results;
   `registerStaticBuffer(name, buffer, size)` for the scene tables.
5. `RenderThread`: `setCaptureWatches`, `takeCaptureResults`. `Renderer` owns the service.
6. UI: `CaptureWindow`, the Frame Graph window button, the menu entry. `main.cpp`: the `capture` verb writes the dump when its result arrives.

## Verification

- `capture InstanceCullScatter drawCommands <file>.json` on Sponza (frame 150, courtyard camera):
  - the first 65 rows of region 0 are the camera draws
  - each row decodes `indexCount`, `firstIndex`, `vertexOffset`, `firstInstance`
  - the rows' `firstInstance` sequence equals the stage 5 draw log's instance order
- `capture InstanceCullScan drawCounts` decodes 342 in `cameraCulled` (field 20) and 65 in region 0.
- `capture GeometryPass gbuffer.normal <file>.png`: a PNG plus a JSON sidecar with format `R16G16B16A16_SFLOAT`, size 2560×1440, and channel ranges
  in [0, 1].
- `capture DepthPrepass depth` with prepass on, and `capture GeometryPass depth`: D32 captured, min above 0 and max at most 1.
- `capture "" gpuscene.meshtable` decodes 115 non-empty mesh entries whose `indexCount` values match the Render Debug mesh table.
- Validation stays clean with captures active (the state restore is correct), and the screenshot of a frame with captures is byte-identical to one
  without.

## Results

Sponza, courtyard camera:

- `capture InstanceCullScatter drawCommands`: region 0 starts with the 65 camera draws; their `firstInstance` order equals the draw log's.
- `capture InstanceCullScan drawCounts`: `cameraCulled` 342, `draws.r0` 65.
- `gbuffer.normal` captured as `R16G16B16A16_SFLOAT` 2560×1440 with PNG and sidecar; depth captured as D32 with and without the prepass.
  Single-channel formats display as grey.
- `capture "" gpuscene.meshtable` matches the Render Debug mesh table.
- Captured buffers needed `TransferSrc` usage (`gpuscene.instances`, the mesh and material tables, `cull.readback`); validation named each one, and
  they now have it. Validation clean with captures active; screenshots byte-identical.
- Stage 6 added a region to watches (`regionX/regionY/regionWidth/regionHeight`) and texel lists in small texture dumps.
