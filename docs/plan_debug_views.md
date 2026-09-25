# Debug views

**Status. Landed.**

Stage 6 of [plan_introspection.md](plan_introspection.md). The value readout builds on the capture service ([plan_frame_capture.md](plan_frame_capture.md)).

## Current state

The viewport's view modes (`view lit|albedo|normals|depth|shadowfactor|shadowmap|shadowuv|worldpos|miplevel|cascades`) are chosen in the lighting
pass and can only show what the G-buffer holds. Nothing shows the triangles themselves (wireframe, size), how often pixels are shaded (overdraw), or
which instance, mesh, material or primitive covers a pixel. Nothing reads a value back under the cursor.

## Scope

**In**

- **Geometry shaders (RHI).** `RhiShaderStage::Geometry`, `RhiGraphicsPipelineDesc::geometryShader` (optional), `RhiDeviceLimits::geometryShaders`.
  Vulkan enables `geometryShader` when supported. The build compiles `shaders/*.geom`.
- **`DebugViewPass`** (`src/renderer/passes/debugviewpass.h/.cpp`): when a debug view is on, it draws the camera's indirect commands again, after
  the geometry pass, with `debugview.vert/.geom/.frag`. It writes one RGBA32F value per pixel into `debugview.value`, tested against the frame's depth
  (`LessOrEqual`, no write). A compute pass `DebugViewResolve` (`debugview.comp`) turns the values into colours in `debugview.color`, which replaces
  the lit scene as the image blitted to the backbuffer. AA is skipped: it would blend IDs.
- **Views** (`DebugView` in `src/renderer/debugview.h`; value channels in brackets):
  - **Wireframe:** shaded surfaces with triangle edges, hidden lines removed (edge distance in pixels, shade).
  - **Triangle size:** each triangle coloured by its screen area on a log scale, red under 1 pixel through blue over 1,000 (area in pixels). The
    small-triangle view from `notes.md`.
  - **Overdraw:** fragments per pixel with no depth test, as a heat map (count). Its own pipeline with additive blending.
  - **Instance, mesh, material and primitive IDs:** a hashed colour per ID (the ID).
  - **UV:** a checker from the texture coordinates, tinted by U and V (u, v).
- **Legend and readout.** A Debug View window (opens with a view): the view's colour scale, and the value under the cursor, read with a one-texel
  capture of `debugview.value`. IDs show their prim path (instance) or table entry. The capture service gains a region
  (`CaptureWatch::regionX/regionY/regionWidth/regionHeight`), and texture dumps of up to 64 texels list the values.
- **Menu and verbs:** Debug menu, a "Debug View" radio group under the buffer views; `debugview <name|off>`; `capture` takes an optional region,
  `capture DebugViewPass debugview.value PATH X Y W H`.

**Out**

- Tangent checks: the vertex format has no tangents yet.
- Fill-mode wireframe (`fillModeNonSolid`). The geometry-shader wireframe removes hidden lines and keeps a constant width; see Decisions.
- Debug views for shadow cascades beyond the existing `cascades` view mode.

## Decisions

Made while planning, following the umbrella's recommendations.

- **Geometry shader rather than line fill mode or the barycentric extension.**
  - A geometry shader sees the whole triangle, so one stage gives the barycentric edge distance (wireframe with hidden-line removal), the screen area
    (triangle size) and `gl_PrimitiveIDIn` (primitive ID).
  - Line fill mode would give neither the area nor the hidden-line removal.
  - `VK_KHR_fragment_shader_barycentric` covers the wireframe, but it is newer and less widely supported. Primitive ID in a fragment shader needs the
    geometry capability anyway.
  - Geometry shaders are slow on some GPUs, but they only run when a debug view is on.
- **A separate pass with a value target, not more G-buffer channels.** The views cost nothing when off. The raw values make the readout exact: an ID
  is read as an ID, not decoded from a colour.
- **Same draws, same descriptor set.** The pass reuses the camera regions' indirect commands and the geometry pass's set 0, so it shows exactly what
  the geometry pass drew.

## Steps

1. RHI: geometry stage and limit; Vulkan feature, stage flags (pipeline, push constants, descriptor bindings); `build.cpp` glob for `*.geom`.
2. `debugview.h` (enum, names), `RenderSnapshot::debugView`, EditorUI state, menu, verb.
3. Shaders: `debugview.vert` (as `gbuffer.vert`, plus flat IDs), `debugview.geom` (edge distances, area, primitive ID), `debugview.frag` (value per
   view), `debugview.comp` (colours).
4. `DebugViewPass`: pipelines (main, cull back and none; overdraw, cull back and none), the resolve pass; `Renderer::render` wires it in after the
   geometry pass and routes `debugview.color` to the blit.
5. Capture region: `CaptureWatch::region`, the copy offset in `recordTexture`, texel lists in the dump; the `capture` verb's region.
6. UI: `DebugViewWindow` with legend and cursor readout.

## Verification

- Every view renders headless on Sponza and three_cubes with validation clean; a screenshot of each shows the expected picture (checked by eye once):
  wireframe edges on shaded surfaces, IDs as flat colour patches, the overdraw heat map brighter where arches overlap.
- Readout, from `capture DebugViewPass debugview.value` with a region on three_cubes:
  - instance ID at a cube's centre pixel equals that cube's instance index in `instances_joined.json`
  - the background reads 0 with alpha 0
  - the overdraw count at a pixel covered by two cubes is 2 or more
- Triangle size: the cube faces (large triangles) read an area in the thousands of pixels.
- `debugview off` returns to the normal image: screenshots byte-identical to the baseline.

## Results

- All eight views render headless on three_cubes and Sponza with validation clean (`--fail-on-validation`, exit 0). Checked by eye:
  - wireframe: shaded cubes and floor with their triangle edges; Sponza's roof tiles dense with edges
  - triangle size: the large walls blue, the roof tiles and small props red and orange
  - overdraw: floor 1, cubes 2, overlaps higher; Sponza's interior arches and columns hot
  - instance, mesh, material and primitive IDs as flat colour patches; UV checker on Sponza (the three_cubes cubes have no texture coordinates, so
    they read 0)
- Readout on three_cubes, from `capture DebugViewPass debugview.value PATH X Y W H` (texels listed in the JSON):
  - instance view at (1496, 736): 0, `/World/Cube`; the screenshot pixel there is (230, 78, 197), the hash colour of ID 0
  - background at (100, 100): `[0, 0, 0, 0]`
  - overdraw at (1152, 716), where two cubes overlap the floor: 3
  - triangle size at (1496, 736): 10,458 px, a cube face
- On three_cubes, mesh and material views look the same: each cube's mesh and material indices are equal (1–4 in `instances_joined.json`).
- With `debugview off` the six screenshots are byte-identical to the baseline.
- The Debug View window shows the legend (triangle-size scale 1 px, ~32 px, 1000+ px) and waits for the cursor; the live readout under a real
  cursor is not checked headless.
