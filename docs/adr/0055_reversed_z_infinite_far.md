# 55. Reversed-Z with an infinite far plane

Date: 2026-10-13

## Status

Accepted

## Context

A fixed depth range of 0.1 to 3,000 units was copied in four places. Caldera framed from about 60 kilometres away and was clipped. Standard depth
in a 32-bit float wastes most of its precision near 1, where distant geometry lands, so a larger far plane would have meant z-fighting. Shadow
cascades fitted to a whole kilometre-scale scene came out at about one metre per texel.

## Decision

- One `perspectiveProjection` for the camera, with reversed-Z (depth 1 at the near plane, falling to 0 at infinity) and no far plane.
- Depth is cleared to 0 and tested Greater (Equal after the prepass, GreaterOrEqual where needed). Culling never rejects by distance.
- Shadow maps keep standard depth with `LessOrEqual`: an orthographic projection's depth is already linear.
- Cascades fit from the near plane to the smaller of the scene's far corner and a shadow distance, 200 metres by default. Beyond it, surfaces are
  lit without shadow.

## Consequences

- Easier: nothing is clipped at a distance, and precision holds far away.
- Easier: one projection instead of four copies of a range.
- Harder: no shadows beyond 200 metres by default.
- Harder: two depth conventions — camera and shadow — to keep in mind when writing a pass.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_depth_range.md](../plan_depth_range.md), [src/renderer/README.md](../../src/renderer/README.md) ("Depth", "Shadows").
- Code: `src/renderer/projection.h:5` (the convention) and `:11` (`perspectiveProjection`), `src/renderer/passes/geometrypass.cpp:54`
  (`RhiCompareOp::Greater`), `src/renderer/shadowcascades.h:24` (`maxDistance = 200`), `src/renderer/renderer.cpp:69` (the shadow compare sampler,
  `LessOrEqual`).
- Differs from plan: the plan's decision 4 (fly speed follows framing) was reverted the same day; it is camera behaviour, not depth.
- Rejected alternatives: a far plane fitted per frame (it would move culling and cascades with the camera); standard depth with a larger far plane
  (z-fighting); cascades fitted to the whole scene (too coarse); a shadow fade band (deferred).
