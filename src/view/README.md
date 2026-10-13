# View

ngen-view's library: the scene it shows (`viewscene.*`), its commands as RPC methods and script verbs (`viewcommands.*`), and its dumps
(`viewdumps.*`). The program's `main` is `src/apps/view.cpp`; its UI is `ViewUI` (`src/ui/viewui.*`).

## The scene: `ViewScene`

The view shows a scene composed from its packs, with nothing of USD in it (`src/world/README.md`). `ViewScene::open(id)` composes the
scene's World from its layer packs, fetched from the asset server round by round, then fetches the meshes and materials of what is drawn
and the textures those materials name, and builds the renderer's inputs: `RenderWorld` (a draw instance per submesh of every drawn mesh,
and the distant lights), `MeshLibrary` and `MaterialLibrary`. It blocks until all of it has arrived; a pack that fails is a gap, with a
warning.

- **What is drawn**: entities that are active, defined and not abstract, visible (an invisible ancestor hides its subtree), not in a LOD
  level their root doesn't draw, with a mesh asset, and of a purpose that is shown: `default` and `render` always, `guide` and `proxy`
  when turned on (`purposes` verb, `view.purposes`, the View menu).
- **Materials**: a mesh's bound material, and each `GeomSubset`'s own binding for its submesh, unless an ancestor's binding is
  `strongerThanDescendants`. A material's texture, else its base colour, else the mesh's display colour; a constant colour is a 1x1
  texture. A mesh with no material bound gets one per display colour, shared.
- **Transforms**: in float, parent times local, then scaled to metres by the root layer's `metersPerUnit`, as the view always drew; the
  camera's up is the scene's up axis.
- **The engine's light**: when the scene has no distant light, one is added, upper-front-right in the scene's frame (intensity 1, colour
  (1, 0.95, 0.88), angle 0.53).
- **Changing what is drawn**: `setLodLevel` (the `lod` verb, `view.lod.set`) and `setPurposeShown` compose and fetch what the change
  needs and rebuild the inputs; `revision()` moves on, and the main loop uploads them.
- **Names**: draw instances and lights carry a prim index; `primPath` turns it into the entity's full path, for the render debugger, the
  frame debugger, the GPU scene window and the dumps.

The trace shows the steps: `SceneRequested`, `PacksComposed` (packs, rounds, prims, prototypes, time), `SceneAssetsReceived` (meshes,
materials, textures, time), `WorldExtracted` (instances, meshes, materials, lights, time), and `SceneAssetFailed` or `CompositionMessage`
warnings.

Nothing is selected or edited in the view yet: there is no picking, no gizmo editing and no selection.
