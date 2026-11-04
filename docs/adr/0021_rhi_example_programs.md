# 21. The RHI is verified by example programs with analytic checks

Date: 2026-09-19

## Status

Accepted

## Context

RHI changes needed a fast loop that runs unattended, so an agent can tell whether a change broke something without looking at a window. Running
the whole engine for every RHI change is slow, and its images depend on scenes and assets. Validation was switched by a compile-time define
(`NGEN_ENABLE_VALIDATION`), so it could not differ between the engine and a test program built in the same variant.

An earlier plan proposed `ngen-test-rhi`: one harness binary with a registry of tests, reporting through the observation bus, possibly on a test
framework. It was superseded before it was built.

## Decision

The RHI is exercised by small standalone programs under `src/rhi/examples/`, one per concept — triangle, quad, texture, uniforms, depth, render
target, blend, lines, push constants, mip cube, compute, indirect, bindless, timestamps, GPU zones — each on a shared `RhiExample` base:

- An example includes only `src/rhi/`, embeds its GLSL and compiles it with shaderc at start-up, and constructs the backend directly.
- Common flags: `--frames`, `--check`, `--screenshot`, `--resize-at`, `--validation`. Exit code 0 passes, 1 is a pixel mismatch, 2 a validation
  error or failure.
- `--check` compares pixels against values derived from constants in the same file, not against stored golden images.
- Validation is a runtime choice, `RhiDeviceOptions::enableValidation`, with `validationErrorCount()` turning validation output into a result.
- All examples build together with the `examples` target.

## Consequences

- Easier: each example reads as the integrator contract for one RHI feature, and new features (indirect, bindless) were proven here before the
  renderer switched to them.
- Easier: an agent verifies an RHI change with one loop of exit codes, no screenshots to look at.
- Easier: one backend build serves the engine (validation off) and the examples (validation on).
- Harder: SDL glue is duplicated in `examples/common/`, by design (ADR 20).
- Harder: CI later added a hash comparison of lavapipe screenshots, which sits in some tension with "no stored images": the checks stay analytic,
  the hashes only catch drift on one driver.

## Verification

- Sources: [plan_rhi_examples.md](../plan_rhi_examples.md), [plan_rhi_validation.md](../plan_rhi_validation.md) (superseded),
  [src/rhi/README.md](../../src/rhi/README.md) "Examples".
- Code: `src/rhi/examples/` (15 examples and `common/`), `src/rhi/rhidevice.h:18` (`RhiDeviceOptions`), `.github/workflows/build.yml:129` (lavapipe),
  `:182` (hashes against `src/rhi/examples/screenshots.sha256`). `NGEN_ENABLE_VALIDATION` no longer appears in `src/` or `build.cpp`.
