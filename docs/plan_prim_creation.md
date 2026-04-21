# Prim Creation

## Context

`USDScene::createPrim(parentPath, name, typeName, ctx)` exists (`src/scene/usdscene.cpp:1211-1230`) but is **unused**. There's no UI surface, no `SceneEditCommand::CreatePrim` / `RemovePrim`, no way to author USD reference arcs, and the Scene tree's right-click menu stops at Copy / Copy Full Path (`src/ui/scenewindow.cpp:88-95`). Adding a point light to a scene means hand-editing USD.

**Goal.** Right-click a Scene tree node → **Add Child** → typed prims (Xform, lights, geometry) *or* **Reference…** (reuses the asset browser from `plan_layer_browser.md` to pick an asset). Plus **Delete**. All changes route through the existing `SceneEditCommand` → `SceneUpdater` → `UndoStack` pipeline so undo/redo works for free.

Split into two phases so Phase A (infrastructure + lights + Xform + Reference) can ship independently of Phase B (procedural geometry rendering). After Phase A alone, creating a Cube/Sphere/Cylinder/Cone will author a valid USD prim but it won't render until Phase B lands.

---

## Phase A — Infrastructure + creation UI

### New `SceneEditCommand` types

Edit `src/ui/editcommand.h`:
```cpp
enum class Type : uint8_t {
    MuteLayer,
    SetTransform,
    SetVisibility,
    AddSubLayer,
    ClearSession,
    CreatePrim,             // NEW: typed-prim creation
    CreateReferencePrim,    // NEW: prim + references arc in one atomic edit (for undo)
    RemovePrim,             // NEW: delete prim
};

struct SceneEditCommand {
    Type type;
    PrimHandle prim;
    Transform transform;
    Transform inverseTransform;
    bool boolValue;
    LayerHandle layer;
    std::string stringValue;        // existing: sublayer path
    // NEW fields (only populated for Create*/RemovePrim):
    std::string parentPath;         // absolute USD path of the parent
    std::string primName;           // child name, valid identifier
    std::string typeName;           // e.g. "Xform", "SphereLight", "Cube"; empty = untyped
    std::string referenceAsset;     // for CreateReferencePrim: relative asset path
    SceneEditRequestContext::Purpose purpose;
};
```

### New `USDScene` API

In `src/scene/usdscene.h` / `.cpp`:
```cpp
PrimHandle createPrim(const char* parentPath, const char* name, const char* typeName,
                      const SceneEditRequestContext& ctx = {});            // already exists
PrimHandle createReferencePrim(const char* parentPath, const char* name,
                               const char* referenceAsset,
                               const SceneEditRequestContext& ctx = {});   // NEW
bool       removePrim(PrimHandle h, const SceneEditRequestContext& ctx = {});  // already exists
```
`createReferencePrim` inside a `UsdEditContext(stage, chooseEditLayer(ctx))`:
1. `stage->DefinePrim(path, TfToken("Xform"))` (empty-type also fine — the referenced layer supplies its own typeName on composition).
2. `prim.GetReferences().AddReference(referenceAsset)`.
3. Return handle (same pattern as existing `createPrim` at `usdscene.cpp:1224-1229`).

Unique-name helper (to avoid collisions like two `SphereLight_1` under the same parent):
```cpp
std::string uniqueChildName(const char* parentPath, const char* baseName) const;
```
Walks the children under `parentPath`, appends `_1`, `_2`, … until no collision.

### `SceneUpdater` wiring

Extend the async-batch `switch` at `src/scene/sceneupdater.cpp:87-104`:
```cpp
case SceneEditCommand::Type::CreatePrim:
    usdScene.createPrim(cmd.parentPath.c_str(), cmd.primName.c_str(),
                        cmd.typeName.c_str(), {.purpose = cmd.purpose});
    break;
case SceneEditCommand::Type::CreateReferencePrim:
    usdScene.createReferencePrim(cmd.parentPath.c_str(), cmd.primName.c_str(),
                                 cmd.referenceAsset.c_str(), {.purpose = cmd.purpose});
    break;
case SceneEditCommand::Type::RemovePrim:
    usdScene.removePrim(cmd.prim, {.purpose = cmd.purpose});
    break;
```
These are structural changes, not transform-only, so they must go through the existing async path (they'll produce a `primsResynced` dirty set → full re-extract). Not eligible for the fast path at `sceneupdater.cpp:42`.

### Undo integration

Extend `src/scene/undostack.cpp::recordBatch`:
- `CreatePrim` / `CreateReferencePrim` → inverse is `RemovePrim` against the path `parentPath + "/" + primName`. Path is known at record-time, so this is straightforward.
- `RemovePrim` → redo requires reconstructing the removed subtree. v1 approach: at record-time, `SdfCopySpec` the removed prim into a temporary `SdfLayer::CreateAnonymous()` and keep the anonymous layer pinned in the undo entry. On undo, flatten that anonymous layer into the current edit target at the original path.
  - If the anonymous-layer snapshot is too much for v1, skip pushing an inverse for `RemovePrim` — Delete won't be undoable, and we can note this in a follow-up. **Recommended:** ship the anonymous-layer snapshot since `SdfCopySpec` is a one-liner per prim.

### Scene-tree right-click menu

Modify `src/ui/scenewindow.cpp`'s existing context-menu block (lines 88-95):
```cpp
if (ImGui::BeginPopupContextItem()) {
    // ... existing Copy / Copy Full Path ...
    ImGui::Separator();
    if (ImGui::BeginMenu("Add Child")) {
        addTypedItem("Xform",          "Xform");
        ImGui::Separator();
        ImGui::TextDisabled("Lights");
        addTypedItem("Distant Light",  "DistantLight");
        addTypedItem("Sphere Light",   "SphereLight");
        addTypedItem("Rect Light",     "RectLight");
        addTypedItem("Disk Light",     "DiskLight");
        addTypedItem("Cylinder Light", "CylinderLight");
        addTypedItem("Dome Light",     "DomeLight");
        ImGui::Separator();
        ImGui::TextDisabled("Geometry");
        addTypedItem("Cube",     "Cube");
        addTypedItem("Sphere",   "Sphere");
        addTypedItem("Cylinder", "Cylinder");
        addTypedItem("Cone",     "Cone");
        ImGui::Separator();
        if (ImGui::MenuItem("Reference...")) {
            sceneState.openReferenceDialog = true;
            sceneState.referenceParent = prim.path;
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Delete", "Del")) {
        pendingEdits.push_back({.type = SceneEditCommand::Type::RemovePrim, .prim = prim.handle});
    }
    ImGui::EndPopup();
}
```
Where `addTypedItem(label, type)` is a lambda that pushes a `CreatePrim` edit with an auto-generated unique child name (`usdScene.uniqueChildName(parentPath, typeBaseName)`).

### Reference dialog (reuses the asset browser)

In `src/ui/scenewindow.cpp`, add an `ImGui::BeginPopupModal("Add Reference")` triggered by `sceneState.openReferenceDialog`. Body:
1. `drawAssetBrowser(sceneState.referenceBrowser, 250.0f)` (reuses the widget from `plan_layer_browser.md`).
2. "Create" / "Cancel" buttons. Create is enabled when a file is selected.
3. On Create: push `CreateReferencePrim{parentPath = sceneState.referenceParent, primName = stem-of-selected-file, referenceAsset = selected}`.

`SceneWindowState` gains `bool openReferenceDialog; std::string referenceParent; AssetBrowserState referenceBrowser;`. The browser's `rootDir` is refreshed from `usdScene.rootLayerDirectory()` each time the modal opens.

### Critical files (Phase A)

- **`src/ui/editcommand.h`** — new enum values + fields on `SceneEditCommand`.
- **`src/scene/usdscene.{h,cpp}`** — add `createReferencePrim`, `uniqueChildName`.
- **`src/scene/sceneupdater.cpp`** — switch cases for `CreatePrim` / `CreateReferencePrim` / `RemovePrim`.
- **`src/scene/undostack.cpp`** — inverse generation for create/remove.
- **`src/ui/scenewindow.{h,cpp}`** — context-menu submenus + reference modal.
- **`src/ui/editorui.cpp`** — pass the shared asset-browser state through where needed.

### Verification (Phase A)

1. Right-click root prim in Scene → **Add Child → Sphere Light** → new `SphereLight_1` appears under it; Properties panel shows the new prim's attributes.
2. Right-click same prim → **Add Child → Reference…** → modal with the asset browser; pick `models/Kitchen_set/assets/Cup/Cup.usd` → the cup composes in as a child with full geometry.
3. Right-click the created prim → **Delete** → removed from tree and render.
4. `Ctrl+Z` undoes each create; `Ctrl+Shift+Z` redoes. Delete also round-trips.
5. Layers window shows the edit target layer as dirty after each create/delete.

---

## Phase B — Procedural geometry rendering

The extractor at `src/scene/usdrenderextractor.cpp:6` only handles `UsdGeomMesh`. For `UsdGeomCube/Sphere/Cylinder/Cone` to render after Phase A creates them, the extractor must tessellate them.

### Approach

- **`src/scene/primshapemesh.h` / `.cpp`** (new) — tessellation helpers returning `MeshData { vertices, indices }`:
  - `tessellateCube(double size)` — 24 verts / 36 indices (per-face flat-shaded normals).
  - `tessellateSphere(double radius)` — lat/lon parameterized, default (16 lat, 32 lon).
  - `tessellateCylinder(double radius, double height, TfToken axis)` — end caps + side band.
  - `tessellateCone(double radius, double height, TfToken axis)` — base cap + side.

- **`src/scene/usdscene.cpp::classifyPrim`** — fire `PrimFlagRenderable` for the USD shape schemas too:
  ```cpp
  if (prim.IsA<UsdGeomMesh>() || prim.IsA<UsdGeomCube>() || prim.IsA<UsdGeomSphere>()
      || prim.IsA<UsdGeomCylinder>() || prim.IsA<UsdGeomCone>()) {
      flags |= PrimFlagRenderable;
  }
  ```

- **`src/scene/usdrenderextractor.cpp`** — in the renderable branch, when the prim isn't a Mesh, read the schema attributes (`size`, `radius`, `height`, `axis`) and ask the `MeshLibrary` for a cached tessellation keyed by `(type, params)`. Falls through to the existing UsdGeomMesh path for everything else. One cache entry per distinct parameter combination so hundreds of identically-sized cubes share a vertex buffer.

### Critical files (Phase B)

- **`src/scene/primshapemesh.{h,cpp}`** (new).
- **`src/scene/usdscene.{h,cpp}`** — extend `classifyPrim` to mark shape schemas renderable.
- **`src/scene/usdrenderextractor.cpp`** — tessellate shape schemas in the extraction loop; cache keys.

### Verification (Phase B)

1. Right-click any Xform → **Add Child → Cube** → a unit cube renders at the parent's origin.
2. Same for Sphere / Cylinder / Cone.
3. Selecting the new prim in the Scene tree anchors the gizmos on the tessellated bounds (the existing anchor walker in `sceneQuery` should pick it up automatically once `PrimFlagRenderable` fires).
4. Re-running with `./_out/ngen` on a scene that already contains tessellated shapes (e.g. saved then reopened) renders them identically.

---

## Non-goals

- Drag-and-drop reparenting in the Scene tree.
- Per-variant / per-material editing on the new prim — the create is bare; the user uses the existing Properties panel afterward.
- Multi-select delete in v1 — single prim only.
- Live file-watcher on the asset browser (owned by `plan_layer_browser.md`; v1 is manual refresh).
