# Layer Browser

## Context

Today the Layers window (`src/ui/layerswindow.cpp`) can toggle mute, switch edit target, save, and add a sublayer — but sublayer addition requires the user to **type a path** into a text input (lines 90–97). There's no way to browse the project for `.usd/.usda/.usdc/.usdz` files. A single-click embedded browser that lists usd-family files under the scene's directory removes that friction and becomes a shared widget reused by the prim-creation reference dialog (see `plan_prim_creation.md`).

## Files to create

- **`src/ui/assetbrowser.h` / `.cpp`** — reusable widget that enumerates usd-family files under a given root directory and returns a selected relative path. Public API:
  ```cpp
  struct AssetBrowserState {
      std::string rootDir;
      std::string selected;                 // relative to rootDir; empty = nothing selected
      std::vector<std::string> cachedFiles; // relative paths, sorted
      bool dirty = true;                    // triggers rescan on next draw
  };

  // Returns true if `selected` changed this frame.
  bool drawAssetBrowser(AssetBrowserState& state, float height);
  void invalidateAssetBrowser(AssetBrowserState& state);
  ```
  Implementation uses `std::filesystem::recursive_directory_iterator` (first `std::filesystem` usage in `src/` — fine, standard C++17+). Extension filter `.usd|.usda|.usdc|.usdz`. Sorts alphabetically. Shows relative paths for readability. A small "↻" refresh button invalidates the cache. No file-watcher in v1.

## Files to modify

- **`src/scene/usdscene.h` / `.cpp`** — add public accessor:
  ```cpp
  std::string rootLayerDirectory() const;   // filesystem dir of stage's root layer; empty if closed
  ```
  Implementation reads `m_impl->rootLayer->GetIdentifier()` and returns `std::filesystem::path(...).parent_path().string()`.

- **`src/ui/layerswindow.h`** — introduce `LayersWindowState` (mirrors `PropertiesWindowState` / `SceneWindowState`):
  ```cpp
  struct LayersWindowState {
      AssetBrowserState browser;
  };
  ```
  Pass by `&` into `drawLayersWindow(...)`.

- **`src/ui/layerswindow.cpp`** — after the existing sublayer add-input block (lines 82–97), insert a `CollapsingHeader("Browse Project", ImGuiTreeNodeFlags_DefaultOpen)` section that:
  1. Calls `drawAssetBrowser(state.browser, 200.0f)`.
  2. Below the browser, an **"Add as Sublayer"** button enabled only when `state.browser.selected` is non-empty. Click emits `SceneEditCommand{.type = AddSubLayer, .stringValue = state.browser.selected}` (the existing path already works — USD resolves the relative path via the root layer's asset resolver).

- **`src/ui/editorui.h`** — add `LayersWindowState layersState;` member.
- **`src/ui/editorui.cpp`** — thread `layersState` into the `drawLayersWindow(...)` call at line 67. In `openScene` success branch, set `layersState.browser.rootDir = usdScene.rootLayerDirectory(); layersState.browser.dirty = true;`.
- **`src/main.cpp`** — after the initial load (around `usdScene.open(...)` at the top of `main`), set the same initial `rootDir` so the browser works on first launch.

## Behaviour

- Scene open → browser re-scans on next draw.
- User pushes refresh → `invalidateAssetBrowser` clears cache; next draw re-scans.
- Paths shown relative to `rootDir`; the `AddSubLayer` command receives the relative path verbatim. USD's asset resolver handles composition.

## Verification

1. `make && ./_out/ngen models/Kitchen_set/Kitchen_set.usd`.
2. Open the Layers window → "Browse Project" section lists every `.usd/.usda/.usdc/.usdz` under `models/Kitchen_set/` (Cup.usd, Cup_payload.usd, Cup.geom.usd, hundreds of assets).
3. Pick `assets/Cup/Cup.usd` → "Add as Sublayer" → cup geometry composes into the stage; layer stack in the window gains a new Sublayer row and the root layer flips to dirty.
4. File → Open a different scene → browser resets to the new scene's directory.
5. Press the ↻ refresh button after manually dropping a new `.usda` into the scene folder → the new file appears.

## Non-goals

- File-watcher / live auto-refresh.
- Drag-and-drop reordering of the sublayer stack.
- A configurable "project root" separate from the scene directory.
