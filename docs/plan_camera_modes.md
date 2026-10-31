# Camera modes: orbit and ground following

**Status. Landed.** Decisions locked 2026-10-31 (Henrik): built on what exists, nothing new to accommodate collision. Built
2026-10-31. Builds on
[plan_debug_camera.md](plan_debug_camera.md): the modes act on the camera that has the controls (the main camera, or the debug camera
while it is on).

## Current state

The view's camera is a fly camera only (`src/camera.cpp`): WASD and Q/E move it, right-drag looks around in place, the wheel scales the
speed (`src/apps/view.cpp`'s event loop). Picking copies one texel of `gbuffer.instance` under the cursor through the capture service and
hands back the instance a few frames later (`ViewPicking`); it carries no depth.

What the main thread already holds, and the ground test reads: `RenderWorld::meshInstances` (every draw's world transform, world bounds,
mesh handle and prim), and every mesh's `MeshDesc` (vertices and indices), which the scene thread's mesh library makes and the main thread
passes to the renderer in `SceneChanges::meshesAdded`.

Nothing in the engine reads collision. The streaming test world authors it as UsdPhysics says (`physics:collisionEnabled` on each LOD root's
default level, on the mesh prims, regenerated 2026-10-31), and a level that collides is often not the one drawn: near the camera a ground
chunk draws `fine` while `base` collides, and a resident level that isn't drawn has no draws (`placement.cpp`, held entries only).
Collision belongs to Physics, roadmap step 5 ([roadmap.md](roadmap.md)): Jolt, the packer reading UsdPhysics colliders, physics as a UsdLod
domain of its own.

## Scope

**In**
- Orbit while Left Alt is held, around the surface under the cursor.
- Ground following: the camera kept at a height above the render geometry under it.
- RPC, verbs, the `status` record, overlays, the Camera window, the Engine Map.

**Out**
- Ground from collision geometry, and the switch between collision and render geometry in the Camera window: deferred to Physics (below),
  where it is a ray cast against the colliders the physics domain makes resident. Nothing is built here to accommodate it: no collision
  attribute carried by the packer, no collision flag on draws, no list of colliders.
- Any acceleration structure (BVH, grid, index): the ground test loops the draws and tests triangles by brute force.
- Walking (walls, slopes, gravity): the camera passes through everything.

## Decisions

### 1. What orbit turns around

**The surface under the cursor when Alt goes down**: the pick also copies the `gbuffer.depth` texel, turned into a world point. Every
scene, a few frames late; until it arrives (and over the sky, depth 0) the camera turns around the point at the last pivot's distance along
the cursor ray, 10 m the first time.

### 2. Orbit's controls

While Left Alt is held: right-drag turns around the pivot (yaw around the world's up through the pivot, pitch around the camera's right,
the distance kept); the wheel moves toward and away from the pivot (each step a fraction of the distance, never through it); WASD and Q/E
fly as before and carry the pivot along (a pan). A small cross is drawn at the pivot while Alt is held. Releasing Alt keeps the pose: the
fly camera goes on from there.

### 3. What the ground is

**The render geometry**: every draw, as the main thread holds them. No authoring needed (Sponza works). Its limits are shown, not
filtered: grass, plants and roofs lift the camera; every level of a render-LOD group has draws (the GPU picks one a frame), so a coarser
level above the finer one wins. If the render-LOD levels prove visibly wrong, the smallest fix is testing only each group's default level,
which the CPU knows.

### 4. How the ground is found

Naive, from what the main thread holds:
1. Loop `RenderWorld::meshInstances`: skip a draw that isn't alive or whose world bounds don't hold the camera's position across the up
   axis.
2. For each one left, take the vertical ray into the mesh's space (the inverse world transform) and test every triangle of its submesh
   (Möller–Trumbore).
3. The ground is the highest hit below the eye.

The meshes come from `SceneChanges`: the main thread keeps each `shared_ptr<const MeshDesc>` by handle as it passes them on, and drops it
when the handle is removed. That and the test are the only new pieces.

### 5. Following

The eye kept `height` above the ground (1.8 m by default; Q/E change the height instead of moving); WASD move across the ground by yaw
only. The height is set to the ground's each frame, no smoothing: a step between two LOD levels' terrain shows as a step. With no ground
under the camera (none drawn there, or off the world), it holds its height and the `status` record says so. `fly` suspends following while
it flies; orbit too while Alt is held.

## Steps

1. **The ground test** (new `src/view/ground.{h,cpp}`):

   ```cpp
   struct GroundHit {
       glm::vec3 point = glm::vec3(0.0f); // world
       uint32_t prim = 0;                 // ViewScene::primPath index of the draw it hit
   };

   // The meshes the draws use, by MeshHandle index, kept from the scene's changes as the main thread passes them to the renderer.
   class GroundMeshes {
   public:
       auto apply(const SceneChanges& changes) -> void;
       auto get(MeshHandle handle) const -> const MeshDesc*;

   private:
       std::vector<std::shared_ptr<const MeshDesc>> meshes;
   };

   // The highest surface below `eye` along -up: the draws' triangles by brute force, the draws filtered by their world bounds.
   auto groundBelow(const RenderWorld& world, const GroundMeshes& meshes, const glm::vec3& eye, const glm::vec3& up) -> std::optional<GroundHit>;
   ```
2. **The camera's modes** (`src/camera.{h,cpp}`):
   - `Camera::orbit(const glm::vec3& pivot, float yawDegrees, float pitchDegrees)` and `Camera::dolly(const glm::vec3& pivot, float steps)`.
   - `Camera::follow` (`std::optional<float>` height above the ground): `update()` moves by yaw only while set, and Q/E change it;
     `view.cpp` sets the eye's height from `groundBelow` after `update()`.
3. **The pivot** (`src/view/viewpicking.{h,cpp}`): a pick can also copy the `gbuffer.depth` texel; `view.cpp` turns it into a world point
   with the inverse view-projection of the frame it was taken in.
4. **Input** (`view.cpp`'s event loop): Left Alt held switches right-drag and the wheel to orbit and dolly, a pick for the pivot sent when
   Alt goes down; G toggles following (shown in the View menu).
5. **RPC, verbs, records** (`src/view/viewcommands.cpp`):
   - `view.camera.orbit {pivot: [x, y, z], yaw, pitch}`: turns a camera (main, or `camera: "debug"`) around a pivot by angles, scriptable.
   - `view.camera.follow {on?, height?}`, returning the state and the ground hit; verbs `camera-follow on|off`, `camera-follow height <m>`.
   - The `status` record's camera gains `follow {on, height, ground: {point, prim} | null}` and `pivot` (while orbiting).
6. **Overlays and UI**: the pivot's cross while orbiting; while following, a line from the eye to the hit and a cross at it (`DebugDraw`).
   The Camera window: a Mode section (follow on, height, the last hit and its prim, the orbit's pivot). The View menu: Follow Ground (G).
   The Engine Map: the "Input and camera" node names the modes and the ground test, and an edge from the scene's draws to it.
7. **READMEs**: `src/view/README.md` (the modes, the ground test), the `run-headless` skill (the verbs).

## Visibility

- **Records**: `status` gains the following state with its ground hit, and the orbit's pivot.
- **RPC and verbs**: `view.camera.orbit`, `view.camera.follow` and `camera-follow …` (new).
- **Windows and overlays**: the Camera window's Mode section; the View menu's Follow Ground; the pivot cross; the ground line and cross; the
  Engine Map's camera node and its edge from the draws.

## Verification

- **Nothing changes without the modes**: the 8 reference images (three_cubes and Sponza: lit, albedo, normals, prepass) byte-identical,
  with validation on.
- **Following**: `camera-follow on`, then the camera stepped 1 m a frame for 60 frames with `camera` across the village (not a flight): in
  every frame's `status`, the eye's height above the ground hit is 1.8 m, the hit's prim a drawn surface (terrain or ground cover). Off the
  world's edge, `ground` is null and the height held. On Sponza, the floor.
- **Orbit**: `view.camera.orbit` by 90° around a pivot keeps the distance to it, to the centimetre, and turns the yaw by 90°. By hand:
  Alt + right-drag turns around the surface under the cursor (handed off as a check).
- **Cost**: the ground test's zone under 0.5 ms a frame at the village with following on; measured, with the triangle and draw counts
  tested in the record.
- No warnings or errors in the trace.

## Results

Built 2026-10-31, release build. Where the build differs from the steps:
- **The test skips chunks first**: looping all 2.4 million draws of the village cost 17 ms a frame, nearly all of it reading every draw's
  bounds. It now skips each chunk of 4,096 draws by the bounds the existing `SceneBoundsCache` keeps for the shadow cascades (an accessor
  added), so it reads 12 to 14 chunks; nothing new holds draws.
- **The ray starts the height above the eye**, so ground that rose by up to the height since the last frame is still under it.
- **A `camera-orbit` verb** beside `view.camera.orbit`, to script the orbit check.
- **The state** (`CameraModesState`) is the Camera window's, beside the debug camera's, as the other windows' states.

Checks:
- **Nothing changes without the modes**: the 8 reference images byte-identical, validation on.
- **Following at the village**: 30 steps of 2 m across it, every one 1.800 m above the hit; the hits on the tiles' `fine` ground chunks
  (`…/T_1_1/full/G_4_0/fine/ground`, then `T_2_1`'s), once on a grass blade (0.3 m higher), as render geometry is. Tested per frame: 12 to
  14 chunks, 2 or 3 draws, about 20,800 triangles, **0.58 ms median, 1.8 ms at most**: over the 0.5 ms aimed at.
- **Following on Sponza**: the floor (`/root/_st_floor/floor_1stfloor_01`), the eye 180 units (1.8 m, the stage in centimetres) above it;
  **6 ms a frame**: 47 to 49 large draws hold the camera, 560,000 triangles.
- **Off the world**: no ground, the height held.
- **Orbit**: 90° of yaw around a pivot keeps the distance (10.44031 before and after) and turns the yaw by 90°; 20° of pitch turns it from
  -10° to 10°, the distance kept. Alt + right-drag and the wheel are by hand (handed off as a check).
- **Seen**: from the debug camera, the main camera following with the line down to its ground hit and the cross there.

## Deferred / follow-ups

- **Ground from collision**, and the Camera window's switch between collision and render geometry: one ray cast against the colliders
  the physics domain makes resident. Trigger: Physics, roadmap step 5 (Jolt in).
- **Following bumps over ground cover** (grass lifts the eye up to its height, 2026-10-31). A filter of draws to ignore (by prim path,
  size or placement) was weighed and left out: a special case where the systematic answer is collision. Ground from collision removes it.
  Trigger: the same, Physics.
- **A faster ground test** (a list of the draws near the camera, then an acceleration structure). Trigger: the zone over 0.5 ms a frame.
- **Walking**: the camera stopped by walls and slopes, gravity. Trigger: a game needing a player that collides (Physics).
