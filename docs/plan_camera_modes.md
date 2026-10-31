# Camera modes: orbit and ground following

**Status. Draft.** Decisions as leaned, pushback welcome. Builds on [plan_debug_camera.md](plan_debug_camera.md): the modes act on the camera
that has the controls (the main camera, or the debug camera while it is on).

## Current state

The view's camera is a fly camera only (`src/camera.cpp`): WASD and Q/E move it, right-drag looks around in place, the wheel scales the
speed (`src/apps/view.cpp`'s event loop). Picking copies one texel of `gbuffer.instance` under the cursor through the capture service and
hands back the instance a few frames later (`ViewPicking`); it carries no depth.

Nothing in the engine reads collision: the packer keeps no `physics:*` attribute (`isEngineAttribute`, `usdpacker.cpp`), and the composer
marks a `physics` LOD heuristic unusable. The streaming test world authors it: a tile's 1 m terrain level is its collision level, with
`physics:collisionEnabled` on, as are the default levels of rocks and structures (`assets/streamtest-world/docs/LOD_STREAMING.html`,
"Terrain" and "Physics").

What the main thread already holds, and the ground test reads: `RenderWorld::meshInstances` (every draw's world transform, world bounds,
mesh handle and prim), and every mesh's `MeshDesc` (vertices and indices), which the scene thread's mesh library makes and the main thread
passes to the renderer in `SceneChanges::meshesAdded`.

## Scope

**In**
- Orbit while Left Alt is held, around the surface under the cursor.
- Ground following: the camera kept at a height above the ground under it, the ground taken from the collision geometry or from the
  render geometry, switched in the Camera window.
- `physics:collisionEnabled` carried from the packer to each draw.
- RPC, verbs, the `status` record, overlays, the Camera window, the Engine Map.

**Out**
- Any acceleration structure (BVH, grid, index): the ground test loops the draws and tests triangles by brute force. Added only if it
  measures slow, and then the smallest thing first (a list of the collision slots, rebuilt when the draws change).
- Physics: collision is read only for the camera's ray; nothing collides, the camera passes through walls.
- The physics LOD heuristic (which level is the collision level): the authored `physics:collisionEnabled` says it, as the test world
  authors it on the default level only.

## Decisions

### 1. What orbit turns around

- **A. The surface under the cursor when Alt goes down**: picking also copies the depth texel, turned into a world point. Every scene, a
  few frames late; until it arrives the camera turns around the point at the last pivot's distance along the cursor ray.
- **B. The selection's centre**, or a point at a fixed distance with nothing selected.

**A**, with B's fixed distance over the sky (depth 0). A ray against the collision geometry (decision 3) would answer at once, but scenes
without collision (Sponza) would have nothing to orbit.

### 2. Orbit's controls

While Left Alt is held: right-drag turns around the pivot (yaw around the world's up through the pivot, pitch around the camera's right,
the distance kept); the wheel moves toward and away from the pivot (each step a fraction of the distance, never through it); WASD and Q/E
fly as before and carry the pivot along (a pan). A small cross is drawn at the pivot while Alt is held. Releasing Alt keeps the pose: the
fly camera goes on from there.

### 3. What the ground is

- **A. Collision geometry**: draws under an authored `physics:collisionEnabled = true`. On the test world the terrain's collision level,
  and rocks and structures, which the camera then rides over.
- **B. Render geometry**: every draw. No authoring needed (Sponza works), but grass, plants and roofs lift the camera; and every level of
  a render-LOD group is placed (the GPU picks one a frame), so a coarser level above the finer one wins.

**Both, switched** (Camera window, `from collision|render`), **A by default**. Collision is the authored answer and the start of the
queries a game makes; render geometry is the one every scene has. Render geometry's limits are shown, not filtered: if the render-LOD
levels prove visibly wrong, the smallest fix is testing only each group's default level, which the CPU knows.

### 4. How the ground is found

Naive, from what the main thread holds:
1. Loop `RenderWorld::meshInstances`: skip a draw that isn't alive, isn't collision (with `from collision`), or whose world bounds don't
   hold the camera's position across the up axis.
2. For each one left (one or two), take the vertical ray into the mesh's space (the inverse world transform) and test every triangle of
   its submesh (Möller–Trumbore).
3. The ground is the highest hit below the eye.

The meshes come from `SceneChanges`: the main thread keeps each `shared_ptr<const MeshDesc>` by handle as it passes them on, and drops it
when the handle is removed.

### 5. Following

The eye kept `height` above the ground (1.8 m by default; Q/E change the height instead of moving); WASD move across the ground by yaw
only. The height is set to the ground's each frame, no smoothing: a step between two LOD levels' terrain shows as a step. With no ground
under the camera (none resident, or off the world), it holds its height and the `status` record says so. `fly` suspends following while
it flies; orbit too while Alt is held.

## Steps

1. **Collision on each draw**:
   - `src/asset/pack/usd/usdpacker.cpp`: `isEngineAttribute` keeps `physics:collisionEnabled`; `src/asset/README.md` lists it.
   - `src/world/world.h` / `worldcomposer.cpp`: `Entity::collision`, `std::optional<bool>`, inherited from the nearest ancestor that authors
     it, as `purpose` is (the test world authors it on the level prim, not on each mesh).
   - `src/renderer/renderworld.h`: `bool collision = false;` on `RenderMeshInstance`, set in `placement.cpp` where the draw is built. The
     GPU instance record doesn't carry it.
2. **The ground test** (new `src/view/ground.{h,cpp}`):

   ```cpp
   enum class GroundSource : uint8_t {
       Collision,
       Render,
   };

   struct GroundHit {
       float height = 0.0f; // along the up axis, world units
       uint32_t prim = 0;   // ViewScene::primPath index of the draw it hit
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
   auto groundBelow(const RenderWorld& world, const GroundMeshes& meshes, const glm::vec3& eye, const glm::vec3& up, GroundSource source)
       -> std::optional<GroundHit>;
   ```
3. **The camera's modes** (`src/camera.{h,cpp}`):
   - `Camera::orbit(const glm::vec3& pivot, float yawDegrees, float pitchDegrees)` and `Camera::dolly(const glm::vec3& pivot, float steps)`.
   - `Camera::follow` (`std::optional<float>` height): `update()` moves by yaw only while set, and Q/E change it; `view.cpp` sets the eye's
     height from `groundBelow` after `update()`.
4. **The pivot** (`src/view/viewpicking.{h,cpp}`): a pick can also copy the `gbuffer.depth` texel; `view.cpp` turns it into a world point
   with the inverse view-projection of the frame it was taken in.
5. **Input** (`view.cpp`'s event loop): Left Alt held switches right-drag and the wheel to orbit and dolly, a pick for the pivot sent when
   Alt goes down; a shortcut toggles following (one not bound yet, shown in the menu).
6. **RPC, verbs, records** (`src/view/viewcommands.cpp`):
   - `view.camera.orbit {pivot: [x, y, z], yaw, pitch, distance?}`: turns the controlled camera around a pivot by angles, scriptable.
   - `view.camera.follow {on?, height?, from?: "collision" | "render"}`, returning the state and the ground hit; verbs
     `camera-follow on|off`, `camera-follow height <m>`, `camera-follow from collision|render`.
   - The `status` record's camera gains `mode` (`fly`, `orbit`, `follow`), `pivot`, and `follow {height, from, ground: {height, prim} |
     null}`.
7. **Overlays and UI**: the pivot's cross while orbiting; while following, a short line from the eye to the hit and a cross at it
   (`DebugDraw`, from `ViewUI::drawDebug`). The Camera window: a Mode section (follow on, height, ground from collision / render, the last
   hit and its prim, the orbit's pivot). The Engine Map: the "Input and camera" node names the modes, and an edge from Placement (the draws
   and meshes) to it for the ground test.
8. **READMEs**: `src/view/README.md` (the modes, the ground test and its sources), `src/world/README.md` (`collision` on an entity),
   `src/asset/README.md` (the kept attribute), the `run-headless` skill (the verbs).

## Visibility

- **Records**: `status` gains the camera's mode, pivot and following state with its ground hit.
- **RPC and verbs**: `view.camera.orbit`, `view.camera.follow` and `camera-follow …` (new).
- **Windows and overlays**: the Camera window's Mode section; the pivot cross; the ground line and cross; the Engine Map's camera node and
  its edge from Placement.

## Verification

- **Nothing changes without the modes**: the 8 reference images (three_cubes and Sponza: lit, albedo, normals, prepass) byte-identical,
  with validation on.
- **Collision reaches the draws**: at `/cameras/village`, the draws under tile terrain's collision level and rocks' default levels have
  `collision`; a count in the `status` record's follow state (draws tested, collision draws) is non-zero, and zero on Sponza.
- **Following**: `camera-follow on`, then the camera stepped 1 m a frame for 60 frames with `camera` across the village (not a flight): in
  every frame's `status`, the eye's height minus the ground hit is 1.8 m, the hit's prim is under a tile's terrain; `from render` gives
  hits at least as high, some on ground cover. Over the sky's edge of the world, `ground` is null and the height held.
- **Orbit**: `view.camera.orbit` by 90° around a pivot keeps the distance to it, to the centimetre, and turns the yaw by 90°. By hand:
  Alt + right-drag turns around the surface under the cursor (handed off as a check).
- **Cost**: the ground test's zone under 0.5 ms a frame at the village with following on, `from collision` and `from render`; if not,
  the list of collision slots (Out) is the next step, measured again.
- No warnings or errors in the trace.

## Open questions

- Where the test world authors `physics:collisionEnabled` (on the level prim or on each mesh) decides whether `collision` must inherit.
  Step 1 inherits, as `purpose` does; checked against the asset when built.

## Deferred / follow-ups

- **An acceleration structure** for the ground test. Trigger: the zone over 0.5 ms a frame after the collision slot list.
- **Walking**: the camera stopped by walls and slopes, gravity. Trigger: a game needing a player that collides.
- **Orbit around the ground** (a ray through the cursor against collision geometry, no readback). Trigger: the readback's latency felt in
  use.
