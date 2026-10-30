# Streaming tool: a window, overlays and configuration for residency and budgets

**Status. Draft.** Decisions as leaned, pushback welcome. Second of three plans: [plan_streaming_domain.md](plan_streaming_domain.md) (what
is resident), this one (seeing and steering it), [plan_streaming_budgets.md](plan_streaming_budgets.md) (what fits; its controls land here).

## Current state

What streaming does is shown in pieces: the LOD window lists streaming roots beside render-LOD groups, with wanted, drawn and loaded per row
([plan_lod_tool.md](plan_lod_tool.md)); Scene Flow has a line for kept levels; the `streaming` record has the numbers. After
[plan_streaming_domain.md](plan_streaming_domain.md) the record carries residency per root (selected, state, resident, drawn, late,
authoring findings, per-level lead) and totals (late switches, holes, reloads, lead), and [plan_streaming_budgets.md](plan_streaming_budgets.md)
adds budgets, granted states and priorities. None of it is visible as a whole: where around the camera things are resident, requested or
late, how close a budget is, why a root draws what it draws, and nothing in the UI sets the radii's tuning for streaming or the budgets.

The LOD window stays the tool for levels and heuristics of both LOD kinds; this window is for residency: what is loaded, what is on its way,
what it costs, and what doesn't hold.

## Scope

**In**
- A Streaming window: overview, roots, inspector, configuration, measurement.
- Overlays in the viewport: a top-down map, root bounds coloured by state, the streaming heuristic's rings, labels, and a `streaming` view
  mode colouring draws.
- Every control reachable by RPC and verb as well; the window uses the same requests.
- The Engine Map: a Residency node.

**Out**
- What residency and budgets do: the other two plans. This plan adds no rule, only shows and sets.
- Editing the authored radii in USD: the tuning is view-local, as the LOD tool's ([plan_lod_tool.md](plan_lod_tool.md)); writing it back is
  the editor's.

## Decisions

### 1. One window or the LOD window extended

- **A. A Streaming window of its own**, cross-linked with the LOD window (a selected root is selected in both; a streaming row in the LOD
  window opens it here).
- **B. A Streaming tab in the LOD window.**

I lean **A**: the LOD window is about levels and heuristics for both kinds, render LOD mostly; residency and budgets are a different
question, asked while flying, with the map open beside the viewport.

### 2. The map

- **A. A top-down map in the window**: the placed streaming roots' bounds from above, coloured by state, the camera and its view direction,
  each root's load and unload rings for its next boundary on hover; pan and zoom, click selects, double-click frames. Drawn with ImGui's draw
  list from the record's data.
- **B. Only in the viewport** (bounds and rings drawn in the world).

I lean **A and the viewport overlays both**: the map shows the whole ring around the camera at once, which the viewport can't from eye
height; the viewport shows it in place.

### 3. Colours

One legend for the map, the bounds overlay and the `streaming` view, by what holds for a root, worst first: hole (drawing nothing), late
(drawing coarser than selected), refused by a budget (granted coarser than wanted), requested (a level on its way), prefetched (a finer level
resident, not drawn yet), drawn as selected. Authoring findings outline a root in a colour of their own. I lean on these six; they are the
states the records already name.

## Steps

1. **The window** (`src/ui/streamingwindow.*`, `ViewUI`), fed per snapshot from the `streaming` record's data (`buildStreamingRows`, as the
   LOD tool's rows):
   - *Overview*: per budget a bar (use, limit, peak; over in red); requests in flight and queued; counters: roots late and holes now, late
     switches, root-seconds coarser, loads and reloads, lead min and median, arrivals unwanted, authoring findings.
   - *Roots*: a table: name, levels (authored names), selected, state, granted, resident, drawn, measure, priority, bytes; filters: late,
     hole, refused, prefetching, with findings, under the selection, by path. Click selects, double-click frames.
   - *Inspector* (`view.streaming.inspect {root}`, the `streamingtool` record for the root at or above the selection): its heuristics by
     domain, used or why not; the streaming radii authored and in force; its state and why (which radius, the previous state), granted and
     why; per level its authored name, bytes per budget, requested / resident / drawn, lead, reloads; its authoring findings with the
     numbers; its parent and children roots.
   - *Configuration*: the budgets and margin ([plan_streaming_budgets.md](plan_streaming_budgets.md) step 2), the streaming radii's tuning
     per authored root (load: `lod-thresholds h=<i>`, unload: `lod-unload-thresholds`), reset per root and all.
   - *Measurement*: reset counters (`view.streaming.reset`), fly a camera path at a speed and stop (`fly`, `fly-stop`), and a strip chart of
     the last frames: resident bytes per budget, requests in flight, late roots. The chart measures; nothing acts on it.
2. **The map** (decision 2), in the window, toggled with its own section.
3. **Overlays** (`view.streaming.visualize`, `streaming-show bounds|rings|labels on|off`), drawn with `DebugDraw` only for the roots placed
   (a few hundred at the village, never one box per draw):
   - *bounds*: each root's streaming bounds in its state's colour (decision 3);
   - *rings*: the selected root's (or with `rings-all`, every placed root's) load and unload rings per boundary, beside the imaging rings of
     the LOD tool's gizmo;
   - *labels*: name, drawn / selected, state, granted; a click selects.
4. **The `streaming` view mode** (`view streaming`): every draw coloured by its streaming root's state, as `lodlevel` colours by level.
5. **Engine Map**: a Residency node between LOD selection and Placement (`src/ui/enginemap.cpp`): what it decides (state, granted, the
   requests), its records (`streaming`, `streamingtool`) and windows (Streaming); edges from LOD selection (the imaging pick) and to the
   asset client (requests) and Placement (resident levels).
6. **READMEs**: `src/view/README.md` (the streaming tool), `src/ui` where the windows are described; the `run-headless` skill (the verbs).

## Visibility

- **Records**: `streamingtool` (the inspector's root); `streaming` unchanged by this plan.
- **RPC and verbs**: `view.streaming.inspect {root}`; `view.streaming.visualize {bounds, rings, ringsAll, labels}` /
  `streaming-show bounds|rings|rings-all|labels on|off`; `view streaming`; `window streaming on|off`. Configuration uses the other plans'
  methods (`view.streaming.budgets`, `view.lod.override`, `view.streaming.reset`, `view.camera.fly`).
- **Windows**: Streaming (new), the LOD window (cross-links), the Engine Map's Residency node.

## Verification

- **Same numbers**: at `/cameras/village`, the window's overview and the `streaming` record's totals agree (read through RPC while the window
  is open); the roots table has a row per placed streaming root (196).
- **Every control has its method**: each configuration and measurement control, used once from a script through its verb, changes the
  record as the window would; the window shows the change on the next snapshot.
- **Overlays**: screenshots at `/cameras/village` with `streaming-show bounds on`, `rings on` with a tile selected, `labels on`, and
  `view streaming`: each shows what it says, the colours as the legend; DrawDebug stays under 1 ms a frame with bounds on (a few hundred
  boxes, not the AABB overlay's million).
- **States shown**: a tile's load radius tuned under its imaging threshold shows the tile late in the map, the bounds and the table, its
  finding in the inspector; a budget set to half the village's use shows the refused roots in the budget colour.
- **Engine Map**: the Residency node lists its records and windows, and its edges.
- No warnings or errors in the trace.

## Deferred / follow-ups

- **A timeline of a flight**: per root, when it was requested, resident and drawn, scrubbable. Trigger: late switches on a flight that the
  counters don't explain.
- **Writing tuned radii back** to the authored USD. Trigger: the editor ([plan_lod_tool.md](plan_lod_tool.md) defers the same for
  thresholds).
