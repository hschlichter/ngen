# Frame Graph Node View

## Context

The list-based frame graph debug window (`src/ui/framegraphwindow.cpp`) shows passes and resources as tables. It works for inspecting individual nodes but doesn't convey the *shape* of the DAG — branching, width, bottlenecks — at a glance.

Goal: add a second tab to the same window that renders the frame graph as a node diagram with passes and resources spatially laid out, edges drawn, and resource previews embedded directly in the resource nodes.

## Design decisions

| Question | Decision |
|---|---|
| Library vs custom | Custom `ImDrawList`. No new submodule. |
| Graph shape | Bipartite — passes and resources both as nodes. Lets resource thumbnails live in the graph. |
| Layout | Layered by `executionIndex` + one barycenter sweep for y. No iterative optimization. |
| Integration | `BeginTabBar` with "List" and "Graph" tabs sharing the existing bottom detail pane. |
| Pan/zoom | Pan only (middle-drag or empty-canvas left-drag). No zoom in v1. |
| Interaction | Read-only. No node dragging, no persisted layout. |

**Why custom over imnodes**: existing code already uses `ImDrawList` directly (`accessChip`, embedded `ImGui::Image`). imnodes is opinionated toward authoring (drag, zoom, link creation) we don't want. ~400 lines of focused rendering < submodule + fighting the interaction model.

**Why bipartite**: resources have `previewTextureId` and thumbnails are the feature. If resources were edge labels, thumbnails would have nowhere to live. Bipartite also matches the list view's existing pattern of treating resources as first-class selectable nodes.

## Files

**New:**
- `src/ui/framegraphnodeview.h` — one public function.
- `src/ui/framegraphnodeview.cpp` — layout + rendering.

**Modified:**
- `src/ui/framegraphwindow.cpp` — wrap the current split table in a `BeginTabBar`, factor the bottom detail pane into a local lambda reused by both tabs.

No Makefile changes (`find src -name '*.cpp'` picks up new files).

## Public API

```cpp
// framegraphnodeview.h
void drawFrameGraphNodeView(const FrameGraphDebugSnapshot& snap,
                            std::optional<uint32_t>& selPass,
                            std::optional<uint32_t>& selResource);
```

Reuses `fgSelectedPass` / `fgSelectedResource` on `EditorUI` so clicking a node in Graph updates the same detail pane driven by List.

## Internals (all in anonymous namespace)

```cpp
enum class NodeKind { Pass, Resource };

struct LaidOutNode {
    NodeKind kind;
    uint32_t index;     // into snap.passes or snap.resources
    ImVec2 pos;         // canvas-space top-left
    ImVec2 size;
};

struct Layout {
    std::vector<LaidOutNode> nodes;
    std::unordered_map<uint64_t, uint32_t> lookup; // encode(kind,index) -> nodes idx
};

auto buildLayout(const FrameGraphDebugSnapshot&) -> Layout;
void drawPassNode(ImDrawList*, const FgPassDebug&, ImVec2 pos, ImVec2 size, bool selected);
void drawResourceNode(ImDrawList*, const FgResourceDebug&, ImVec2 pos, ImVec2 size, bool selected);
void drawEdge(ImDrawList*, ImVec2 from, ImVec2 to, FgAccessFlags, bool highlighted);
auto hitTest(const Layout&, ImVec2 mouseCanvasSpace) -> std::optional<uint32_t>;
```

## Layout math

Two columns per execution step so resources sit visually between producer and consumers:

```
passColumn[p]     = 2 * executionIndex[p]
resourceColumn[r] = 2 * executionIndex[producerPass(r)] + 1
                    // external-with-no-producer: 2 * firstUseOrder - 1 (clamp 0)
                    // unused: rightmost "dead" column
x = kMarginX + column * kColumnStride                // kColumnStride ~ 220
```

Culled passes: keep their `executionIndex` if present; otherwise park in a trailing column.

**Rows (y)**: initialize to `slotInColumn * kRowStride`. One left-to-right sweep over columns starting at 1, set each node's y to the mean of its predecessors' y (resource → its producer; pass → its read resources). Then one right-to-left sweep using successors. Finally, per column, sort by y and re-stack with `y = max(y, prev.y + prev.h + kRowGap)` to prevent overlap. ~40 lines, O(V+E).

```
kNodeWidthPass     = 140   kNodeHeightPass    = 56
kNodeWidthResource = 160   kNodeHeightResource = 96
kColumnStride      = 220   kRowStride         = 120   kRowGap = 24
```

## Rendering

- Host: `BeginChild("##fg_graph", ImVec2(0,0), Borders, NoScrollbar | NoMove)`. Canvas origin = `GetCursorScreenPos() + scroll`.
- Pan: `scroll += GetIO().MouseDelta` while hovered AND middle-dragging (or left-drag when no node was hit this frame).
- Draw order: edges first, nodes over edges, selected node border stroked last on top.
- **Pass node**: rounded filled rect; name + `std::format("exec #{}", executionIndex)`; yellow border if `hasSideEffects`, dimmed alpha if `culled`, accent border if selected.
- **Resource node**: rounded filled rect; 64×(64/aspect) `ImGui::Image((ImTextureID) previewTextureId, ...)` when present (same aspect math as `drawResourceDetail`); truncated label below; blue border if `external`, accent if selected.
- **Edges**: `ImDrawList::AddBezierCubic` with horizontal control-point offsets = 0.4 × dx; color from existing `accessColor` palette (lift from `framegraphwindow.cpp` — either expose in the header or duplicate the 15-line helper); thicker + saturated when an endpoint is selected.

## Hit testing

AABB vs mouse in canvas space. Iterate `layout.nodes`, pick topmost hit. Left-click sets `selPass` (for Pass) or `selResource` (for Resource) — no mutual clearing; list-view already treats them as independent.

## ID stability

Previous bug (see memory): `PushID(&element)` used addresses of vector elements that move each frame when the snapshot is replaced. Use data-derived IDs only: `(passIdx * 2 + 0)` and `(resIdx * 2 + 1)`. Most ImDrawList drawing doesn't need IDs anyway.

## Explicitly avoid

- Draggable nodes, persisted positions, layout cache by frameIndex.
- Any graph-layout library (Graphviz, OGDF, Sugiyama with crossing minimization).
- Pin/port abstraction — edges connect node centers or edge midpoints.
- Zoom in v1.
- `imnodes` or any new external submodule.
- Address-keyed `PushID`.

## Implementation order

1. Stub `framegraphnodeview.{h,cpp}` returning placeholder text; wire `BeginTabBar` with "List" and "Graph" in `framegraphwindow.cpp`. Factor the bottom detail pane into a lambda shared by both tabs. Confirm tab switching works.
2. `buildLayout` — columns + naive stacked y. Draw nodes as plain labeled rects. Confirm x positions match execution order.
3. Edges: bezier curves colored by access flags.
4. Resource thumbnails inside resource nodes; color coding for side-effects / culled / external / selected.
5. Hit testing + click-to-select; confirm bottom detail pane updates.
6. Panning.
7. Barycenter y sweep + overlap re-stack.

## Verification

1. `make` builds.
2. Open **Debug → Frame Graph**, switch to the Graph tab.
3. Graph matches the list: `GeometryPass` on the left, `gbuffer.albedo` / `gbuffer.normal` to its right, `LightingPass` reading both, `backbuffer` flowing through `DebugRenderer` / `GizmoPass` / `EditorUIPass`, `depth` connected to GeometryPass/LightingPass/DebugRenderer.
4. Resource nodes with blittable formats show live thumbnails identical to the list tab.
5. Clicking a node highlights it and populates the shared bottom detail pane.
6. Middle-drag pans the canvas; no drift when snapshots tick.
7. Closing + reopening the window restores selection (state lives on `EditorUI`).
