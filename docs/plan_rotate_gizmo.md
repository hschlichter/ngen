# Rotate Gizmo

## Context

The translate gizmo is working. The tools window already has a dummy "Rotate" button bound to `EditorTool::Rotate`. We need the actual rotate gizmo — same
architecture (main-thread class, Preview/Authoring, undo integration), different visual (circles) and math (angle on a plane instead of parameter along a line).

## Visual

Three circles, one per axis, colored X=red, Y=green, Z=blue. Each circle lies in the plane perpendicular to its axis, centered on the gizmo anchor. Drawn as ~32
line segments approximating the ring. Hovered/dragged axis turns yellow. Same `lineWidth = 4.0` via the existing `GizmoPass` pipeline.

Circle points for axis `i` at angle `θ`:
```
perpA = kAxisDirs[(i+1)%3]
perpB = kAxisDirs[(i+2)%3]
point = origin + (perpA * cos(θ) + perpB * sin(θ)) * scale
```

Same constant-pixel-size scale factor as the translate gizmo: `scale = kPixelLength * worldPerPixel` derived from `proj[1][1]` and camera distance.

## Hit testing

Project each circle's sample points to screen. For each axis, compute screen-space distance from cursor to the nearest line segment on that circle (same
`distToSegment` helper as translate). Pick the closest within `kHitThresholdPx`.

## Drag math

On grab:
1. Build mouse ray. Intersect with the plane `normal = kAxisDirs[dragAxis]` through `dragStartAnchor`.
2. Compute start angle: `atan2(dot(hit - anchor, perpB), dot(hit - anchor, perpA))`.

On drag:
1. Same ray-plane intersection → current angle.
2. `deltaAngle = currentAngle - dragStartAngle`.
3. `deltaQuat = glm::angleAxis(deltaAngle, kAxisDirs[dragAxis])` — world-space rotation.
4. Convert to local space: `localDelta = inverse(parentWorldRot) * deltaQuat * parentWorldRot`.
5. `newLocal.rotation = localDelta * dragStartLocal.rotation`. Position and scale unchanged.

Parent world rotation is extracted at `tryGrab` time from `parentWorld = currentWorld * inverse(currentLocal.toMat4())`, with column normalization for scale
safety.

## Class

```cpp
// src/renderer/rotategizmo.h — mirrors TranslateGizmo's API exactly.
class RotateGizmo {
public:
    auto update(RhiExtent2D extent, const glm::mat4& view, const glm::mat4& proj,
                const glm::vec3& cameraPos, float mouseX, float mouseY,
                bool visible, const glm::vec3& originWorld) -> void;
    auto vertices() const -> std::span<const GizmoVertex>;

    auto tryGrab(float mouseX, float mouseY, RhiExtent2D extent,
                 const glm::mat4& view, const glm::mat4& proj,
                 const glm::vec3& gizmoAnchor,
                 const Transform& currentLocal, const glm::mat4& currentWorld) -> bool;
    auto dragUpdate(float mouseX, float mouseY, RhiExtent2D extent,
                    const glm::mat4& view, const glm::mat4& proj) const -> std::optional<Transform>;
    auto release() -> void;
    auto isDragging() const -> bool;
    auto dragStartLocalTransform() const -> const Transform&;
};
```

## Files to add
- `src/renderer/rotategizmo.h`
- `src/renderer/rotategizmo.cpp`

## Files to modify

| File | Change |
|---|---|
| `src/main.cpp` | Own `RotateGizmo`. When `activeTool == Rotate`: call `update`, `tryGrab`, `dragUpdate`, `release`. Copy vertices to snapshot. Same Preview/Authoring + undo inverse hint pattern as translate. |
| `src/renderer/rendersnapshot.h` | Add `std::vector<GizmoVertex> rotateGizmoVerts`. |
| `src/renderer/renderer.cpp` | In `gizmoUpdate`, append `rotateGizmoVerts` draw request (same viewProj, same fullExtent, same as translateGizmoVerts handling). |
| `src/ui/toolswindow.cpp` | Mark Rotate as `implemented = true`. |

## Reused existing pieces
- `GizmoVertex`, `GizmoDrawRequest`, `GizmoPass` (push-constant wide-line pipeline) — unchanged.
- `kAxisDirs`, `kAxisColors`, `kHotColor` — same constants (copy from translategizmo.cpp).
- `mouseRay()` helper — same free function (copy from translategizmo.cpp, or factor to a shared header).
- `SceneEditCommand::inverseTransform` — same undo pattern.
- `SceneQuerySystem::anchorPivot` / `anchorBounds` — same anchor logic.
- `SceneEditRequestContext::Purpose::Preview` / `Authoring` — same pipeline.
- `SceneUpdater::addEdit` — same queue.

## Integration in main.cpp

Identical structure to the existing translate gizmo wiring:

```cpp
// SDL_EVENT_MOUSE_MOTION + rotateGizmo.isDragging(): Preview edit with newLocal.
// SDL_EVENT_MOUSE_BUTTON_UP + isDragging(): Authoring edit with inverseTransform hint.
// SDL_EVENT_MOUSE_BUTTON_DOWN + activeTool==Rotate: tryGrab.
// Per-frame: rotateGizmo.update(); snapshot.rotateGizmoVerts = ...;
```

The input priority: orientation gizmo (corner) → translate/rotate gizmo (whichever is active) → scene raycast. Only one tool-gizmo is active at a time (tools
window ensures mutual exclusion).

## Verification
1. `bear -- make` builds clean.
2. Select a prim, switch to Rotate tool. Three colored circles appear centered on the anchor.
3. Hover a ring: it turns yellow.
4. Drag along the ring: prim rotates around the corresponding world axis. Properties panel rotation values update live.
5. Release: prim stays at the dragged rotation. Properties confirms final values.
6. Ctrl+Z: reverts to pre-drag rotation. Ctrl+Shift+Z: re-applies.
7. Translate tool: rotate gizmo disappears, translate arrows appear.
8. F shortcut, R shortcut, Ctrl+E toggle — all still work.
9. `make format` — no diff.
