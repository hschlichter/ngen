# Translate Gizmo Plan

## Context
The editor currently has only an orientation gizmo (`Axis3DGizmo`, top-right corner) used to snap the camera to cardinal axes. The only way to change a selected prim's position is by typing into the Properties panel `DragFloat3` for `Transform::position`. We want a direct-manipulation **translate gizmo**: 3 colored arrows pinned to the selected prim's world origin, world-aligned, draggable to update the prim's local position. Edits commit every frame through the existing `SceneEditCommand::SetTransform` pipeline so the change flows through `SceneUpdater` → `USDScene::setTransform` → render extraction, identical to Properties-panel edits.

## Scope (v1)
- 3 axis arrows only (no plane handles, no screen-space center).
- World-aligned frame.
- Live edits every frame during drag.
- Visible only when a prim is selected.
- Constant pixel size (scaled by distance to camera) so it stays usable up close and far away.

## Architecture
Mirrors the existing `Axis3DGizmo` + `GizmoPass` split. Add a new gizmo class; reuse `GizmoVertex`, `GizmoDrawRequest`, and `GizmoPass` unchanged. Unlike `Axis3DGizmo` (which renders into a fixed 120×120 corner viewport with its own viewProj), the translate gizmo renders into the **full window viewport** using the **main camera's viewProj** so it lives in world space at the prim. `GizmoDrawRequest` already supports arbitrary `viewProj` + viewport offsets — no pass changes needed.

## Files to add
- `src/renderer/translategizmo.h`
- `src/renderer/translategizmo.cpp`

## Files to modify
- `src/renderer/renderer.h` / `src/renderer/renderer.cpp` — own a `TranslateGizmo`, expose update/hit/drag entry points, pass selection + transform in.
- `src/main.cpp` — wire mouse down/move/up to the new gizmo before falling through to scene picking; queue `SceneEditCommand::SetTransform` on drag deltas.
- `src/main.cpp` `RenderSnapshot` build site (and `src/renderer/rendersnapshot.h` if needed) — pass current selected prim's world position so renderer can place the gizmo.
- `Makefile` — add new source.

## TranslateGizmo class

```cpp
// src/renderer/translategizmo.h
class TranslateGizmo {
public:
    // Per-frame: build draw vertices for the gizmo at `originWorld`.
    // Returns empty request when no selection.
    auto draw(RhiExtent2D fullExtent, const glm::mat4& view, const glm::mat4& proj,
              const glm::vec3& cameraPos, bool hasSelection,
              const glm::vec3& originWorld) -> GizmoDrawRequest;

    auto updateHover(float mouseX, float mouseY) -> void;

    // Returns true if a drag started (mouse-down consumed).
    auto beginDrag(float mouseX, float mouseY, RhiExtent2D windowExtent) -> bool;

    // Returns world-space delta to apply to the prim's *world* position this frame.
    // Caller converts to local. {} if no drag in progress.
    auto updateDrag(float mouseX, float mouseY, RhiExtent2D windowExtent) -> std::optional<glm::vec3>;

    auto endDrag() -> void;
    auto isDragging() const -> bool { return dragAxis >= 0; }

private:
    auto findClosestAxis(float mouseX, float mouseY) const -> int;
    // Project a screen ray onto the world-space axis line through `origin` along `axisDir`.
    // Returns parametric `t` such that closest point is origin + t*axisDir.
    auto projectMouseOntoAxis(float mouseX, float mouseY, RhiExtent2D windowExtent,
                              const glm::vec3& origin, const glm::vec3& axisDir) const -> float;

    glm::mat4 lastView{1.0f}, lastProj{1.0f}, lastViewProj{1.0f};
    glm::vec3 lastCameraPos{0.0f};
    glm::vec3 lastOrigin{0.0f};
    float lastScale = 1.0f;        // world-space length of arrow at current camera distance
    RhiExtent2D lastExtent{};
    int hoveredAxis = -1;          // -1, 0=X, 1=Y, 2=Z
    int dragAxis = -1;
    float dragStartT = 0.0f;       // axis parameter at drag start
    glm::vec3 dragStartOrigin{0.0f};
};
```

### Geometry (per frame, in `draw`)
- Compute `lastScale = distance(cameraPos, originWorld) * k` (e.g. `k = 0.12`) so arrows are ~constant pixel length. Pick `k` to roughly match the orientation gizmo's visual weight.
- For each axis i ∈ {0,1,2}: emit a line from `originWorld` to `originWorld + axisDirs[i] * lastScale`, color = same `axisColors` palette as `Axis3DGizmo` (X red, Y green, Z blue). When `hoveredAxis == i` or `dragAxis == i`, also emit the same offset-line "thickening" trick used in `axis3dgizmo.cpp:34-48`.
- Arrow tip: 4 short lines from the tip toward `tip - axisDir * tipLen ± perpA*tipLen*0.3 ± perpB*...` to suggest a cone (cheap; we're stuck with `LineList`).
- Return `GizmoDrawRequest{vertices, viewProj=proj*view, vpX=0, vpY=0, vpExtent=fullExtent}`.

### Hover / hit (`findClosestAxis`)
Same screen-space technique as `axis3dgizmo.cpp:67-105`: project axis endpoints with `lastViewProj` and `lastExtent`, screen-space distance to segment, threshold ~10 px. Reuse the math; it works identically once the viewport is the full window.

### Drag math (`projectMouseOntoAxis`)
Translation along a single world axis = closest-point between two skew lines:
1. Build mouse ray exactly as `main.cpp:161-177` (NDC → invVP → near/far → ray).
2. Closest point between mouse ray and axis line: standard solve using `dirCross = cross(rayDir, axisDir)`. Parameter `t` along axis = `dot(cross(originDelta, rayDir), dirCross) / dot(dirCross, dirCross)`.
3. On `beginDrag`: store `dragStartT` and `dragStartOrigin = originWorld`.
4. On `updateDrag`: compute current `t`; world-space delta to apply this frame = `(t - dragStartT) * axisDir` *minus* what we already applied last frame. Simplest: each frame compute new desired world origin = `dragStartOrigin + (t - dragStartT) * axisDir`, return delta from *current* `originWorld` to that. Caller adds delta to local position (world-aligned axes ⇒ delta_world == delta_local for translation).

## Renderer integration

`src/renderer/renderer.h`:
```cpp
TranslateGizmo translateGizmo;

auto translateGizmoBeginDrag(float mx, float my, RhiExtent2D ext) -> bool;
auto translateGizmoUpdateDrag(float mx, float my, RhiExtent2D ext) -> std::optional<glm::vec3>;
auto translateGizmoEndDrag() -> void;
auto translateGizmoIsDragging() const -> bool;
```

`src/renderer/renderer.cpp`:
- In `gizmoUpdate(...)`, also call `translateGizmo.updateHover(...)` and `translateGizmo.draw(...)` when `snapshot.hasSelection`. Append its `GizmoDrawRequest` to the returned vector. Existing `gizmoPass.addPass` already handles N requests.
- In `gizmoHitTest`: first check translate gizmo `findClosestAxis` (only meaningful when selection exists). Order: orientation gizmo (corner) takes priority over translate gizmo, since they don't overlap geometrically anyway. Actually — orientation gizmo lives in a fixed corner, translate gizmo near the prim. Pick orientation first to keep current behavior; otherwise hand off to translate.

`RenderSnapshot` (extend in `src/renderer/rendersnapshot.h`): add `bool hasSelection; glm::vec3 selectionWorldPos;`. Populate in `main.cpp` from `usdScene.getTransform(selectedPrim)->world` (column 3).

## main.cpp wiring

Replace the current `if (...gizmoHitTest...)` block in the `SDL_EVENT_MOUSE_BUTTON_DOWN / LEFT` handler with:

```cpp
if (renderer.gizmoHitTest(mx, my, winExtent)) continue;        // orientation gizmo
if (renderer.translateGizmoBeginDrag(mx, my, winExtent)) continue;
// ...existing scene raycast / selection assignment unchanged...
```

Add new handlers in the SDL event loop:
- `SDL_EVENT_MOUSE_MOTION` (always, not gated on capture): if `renderer.translateGizmoIsDragging()`, call `updateDrag`. If it returns a delta and `selectedPrim` is valid:
  - Read `xf = usdScene.getTransform(selectedPrim)->local`.
  - `xf.position += *delta;`
  - `sceneUpdater.addEdit({.type=SetTransform, .prim=selectedPrim, .transform=xf});` — same path the Properties panel uses (`src/ui/propertieswindow.cpp`).
- `SDL_EVENT_MOUSE_BUTTON_UP` (LEFT): `renderer.translateGizmoEndDrag();`.

While dragging, suppress camera pan / scene re-pick — guard the existing right-button capture and the left-down picking branch with `!renderer.translateGizmoIsDragging()`.

## Reused existing pieces (do NOT reimplement)
- `GizmoVertex` (`src/renderer/gizmo.h:8`)
- `GizmoDrawRequest` (`src/renderer/gizmo.h`)
- `GizmoPass` (`src/renderer/passes/gizmopass.{h,cpp}`) — handles upload, pipeline, viewport switching. Already supports multiple requests per frame.
- `axisColors` palette — copy the constants from `axis3dgizmo.cpp:8-9` (or lift to `gizmo.h`).
- Mouse-ray construction — same formula as `src/main.cpp:161-177`; factor into a helper `pickRay(mx,my,winW,winH,view,proj)` either in `camera.{h,cpp}` or a new tiny header, and call from both the picking site and `TranslateGizmo`.
- `SceneEditCommand::SetTransform` queue (`src/ui/editcommand.h`, `SceneUpdater::addEdit` in `src/scene/sceneupdater.{h,cpp}`) — same path as `propertieswindow.cpp`.
- `USDScene::getTransform(PrimHandle)` — for reading current local + world transform.

## Key files at a glance
| Purpose | Path | Lines |
|---|---|---|
| Gizmo vertex/request types | `src/renderer/gizmo.h` | full |
| Reference impl (orientation) | `src/renderer/axis3dgizmo.{h,cpp}` | full |
| Render backend (reuse as-is) | `src/renderer/passes/gizmopass.{h,cpp}` | full |
| Renderer wiring site | `src/renderer/renderer.cpp` | 265-282, 320-321 |
| Snapshot struct | `src/renderer/rendersnapshot.h` | — |
| Picking ray + selection | `src/main.cpp` | 147-185 |
| Selection state | `src/main.cpp` (`selectedPrim`) | declared near init |
| Edit command | `src/ui/editcommand.h` | 8-22 |
| Edit applier | `src/scene/sceneupdater.{h,cpp}` | 24, 37-55 |
| Existing transform editor (reference) | `src/ui/propertieswindow.cpp` | DragFloat3 block |
| Transform read | `src/scene/usdscene.h` | 117-118, 138 |

## Verification
1. `bear -- make` — builds cleanly; `compile_commands.json` updated.
2. Run the editor, open a USD scene with a translatable prim.
3. Click a mesh — selection works (existing path unchanged).
4. With prim selected, three colored arrows appear at the prim's world origin, sized roughly the same on screen at any camera distance.
5. Hover an axis: it thickens.
6. Drag along X — prim slides along world X under the cursor; release mid-drag and it stays. Repeat for Y, Z.
7. Open Properties panel: position values reflect drag in real time (proves the edit went through `SceneUpdater` and `USDScene::setTransform`).
8. Save the layer (existing flow); reopen — position persists (proves edit landed in USD, not just in cache).
9. Orientation gizmo (corner) still works and takes click priority over translate gizmo when overlapping the corner area.
10. With nothing selected, no translate gizmo is drawn and clicks behave as before.
11. `make format` — no diff in modified files.

## Out of scope (future work)
Plane (XY/YZ/XZ) handles, local-space orientation toggle, snap-to-grid, undo, multi-select drag, rotate/scale gizmos.
