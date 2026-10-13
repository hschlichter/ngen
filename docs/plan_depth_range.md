# Depth range for large scenes

**Status. Landed.** 2026-10-13.

The camera's depth range stops being a fixed 0.1 m to 3,000 m. A scene of any size draws from where it is framed, and depth precision holds
up at a distance, so Caldera opens showing its map instead of an empty frame.

## Current state

- **One fixed range, in four places.** The projection is built in `src/apps/view.cpp` (`glm::perspective(45°, aspect, 0.1f, 3000.0f)`), and
  the same `0.1f, 3000.0f` is repeated:
  - for the shadow cascade fit, in `view.cpp` and in `Renderer`'s fallback cascade (`renderer.cpp`);
  - in the lighting pass's `depthParams` (`lightingpass.cpp`), which `lighting.frag` uses to linearise depth for cascade selection and the
    depth preview.
- **Standard depth.** Depth is cleared to 1 and tested `Less`:
  - The geometry pass and depth prepass use `Less`, then `Equal` after the prepass. The debug lines use the default `Less`, and the debug views
    `LessOrEqual`.
  - `lighting.frag` treats depth `>= 1.0` as background.
  - The depth target is `D32_SFLOAT`, where standard depth wastes most of the float's precision near 1.
- **Framing ignores the range.** `Camera::frame` puts the camera where the bounding sphere fits the field of view. Caldera's map frames from
  about 60 km out, past the far plane, so the frame is empty (roadmap, "Depth range and camera framing for large scenes").
- **Culling and overlays assume a finite far plane.**
  - `Frustum::fromViewProj` (`src/scene/scenetypes.h`) derives the far plane from the matrix, and the GPU culling tests all six planes.
  - `Frustum::corners` unprojects NDC z = 0 and 1 for the frozen-frustum overlay.
- **Shadows cover the whole scene.** `fitShadowCascades` (`src/renderer/shadowcascades.cpp`) pulls the far distance in to the scene's
  farthest corner, capped at 3,000 m. For a map kilometres across, that spreads three 1024² cascades over kilometres.
- **The shadow maps** use their own orthographic projections, fitted per cascade, with standard depth and a compare sampler.

## Scope

**In**
- Reversed-Z for the camera: depth cleared to 0, tested `Greater` / `GreaterOrEqual` / `Equal`, background at 0. One projection function
  replaces the four copies of the range.
- An infinite far plane (decision 1), with the near plane at 0.1 m as today.
- Everything that reads camera depth or the far plane:
  - `lighting.frag`: linearising, reconstruction, background test, depth preview;
  - frustum planes and corners;
  - the cascade fit;
  - the debug lines and debug views;
  - the Render Debug window's depth preview.
- A shadow distance: cascades end at `min(scene's farthest corner, maxDistance)` (decision 2).

**Out**
- Reversed-Z for the shadow maps (decision 3).
- Ambient or sky lighting at kilometre scale: the roadmap's "Lighting at kilometre scale", apart from the shadow distance.
- Camera-relative rendering or double-precision transforms. Floats in metres hold Caldera's 3.2 km to about a millimetre; trigger: jitter seen
  at the map's far edge.
- LOD and streaming, which make the framed map light enough to fly over ([plan_streaming_lod.md](plan_streaming_lod.md)).

## Decisions

Locked 2026-10-13, all four as proposed.

1. **Reversed-Z with an infinite far plane.** With a float depth buffer, reversed-Z spreads precision evenly in log space, so an infinite far
   plane costs nothing visible: no far plane to tune per scene, nothing ever clipped at a distance.
   - Rejected: reversed-Z with a far plane fitted to the scene each frame. That precision isn't needed, the far plane changes as the camera
     moves, and culling and cascades would follow it.
   - Rejected: standard depth with a larger far plane. Precision collapses: at 0.1 m to 10 km, depth steps at 1 km are metres apart, which
     z-fights.
   - I lean infinite.
2. **Shadows end at a shadow distance.** `ShadowCascadeSettings::maxDistance`, default 200 m, adjustable in the Shadows window. The cascades
   fit from the near plane to `min(scene's farthest corner, maxDistance)`, and beyond it surfaces are lit without shadow.
   - Sponza (about 30 m across) and three_cubes are under it, so their cascades don't change.
   - Rejected: keeping the fit to the whole scene. Caldera's cascades would be kilometres wide, about a metre per texel.
   - Rejected: fading shadows out over a band before the distance. That's worth doing when the edge shows; it's in Deferred.
   - I lean 200 m as the default, since it covers a district while flying low.
3. **The shadow maps keep standard depth.** Their orthographic depth is linear and fitted tight to each cascade, so reversing it gains no
   precision, and the compare sampler, the bias and `shadow.frag` would all change for nothing.
4. **The fly speed follows the framing.** `Camera::frame` sets the fly speed from the framed distance, about a tenth of it per second, clamped
   to [1, 500] m/s; the speed setting still overrides it.
   - Without this, the map is visible but takes minutes to fly across at today's speed.
   - Rejected: leaving it to the user's speed setting.
   - Pushback welcome: this one is taste, and it changes Sponza's speed when it is framed.
   - **Reverted 2026-10-13**: framed at Caldera's scale the speed was far too fast to control. The fly speed is a fixed 5 m/s again, and
     Shift with the mouse wheel scales it (`Camera::scaleSpeed`).

## Steps

1. **One projection** (`src/camera.h/.cpp`):

   ```cpp
   // The view's projection: reversed-Z (depth 1 at the near plane, 0 at infinity), infinite far plane, the Vulkan y flip.
   inline constexpr float cameraNear = 0.1f;
   inline constexpr float cameraFov = glm::radians(45.0f);

   auto cameraProjection(float aspect) -> glm::mat4;
   ```

   Built by hand, since glm has no reversed infinite form:
   - `m[0][0] = f / aspect`, `m[1][1] = -f` (the y flip), `m[2][3] = -1`, `m[3][2] = near`, and the rest 0, with `f = 1 / tan(fov / 2)`.
   - Clip `z = near`, `w = -z_view`, so depth is `near / -z_view`.

   `view.cpp`, the cascade fit and the lighting pass take it and `cameraNear` instead of the literals. `axis3dgizmo.cpp` keeps its own
   projection: it is a separate small view with no depth test.
2. **Depth state** (passes):
   - The geometry pass and depth prepass use `Greater` with write, and `Equal` stays `Equal`.
   - The debug views use `GreaterOrEqual`, and the debug lines `Greater`.
   - Every camera-depth clear becomes 0.
   - The shadow pass is unchanged.
   - `RhiDepthState`'s default (`Less`) stays as it is, and the camera passes name their compare op explicitly.
3. **Shaders** (`lighting.frag`):
   - The background is `depth <= 0.0`.
   - `linearizeDepth(d)` becomes `near / d`.
   - `depthParams` carries `(near, previewFar)`. The depth preview is a log scale from near to `previewFar`, the scene's farthest corner,
     since there is no far plane.
   - `reconstructWorld` is unchanged, since the matrix is invertible and the background is skipped.
4. **Frustum** (`src/scene/scenetypes.h`):
   - `fromViewProj` takes the planes from the reversed inequalities: near is `z <= w` (`r3 - r2`), and far is `z >= 0` (`r2`).
   - With an infinite projection, `r2` has no xyz part, so the far plane is marked as always passing. The plane is kept as (0, 0, 0, 1), and
     the GPU test already passes it.
   - `corners` takes a far distance for the far corners: the overlay draws the frozen frustum out to the cascades' far distance instead of
     infinity.
   - `instancecull.comp` is unchanged, since it takes the planes as given.
5. **Cascades** (`shadowcascades.*`):
   - `ShadowCascadeSettings` gains `float maxDistance = 200.0f`.
   - `fitShadowCascades(settings, view, proj, nearZ, lightDir, worldUp, sceneBounds, out)` drops `farZ`, and the far distance is
     `min(farthest corner, maxDistance)`.
   - The Shadows window gets a slider, and the `render` record's cascade data shows the distance.
6. **Framing** (`src/camera.*`): `frame()` sets `speed` from the distance (decision 4). An explicit speed from the UI or a script is kept until
   the next frame.
7. **Docs**: `src/renderer/README.md` covers reversed-Z, the depth conventions per pass, the infinite far plane, the shadow distance and the
   depth preview. The roadmap's "Depth range and camera framing" item moves to Landed, and the "Lighting at kilometre scale" item keeps the
   ambient term.

## Verification

- **Caldera opens visibly.** `./ngen-cli view assets/caldera/caldera.usda` with no camera flag:
  - the `render` record shows camera draws above 0;
  - the default-camera screenshot isn't the background colour across the frame;
  - the camera position in the `status` record is about 60 km out, as framed.
- **The capital close up.** With `--camera-frame=/world/mp_wz_island/mp_wz_island_paths/mp_wz_island_geo/map_capital`:
  - the cascades' far distance in the `render` record is 200 m or less;
  - the shadow-factor view shows shadows near the camera;
  - there are no validation messages.
- **The small scenes keep their look.**
  - three_cubes and Sponza at the default camera keep their geometry: the albedo and normals views are byte-identical to `_out/cutout-baseline`
    captures of the same views, taken before the change.
  - The lit view may differ by a little in places, from depth reconstruction. The pixel diff is measured, then inspected.
  - New lit baselines are captured.
- **The prepass still matches.** With the depth prepass on and off, the lit screenshots are byte-identical to each other, as today.
- **Picking.** `pick 499 1216` at `3.293,-1.020,9.798,-176.85,-3.95` in Sponza still selects `/root/_nd_floor/column_2ndfloor_12`.
- **Depth preview.** The Render Debug window's depth preview shows near surfaces bright and far ones dark, with the background black.
- **Culling.** The `culling` record's camera-culled count for Sponza at the default camera matches today's. The frozen-frustum overlay draws a
  finite frustum.
- Debug and release build.

### Results (2026-10-13)

The before set was captured first, in `_out/depthrange-baseline/before`: three_cubes and Sponza in the lit, albedo, normals and prepass views,
with records.
- **Small scenes.** Every pixel that differs before and after, in all six albedo, normals and lit images, is an overlay line (grid, origin,
  light gizmo) showing or hidden where it ties with a surface's depth. A pixel counts as explained when the side without the line matches a render
  with the overlays off. That leaves one unexplained pixel in Sponza lit, out of 1,762 differing. Geometry, lighting and shadows draw as before.
- **Prepass.** Not as planned: prepass on and off already differed before the change, by 50 px on three_cubes and 450 on Sponza (max 91 and 51
  levels), so "byte-identical, as today" was wrong. After the change they differ by 0 and 8 px: reversed-Z resolves the Equal test better.
- **Culling and picking.** Sponza's camera culling is unchanged: 407 drawn, 0 culled, 3,747,022 triangles. `pick 499 1216` and `pick 635 1075`
  at the decal camera still select `column_2ndfloor_12` and `column_2ndfloor_01`.
- **Caldera, default camera.** It draws: 4,427 instances, framed at about 60 km, with every cascade culled at that distance. The frame is
  dominated by a large flat ground disc that sets the map's bounds, with the island small in its middle. That is the scene's content, and
  framing fits it.
- **The capital** (`--camera-frame=…/map_capital`).
  - The lit view runs to the horizon with nothing clipped.
  - The `status` record shows shadow distance 200 and fly speed 218 m/s.
  - The cascades end at 200 m, so they draw only the near ground (1 instance each).
  - The depth preview runs from bright near the camera to dark, with the background black.
  - No validation messages.
- **Frozen frustum.** Frozen and seen from outside, it draws as a finite pyramid out to 200 m.
- **Builds.** Debug and release build.

Changed or added on the way:
- **Gizmo rays.** The gizmos' mouse rays unprojected NDC z = 0, which reversed infinite depth puts at infinity. `Ray::fromCamera` replaces the
  three copies. Dragging isn't exercised headless.
- **Two conventions in the frustum code.** `Frustum::fromViewProj` and `Frustum::corners` serve the camera (reversed) and the cascades
  (standard), so they take the convention, and the plane order stays 4 near, 5 far.
- **Where the distance is reported.** The shadow distance and the fly speed are in the `status` record, not the `render` record. The cascade
  distances themselves aren't recorded.

## Open questions

- Decision 4's numbers (a tenth of the framed distance per second, clamped to [1, 500] m/s) are a first guess, to be tuned on Caldera.

## Deferred / follow-ups

- **Shadow fade** over a band before `maxDistance`. Trigger: the shadow edge shows while moving.
- **Ambient or sky term** for faces turned from the sun (roadmap, "Lighting at kilometre scale"). Trigger: Caldera's unlit sides, now that it is
  visible.
- **Camera-relative rendering.** Trigger: vertex jitter far from the origin.
