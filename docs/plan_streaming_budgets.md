# Streaming budgets: what fits, and in what order it comes

**Status. Draft.** Decisions as leaned, pushback welcome. Third of three plans: [plan_streaming_domain.md](plan_streaming_domain.md) (what is
resident), [plan_streaming_tool.md](plan_streaming_tool.md) (the window and overlays, where these budgets are set), this one (what fits).

## Current state

After [plan_streaming_domain.md](plan_streaming_domain.md), a root's resident levels follow its authored radii, and nothing bounds the
total: memory grows with whatever the radii ask for, and every pack a level names is requested at once, in the order the passes reach
them. [plan_streaming_lod.md](plan_streaming_lod.md) decision 4 (locked 2026-10-11) already says what budgets are for: GPU geometry, GPU
textures and CPU resident packs, a priority per placement from its screen-space error, eviction lowest priority first, never the floor; its
"stays at least N seconds" hysteresis is replaced here by one that isn't on a clock ([plan_streaming_domain.md](plan_streaming_domain.md)
decision 1). The spec allows (§7, MAY) requesting coarser levels before finer ones, then nearer roots first.

What exists to build on:
- **Sizes after the fact**: the view knows each mesh pack's bytes once it is held (`releasedMeshBytes` counts them as they go), the
  `memory` record lists every GPU allocation, the asset client logs every request's bytes and times (`AssetLogEntry`).
- **One throughput limit**: the renderer's upload budget per frame (Scene Flow, `uploadBudgetMiB`). It paces uploads; it bounds nothing
  resident.
- **No settings store**: budgets are view settings, set by RPC, verb and window, kept until the view closes, like the LOD tuning.

## Scope

**In**
- Budgets on what streamed levels hold: GPU geometry, GPU textures, CPU packs held. A limit on requests in flight.
- Per root, *granted* beside *wanted* (the radii's state): the granted state is coarser when the budgets don't fit the wanted one.
- Request order: coarser levels first, then by priority.
- Records and RPC for budgets, use and what each root was refused; trace events when a budget refuses or releases.

**Out**
- The window and overlays that show and set them: [plan_streaming_tool.md](plan_streaming_tool.md).
- Texture mip streaming and partial packs (whole packs only, as now).
- Render-LOD levels inside a level (an asset's 8 levels are all resident with their tile; the budget sees the tile level's total).
- Budgets for anything that isn't a streamed level: the stage, materials and textures every level shares are counted, not refused.

## Decisions

### 1. Which budgets

- **A. Three memory budgets and one request limit**: GPU geometry bytes, GPU texture bytes, CPU bytes of packs held, and levels with
  requests in flight. As [plan_streaming_lod.md](plan_streaming_lod.md) decision 4 lists, minus "the view's cache of fetched packs" (the
  asset server's disk cache, not the view's).
- **B. One memory budget** over everything a level holds.

I lean **A**: GPU and CPU memory run out separately, and the `memory` record already splits them; one number would hide which one is short.

### 2. What gives when the wanted set doesn't fit

- **A. The lowest-priority roots are granted a coarser state**, one level at a time, until it fits: their finest wanted levels aren't
  requested, or are released. A root is never granted less than its coarsest level, which is what it needs to draw at all; when even those
  don't fit, the budget is over and the record says by how much, nothing else gives.
- **B. Refuse new requests only**, never release what is resident.

I lean **A**: B leaves memory to whoever came first, so a flight fills the budget behind the camera and starves what is ahead.

### 3. Priority

- **A. The root's projected size**: its streaming heuristic's bounds as a sphere over its distance from the camera (the same measure for a
  distance or screen-size heuristic), larger first. Position only, no frustum, as the heuristics ([plan_streaming_districts.md](plan_streaming_districts.md)
  decision 8).
- **B. Screen-space error** of not having the finer level: projected size times the detail step between the two levels, as
  [plan_streaming_lod.md](plan_streaming_lod.md) decision 4 has it. The detail step isn't authored, so it would be measured (triangles per
  level), known only once both levels were loaded.

I lean **A**: it uses only what is authored and the camera, so it is known before anything loads; B can follow once levels' sizes are kept
(decision 4).

### 4. A level's size before it is loaded

- **A. Unknown until it arrives**: a level is requested when the budget has room now; what it holds is counted as it arrives, and a budget
  overrun from a level larger than the room is released by decision 2 and counted as an overshoot. Sizes seen are kept for the session, so a
  level loaded again is admitted by its known size.
- **B. Sizes up front** from the asset server (a size query per pack), which it knows only for packs already in its cache.

I lean **A**: B is only partly possible (a cold cache doesn't know), and the request limit bounds how much one decision can overshoot.

### 5. Hysteresis at the budget's edge

A root granted a coarser state would be granted the finer one again as soon as something else is released, and flip. With no clock:
- **A. A margin**: a finer level is granted only while it fits under the budget minus a margin (10% until tuned); a level is released only
  while the budget is over. Between the two, nothing changes.
- **B. Never grant back** until the root's wanted state changes.

I lean **A**: same shape as the radii's hysteresis, in bytes instead of metres.

## Steps

1. **Bytes per level** (`src/view/placement.*`, `viewscene.*`): every level placement counts what it holds, by budget: mesh packs (CPU
   bytes, GPU bytes), textures (CPU, GPU), its layer packs (CPU), shared entries counted once per budget with the levels holding them listed.
   Kept per root and level for the session once seen (decision 4).
2. **Budgets** (`viewscene.h`):
   ```cpp
   struct StreamingBudgets {
       uint64_t gpuGeometryBytes = 0; // 0: unbounded
       uint64_t gpuTextureBytes = 0;
       uint64_t cpuPackBytes = 0;
       uint32_t levelsInFlight = 0;
       float margin = 0.1f; // decision 5
   };
   auto setStreamingBudgets(const StreamingBudgets& budgets) -> void; // a request, as the tuning is
   ```
   Defaults unbounded, so nothing changes until a budget is set.
3. **Granting** (`ViewScene::evaluateLevels`, after the radii's states): the roots' wanted levels in priority order (decision 3), coarser
   levels first; each granted while every budget it adds to fits (decision 5's margin for a finer one), else the root's granted state stops
   there. Over a budget: the lowest-priority roots' finest granted levels are released until it fits or only coarsest levels remain.
   `setResidentLevels` gets the granted set.
4. **Request order** (the scene thread's requests for level contents and their packs): a queue ordered as step 3, sent while fewer than
   `levelsInFlight` levels have requests out.
5. **READMEs**: `src/view/README.md` (budgets, granting, order).

## Visibility

- **Records**: `streaming` gains, per root, `granted` and why it is coarser than `wanted` (which budget), its priority, and its levels' bytes
  by budget; totals: each budget's limit, use, peak and overshoots, roots refused, the request queue's length. `memory` is unchanged and
  agrees with the GPU totals.
- **RPC and verbs**: `view.streaming.budgets` (get and set) / `streaming-budget gpu-geometry|gpu-textures|cpu-packs|in-flight <value>`,
  `streaming-budget margin <fraction>`.
- **Trace**: `BudgetRefused` (roots granted coarser, by budget), `BudgetReleased` (levels released to fit).
- **Windows**: [plan_streaming_tool.md](plan_streaming_tool.md) shows and sets them; its overlay colours a refused root.

## Verification

At fixed camera positions, which give the same result on every run:
- **Unbounded changes nothing**: with the defaults, `granted` equals `wanted` for every root at `/cameras/village`, and the resident set is
  [plan_streaming_domain.md](plan_streaming_domain.md)'s.
- **A GPU geometry budget** set to half of what `/cameras/village` holds: every budget's use ends at or under its limit (or the coarsest
  floor, reported); the roots refused are the smallest projected sizes; each refused root's `granted` is coarser and names the budget; the
  `memory` record's geometry total agrees.
- **No flip at the edge**: the same budget, the camera still for 600 frames: no `BudgetRefused` or `BudgetReleased` after the first settles.
- **Order**: from a cold start at `/cameras/village` with `levelsInFlight` 4, the asset client's log has every coarser level's packs
  requested before a finer level's of the same root, and no more than 4 levels with requests out at once.
- **Floor over budget**: a budget under what the coarsest levels need: the record says over by how much, every placed root draws a level,
  nothing else is released.

The flight is a measurement, as in [plan_streaming_domain.md](plan_streaming_domain.md): `fly /cameras/flight_ 30` with a budget, peak use
per budget and late switches reported beside the unbounded run under the same conditions, not judged.

## Deferred / follow-ups

- **Screen-space error priority** (decision 3 B). Trigger: levels' sizes kept per session make the detail step known.
- **Sizes from the asset server** (decision 4 B). Trigger: overshoots on cold starts that the request limit doesn't contain.
- **Cancelling requests** a budget no longer grants ([plan_streaming_domain.md](plan_streaming_domain.md), Deferred).
- **Texture mip streaming**: textures by level, inside the texture budget. Its own plan.
- **Budgets in project settings**, not only the view's. Trigger: a settings store.
