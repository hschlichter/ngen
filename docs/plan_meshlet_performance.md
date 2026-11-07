# Meshlet performance: the culling compute and the mesh passes, squeezed

**Status. Landed.** Decisions taken by the agent on the user's instruction ("make all the decisions along the way yourself; as much
performance as can be squeezed out"). Follows [plan_meshlets.md](plan_meshlets.md), [plan_occlusion_culling.md](plan_occlusion_culling.md);
the learnings so far are in [meshlets_learnings.md](meshlets_learnings.md).

## Current state

Release, AMD 890M (RADV), the test world (about 3 M instance slots), median frame GPU time over 60 frames at frame 300:

| Viewpoint | Frame GPU | Triangles drawn | Instances drawn |
|---|---|---|---|
| `flight_10` | 68.9 ms | 4.77 M | 239,715 |
| `dense_forest` | 55.5 ms | 0.11 M | 791 |
| `overlook` | 62.8 ms | 18.75 M | 830,084 |

At `dense_forest`, with almost nothing drawn, about 50 ms is the culling compute: `InstanceCull` 8, `Scan` 2.6, `Scatter` 12.7,
`InstanceCullOcclusion` 13, `LateScan` 2.6, `LateScatter` 11.4. Every one of them visits every slot; the scatters walk 20 (or 4) regions with
a workgroup-wide scan each, whether or not the workgroup has anything in them; the occlusion pass reads every instance record; the scans walk
11,600 workgroups serially per counter.

## Scope

In: the culling compute (cull, scan, scatter, occlusion, late phase), the instance data it reads, the mesh passes' shaders and dispatch, the
shadow cascades' cost. Anything that keeps the image the same, or changes it only where a decision below says so.

Out: chunk-level (streaming cell) culling: roadmap streaming work. Cluster LOD: roadmap, deferred. Authoring changes to the test world.

## Steps

Each step was measured with the same harness (three viewpoints, release, median frame GPU time over 60 frames at frame 300, from the `profile`
record) and kept only if it won; instance, meshlet and triangle counts were checked against the step before.

1. **Skip what a workgroup doesn't hold.** The scatters read their workgroup's counters and skip regions with no member, and the whole
   workgroup when it has none; only a slot drawn this phase reads its instance. The occlusion pass reads the visibility word first and touches
   the instance only for a camera-visible slot.
2. **A parallel scan.** One workgroup per counter, 256 workgroups at a time with subgroup prefix sums; a layout dispatch
   (`InstanceCullLayout`) writes the regions' draws from the totals. Subgroup arithmetic is required at device creation; every shader is
   compiled for Vulkan 1.3 (SPIR-V 1.6), which also needs `shaderDemoteToHelperInvocation` enabled.
3. **Group boxes.** A box per workgroup of 256 slots, kept on the CPU as slots change (`DrawLists::updateGroupBounds`) and copied per frame
   slot; a group outside every view skips its slots.
4. **Occlusion in the geometry pass.** Without the depth prepass the geometry pass draws the first phase, the pyramid comes from its depth, and
   `GeometryPassLate` draws the second (`GeometryPhase`). The prepass path stays for `render.prepass` on; the culling-camera path is unchanged.
5. **Packs.** The task shader packs consecutive surviving meshlets into one mesh workgroup (64 vertices, 124 triangles), with each survivor's
   pool offsets in the payload; a one-meshlet pack reads at uniform addresses, a several-meshlet pack copies its words to shared memory once.
6. **A slim culling record.** `GpuCullRecord`, 48 bytes, after the draw records in the instance buffer; every culling pass reads it.

## Decisions

- **Image changes**: none intended, and none found where checked. Lit images and shadow maps at `flight_10` and `dense_forest` are
  byte-identical to the reference taken after step 4 (steps 5 and 6 and the reverts changed no pixel). Step 4 changes which triangles are
  drawn (first-phase meshlets aren't tested against the pyramid) and could resolve depth ties differently from the prepass path; not compared
  against a pre-step-4 release image.
- **Counts that change**: a group outside every view counts its slots as culled (render-LOD slots in it are not counted as levels not drawn),
  and dead slots in such a group count as culled. Drawn instances, meshlets and triangles are unchanged by every step but step 4 (triangles drawn
  rise a little: 4.77 M → 4.95 M at `flight_10`).
- **Render-LOD residency** isn't marked for slots in a skipped group; no view sees them. A group turning into view could draw a level whose
  meshes aren't resident for one frame (not seen).

## Results

Frame GPU time, release, AMD Radeon 890M (RADV), median of 60 frames:

| Step | `flight_10` | `dense_forest` | `overlook` |
|---|---|---|---|
| Baseline (two-phase occlusion in the prepass) | 68.9 ms | 55.5 ms | 62.8 ms |
| 1. Skip empty regions and unread slots | 36.4 | 23.3 | 45.9 |
| 2. Parallel scan | 31.0 | 18.6 | 42.8 |
| 3. Group boxes | 27.7 | 13.8 | 41.5 |
| 4. Occlusion in the geometry pass | 22.7 | 13.8 | 29.6 |
| 5. Packs | 19.9 | 14.1 | 25.9 |
| 6. Slim culling record | **15.3** | **12.2** | **19.2** |

For comparison, main (indexed draws, no occlusion) at `flight_10`: 49 ms.

Tried and reverted (no gain, or not exact):

| Change | Result |
|---|---|
| 32-thread mesh workgroups | +0.5 ms |
| Subgroup counters and compaction in the task shader | no change |
| Subgroup scans in the scatter | no change |
| 64 meshlets per task workgroup | no change |
| Packs of 128 vertices and 256 triangles in 128-thread workgroups | `dense_forest` +1.3 ms (larger cascade meshlets, idle lanes) |
| Packing done in shared memory, payload written in parallel | `dense_forest` +0.6 ms |
| Normal cone against the light in the cascades | −0.2 ms, but a few hundred shadow-map texels changed |
| Per-triangle culling in the mesh shaders (back faces, no pixel centre covered) | exact with a half-pixel margin, but +1.5 to +3.3 ms |

Where the time is now: the shadow and geometry passes run at about 1.3 to 1.4 billion triangles a second, whether writing the G-buffer or depth
only, and a quarter-screen scissor didn't change the geometry pass: primitive throughput, not pixels or vertex fetch, bounds them. The culling
compute is about 3 ms. Fewer triangles (LOD) is what would move it further.

Pass times in the `render` record overlap (their sum exceeds the frame): `CullReadback` shows 7 to 8 ms, but leaving it out saves 0.15 ms.

## Visibility

- **Records**: no new ones. The `render` record and the Frame Graph window show the new passes (`InstanceCullLayout`,
  `InstanceCullLateLayout`, `GeometryPassLate`) and the per-frame-slot `cull.groupbounds` buffer (also in `memory`); the `profile` record's frame
  times measure each step; the `culling` record's counts are the correctness check.
- **Captures**: `capture InstanceCull cullGroupBounds` shows the group boxes as raw words (no schema yet); the instance buffer's culling records
  sit after its draw records, so a capture of `instances` decoded as draw records shows them as rows past the capacity. A schema for both is
  a gap, named here.
- **Verbs, RPC, windows**: none new.

## Verification

- At each viewpoint, the `culling` record's instance, meshlet and triangle counts equal the baseline's, and screenshots are byte-identical,
  unless a step's decision says otherwise.
- No validation messages (`--fail-on-validation`) on the test world, Sponza and layered.
- Frame GPU time per viewpoint recorded per step.

## Deferred / follow-ups

- **Chunk-level culling** (skip authored streaming chunks before their slots). Trigger: the cull passes above 2 ms with group boxes in place.
- **A 64-byte draw record** (an affine model and packed fields) for the task and mesh shaders. Trigger: the geometry pass bound by memory rather
  than primitives.
- **Async compute** for the culling. Trigger: the graph gains a second queue.
