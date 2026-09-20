# Frustum culling with a frozen-frustum debug view

**Status. Landed.**

## Current state

Every instance is drawn every frame. Sponza from inside the courtyard (`--camera=5.309,11.274,1.232,-169.80,-0.20`) draws 407 instances,
3.75 M triangles, in the geometry pass and again in the shadow pass; the geometry pass costs 25.9 ms on the Radeon 890M, of which
roughly 6 ms is vertex and raster work (the depth-only shadow pass on the same triangles costs 6.2 ms). Most of those meshes are behind the
camera or outside the view.

`RenderMeshInstance::worldBounds` (`src/renderer/renderworld.h`) already holds a world-space AABB per instance, maintained by the scene
updater and drawn by the "Show AABBs" overlay in `editorui.cpp`. `GpuInstance` (`src/renderer/passes/geometrypass.h`) mirrors
`world.meshInstances` one-to-one, in order. The camera matrices travel to the render thread in `RenderSnapshot` (`main.cpp` builds it, about
line 816). The shadow frustum is fitted to the union of the instance AABBs (`Renderer::sceneBounds` in `renderer.cpp`), so every instance
is inside it; shadow-pass culling has nothing to remove until cascades exist.

Sibling plans from the same Sponza analysis: `docs/plan_texture_mipmaps.md`, `docs/plan_geometry_pass_cost.md`. Independent; this one
is the vertex-side fix, those two are fragment-side.

## Scope

**In**

- Per-instance AABB versus camera frustum test on the main thread while the snapshot is built; a visibility bitmask travels in the
  snapshot; the geometry pass skips invisible instances. Shadow pass unchanged.
- Debug: freeze the culling frustum while the camera keeps moving. Frozen frustum drawn as lines; culled instance AABBs drawn in red,
  visible ones in green, on top of the existing "Show AABBs" overlay.
- Counters: `RenderStats.culled` and `drawn` in the obs stream and the Render Debug Frame tab; `culled` in the Performance window's
  `Main.Cull` zone value.
- Session verbs `cull on|off` and `cull freeze|unfreeze` so a headless run can measure and screenshot the frozen view.

**Out**

- Shadow-pass culling against the light frustum. Trigger: cascades or a spot-light shadow, where the light frustum stops covering the scene.
- Occlusion culling, hierarchical Z, meshlets. Trigger: a view where the visible instance set is still too many triangles after this lands.
- Sub-mesh culling. An instance is a prim's material submesh already; the AABB is per prim, so all submeshes of a prim share the result.

## Decisions

- **Cull on the main thread, in the snapshot build.** The main thread owns the camera and `renderWorld.meshInstances` with their AABBs,
  and the debug overlay is drawn there. 407 AABB tests are a few microseconds. The render thread receives a `std::vector<uint8_t>
  visible` aligned with `gpuInstances` and never sees the frustum. Rejected: culling in `Renderer::render`, which would need the AABBs
  copied into `GpuInstance` and a result path back to the editor for the overlay.
- **Freeze on the main thread too.** A frozen `glm::mat4 cullViewProj` next to the camera; `Frozen` means the test uses the stored
  matrix while `cam.viewMatrix()` still feeds the snapshot. Rendering follows the live camera; culling follows the frozen one, which is
  what makes the culled set inspectable.
- **Plane test against the AABB's positive vertex.** Six planes extracted from `proj * view` (Gribb-Hartmann); for each plane pick the
  AABB corner along the plane normal; outside if that corner is behind any plane. Conservative: large AABBs crossing a frustum corner
  pass. Good enough for 115 meshes.

## Steps

1. **Frustum type.** `Frustum` in `src/scene/scenetypes.h` next to `AABB` and `Ray` (it is geometry; `spatialindex` uses it too). The
   cull loop and freeze state live in `src/renderer/culling.h/.cpp` (`CullState`, `cullInstances`); `main.cpp` only feeds the editor
   flags and the camera matrix in and hands the result to the snapshot and the debug overlay.

   ```cpp
   struct Frustum {
       std::array<glm::vec4, 6> planes; // normal.xyz, d; inside when dot(n, p) + d >= 0

       static auto fromViewProj(const glm::mat4& viewProj) -> Frustum;
       auto contains(const AABB& box) const -> bool;
       auto corners(const glm::mat4& invViewProj) -> std::array<glm::vec3, 8>; // for the debug lines
   };
   ```

2. **Snapshot.** `RenderSnapshot` gains `std::vector<uint8_t> visible;` (empty means draw everything, so the RHI examples and any caller
   that does not cull keep working). `main.cpp` snapshot build: `PROFILE_ZONE("Cull")`, build the frustum from the live or frozen
   matrix, fill `visible` in `meshInstances` order, `PROFILE_ZONE_VALUE(culled)`.

3. **Geometry pass.** `GeometryPass::addPass` takes `std::span<const uint8_t> visible`; the draw loop `continue`s when
   `!visible.empty() && !visible[m]`. Descriptor set indexing stays `(imageIndex * instanceCount) + m`, unchanged.

4. **Editor flags and debug draw.** `EditorUI` flags `cullEnabled` (default on), `cullFrozen`, `showCulled`, all in one Culling window
   (Debug > Culling, `src/ui/cullingwindow.*`) with this frame's instance, drawn and culled counts. When frozen, `editorui.cpp` draws
   the frozen frustum's 12 edges in yellow and, when "Show culled" is on, each instance AABB red or green by its result. First landed as
   three Debug menu items plus a freeze checkbox in the Camera window; collected into the window at Henrik's request.

5. **Counters.** `RenderStats` gains `culled` and `drawn` (the renderer reads them from the snapshot); Render Debug Frame tab shows
   "Instances: N drawn, M culled". Session verbs `cull on|off|freeze|unfreeze` in `sessionscript` and `main.cpp applyCommand`.

## Verification

- Sponza at the courtyard camera, headless, 130 frames: `RenderStats.culled` greater than 0 and `draws` for `GeometryPass` in
  `--dump-render-debug` equal to `407 - culled`; `GeometryPass` GPU time lower than 25.9 ms; screenshot pixel-identical to a run with
  `cull off` at the same frame (an overlay-free comparison via two PNGs). Verified: 234 culled, 173 geometry draws, 1.85 M triangles,
  GeometryPass 26.0 to 22.7 ms, PNGs byte-identical. The small gain confirms the pass is fragment bound from this view
  (`docs/plan_texture_mipmaps.md`, `docs/plan_geometry_pass_cost.md`).
- Script: `cull freeze` at frame 10, `camera` to a pose looking back at the frozen position at frame 11, `screenshot` at frame 13 with
  "Show Culled" on: the PNG shows the yellow frustum and red boxes outside it. Agent reads the PNG. Verified with `cull freeze`,
  `cull show`, `camera-frame scene`: yellow frustum from the courtyard, red boxes behind it, green inside.
- three_cubes: `culled` is 0 from the default camera; `cull off` changes nothing in `RenderStats`. Verified.
- Example sweep unchanged (no RHI change).

## Deferred / follow-ups

- Shadow-pass culling against the light frustum, with cascades.
- Occlusion culling. Trigger above.
- Cull statistics per pass in the Render Debug window once the shadow pass culls too.
