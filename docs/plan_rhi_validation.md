# RHI validation program (`ngen-test-rhi`)

**Status. Draft.**

## Current state

The RHI is five abstract headers under `src/rhi/` (`rhidevice.h`, `rhicommandbuffer.h`, `rhiswapchain.h`, `rhieditorui.h`, `rhitypes.h`) with one
backend in `src/rhi/vulkan/`. Nothing tests it directly: the only consumer is the renderer inside `ngen-view`, and the only verification loop is a
headless obs-bus run of the whole engine (see the `run-headless` skill). An RHI bug therefore surfaces as a wrong-looking frame or a validation splat
deep inside an engine run, never as a pointed, named failure. There is no test framework, test target, or `tests/` directory anywhere in the repo.

Three gaps block a direct test program today:

- **No CPU readback.** `RhiDevice` has `mapBuffer`/`copyBuffer` (`src/rhi/rhidevice.h:46-48`) but no image→buffer copy, so a program cannot inspect
  what was rendered.
- **Window-coupled device init.** `RhiDevice::init(SDL_Window*)` (`src/rhi/rhidevice.h:22`) always creates a surface
  (`src/rhi/vulkan/rhidevicevulkan.cpp:236`) and requires a present-capable queue (`:260`).
- **Validation is a compile-time toggle with no observer.** `NGEN_ENABLE_VALIDATION` (commented out at `build.cpp:64`) enables the layer but installs
  no debug messenger, so a program cannot detect that validation complained.

The closest existing idiom to build on is the observation bus (`docs/plan_observability.md`, `obs.md`).

## Decisions (locked)

- **One binary per system under test, hand-rolled harness.** This plan ships the first: `ngen-test-rhi`. The `ngen-test-<system>` naming deliberately
  leaves room for `ngen-test-jobsystem`, `ngen-test-swapchain`, … later. The registry/reporting harness is a generic static library shared by all of
  them (step 3); each binary owns its system-specific setup. No vendored test framework — Catch2/doctest were considered and rejected (new dependency,
  foreign idiom).
- **Readback plus analytic checks, no golden images.** Tests assert computed expectations (clear to red → every pixel is red; a known triangle covers
  known pixels). Robust across GPUs and drivers, nothing to bless or maintain. Render targets use `R8G8B8A8_UNORM`, not sRGB, so expected byte values
  are exact.
- **Surfaceless device init.** The test device is created with no window at all — no SDL video subsystem, no surface, no present queue requirement.
  Cleaner than the SDL-offscreen-window trick and CI-friendly; the cost is that swapchain/present tests are out of scope (deferred below).
- **Validation messages fail tests.** The test device runs with `VK_LAYER_KHRONOS_validation` plus a `VK_EXT_debug_utils` messenger; any error- or
  warning-severity message recorded during a test marks it failed. This turns the validation layers into assertions on correct RHI API usage.
- **The validation toggle moves from compile time to runtime.** A consequence of the above that was not part of the original fork: the
  `NGEN_ENABLE_VALIDATION` define cannot be set per-target because `rhivulkan` is one static library shared by `ngen-view` and `ngen-test-rhi`.
  Replacing the define with a flag on device init keeps one library build and lets each program choose; the commented define at `build.cpp:64` goes
  away. The minimum knob for `ngen-view` — there is no config-file or settings system yet — is a `--validation` CLI flag added to the existing
  hand-rolled argument loop (`src/main.cpp:58-112`, next to `--obs-output=`), default off, so app behavior is unchanged. `ngen-test-rhi` does not use
  the flag; it hardcodes validation on.
- **Tests speak only the abstract RHI.** Test files include `rhidevice.h`/`rhicommandbuffer.h`/`rhitypes.h` only; the single place that names
  `RhiDeviceVulkan` is the suite's `main.cpp`. A future backend (e.g. d3d12) runs the same suite by swapping that one construction site.

## Scope

**In**

- `copyTextureToBuffer` readback entry point on `RhiCommandBuffer` + Vulkan implementation.
- Surfaceless init and runtime validation toggle via a new `RhiDeviceDesc`; debug messenger with error/warning counters; `src/main.cpp` updated.
- Generic `testharness` static library (registry, runner, obs-bus reporting — reusable by future `ngen-test-<system>` binaries) and the
  `ngen-test-rhi` target.
- CPU-side tests: buffer map roundtrip, buffer copy, texture upload→readback.
- Rendering tests: clear color, triangle coverage, push-constant color, depth test on/off, sampled-texture descriptor, blit roundtrip.

**Out**

- Swapchain / acquire / present tests (need a windowed device — planned follow-up suite, see Deferred).
- Golden-image comparison (deferred).
- Performance or stress tests; threaded-submission tests.
- `RhiEditorUI` / ImGui backend coverage.
- CI wiring (the project has no CI).

## Steps

### Phase 1 — infrastructure + CPU-side tests

Ships: `ngen-test-rhi` builds, runs green surfaceless, reports over the obs bus.

1. **Device init desc** — replace the window parameter in `src/rhi/rhidevice.h`:

   ```cpp
   struct RhiDeviceDesc {
       SDL_Window* window = nullptr;   // nullptr = surfaceless: no surface, no present-queue requirement, createSwapchain unavailable
       bool enableValidation = false;  // VK_LAYER_KHRONOS_validation + VK_EXT_debug_utils messenger
   };

   virtual auto init(const RhiDeviceDesc& desc) -> std::expected<void, int> = 0;
   virtual auto validationErrorCount() const -> uint64_t { return 0; }
   virtual auto validationWarningCount() const -> uint64_t { return 0; }
   ```

   In `RhiDeviceVulkan::init` (`src/rhi/vulkan/rhidevicevulkan.cpp:180`): when `window == nullptr`, skip `SDL_Vulkan_GetInstanceExtensions` (no
   surface instance extensions), skip `SDL_Vulkan_CreateSurface`, select the queue family by graphics bit only, and drop `VK_KHR_swapchain` from the
   device extensions. When `enableValidation`, verify the layer exists via `vkEnumerateInstanceLayerProperties` (fail init with a clear message if
   missing), enable `VK_EXT_debug_utils`, and install a messenger whose callback increments the two counters and prints the message to stderr. The
   `#ifdef NGEN_ENABLE_VALIDATION` block at `:209` becomes runtime logic. `createSwapchain` returns `nullptr` on a surfaceless device.
   `src/main.cpp` gains a `--validation` flag in its existing argument loop (`:58-112`), and `:165` becomes
   `rhiDevice.init({.window = window, .enableValidation = validationFlag})`.

2. **Readback entry point** — add to `src/rhi/rhicommandbuffer.h`:

   ```cpp
   virtual auto copyTextureToBuffer(RhiTexture* src, RhiBuffer* dst, RhiExtent2D extent) -> void = 0;
   ```

   Vulkan implementation in `rhicommandbuffervulkan.cpp` uses `vkCmdCopyImageToBuffer`; the source must be in `TransferSrc` layout — the caller
   transitions it with the existing `pipelineBarrier`, the same contract `blitTexture` uses. Source textures need `RhiTextureUsage::TransferSrc`,
   destination buffers `RhiBufferUsage::TransferDst` with host-visible memory. (This entry point also unlocks in-engine screenshots later.)

3. **Generic harness** — new folder `src/tests/harness/` (concatenated lowercase filenames per the naming rules), a static library reusable by every
   future `ngen-test-<system>` binary. It knows nothing about the RHI:

   - `testharness.h` / `testharness.cpp` — registry, per-test context, runner, reporting:

     ```cpp
     struct TestContext {
         auto expect(bool condition, std::string_view message) -> void;  // records a failure; the test keeps running
     };

     using TestFunc = void (*)(TestContext&);
     auto registerTest(const char* name, TestFunc func) -> bool;
     #define NGEN_TEST(name) /* static-registration boilerplate expanding to registerTest */

     struct TestRunOptions {
         TestFunc beforeEach = nullptr;  // suite-level hooks, e.g. snapshot/check validation counters
         TestFunc afterEach = nullptr;
     };
     auto runTests(int argc, char** argv, const TestRunOptions& options) -> int;  // returns the failed count
     ```

     `runTests` parses `--obs-output=`, `--test=<name>` (run one test by name), `--dump-dir=<path>` (failure artifacts, off by default), emits
     `TestStarted{name}` / `TestPassed{name}` / `TestFailed{name, message}` on the obs bus (conventions per `obs.md`), prints a per-test PASS/FAIL
     line and a summary to stdout, and returns the number of failed tests (the binary's exit code).

     Suite-specific state is deliberately not in `TestContext`: `ngen-test-rhi` holds its `RhiDevice*` at file scope in the suite, set by its `main`
     before calling `runTests`. If a later suite needs richer sharing, revisit then.

   - `src/tests/rhi/main.cpp` — the only file naming `RhiDeviceVulkan`: creates one device with `{.window = nullptr, .enableValidation = true}` for
     the whole run (each test creates fresh resources), then calls `runTests` with an `afterEach` hook that fails the test if
     `validationErrorCount()`/`validationWarningCount()` moved during it.

4. **CPU-side tests** — `src/tests/rhi/buffertests.cpp`, `src/tests/rhi/texturetests.cpp`:

   - `buffer-map-roundtrip` — create a host-visible buffer, `mapBuffer`, write a byte pattern, read it back, byte-identical.
   - `buffer-copy` — write a pattern into a `TransferSrc` staging buffer, `copyBuffer` into a `TransferDst` host-visible buffer, map and compare.
   - `texture-upload-readback` — `createTexture` with `initialData` pattern and `TransferSrc` usage, barrier to `TransferSrc`,
     `copyTextureToBuffer`, submit, wait on a fence, map and compare byte-identical. Validates the new readback path against known input with no
     rendering involved.

5. **Build targets** — in `build.cpp`, next to the `ngen-view` block (`build.cpp:219`):

   ```cpp
   auto testharness =
       cxx::static_library("testharness")
           .sources(glob({.include = "src/tests/harness/**/*.cpp"}))
           .public_include({"src/tests/harness"})
           .include({"src", "src/obs"})
           .link(obs);

   auto testrhi =
       cxx::program("ngen-test-rhi")
           .sources(glob({.include = "src/tests/rhi/**/*.cpp"}))
           .include({"src", "src/obs", "src/rhi", "src/rhi/vulkan", "external/stb"})
           .link(testharness)
           .link(obs)
           .link(rhi)
           .link(rhivulkan)
           .link_flags(sdl3_libs)  // rhivulkan references SDL_Vulkan_* symbols even on the surfaceless path
           .depend_on(shaders);
   ```

   Register with `p.target(testrhi);` — `ngen-view` stays the default target. Build with
   `./_out/ngen-build -p linux-vulkan -c debug ngen-test-rhi`. Future suites are one more `cxx::program("ngen-test-<system>")` linking `testharness`.

### Phase 2 — rendering tests

Ships: pixel-level verification of the draw path.

6. **Test shaders** — `shaders/testflat.vert` / `shaders/testflat.frag` (position passthrough, solid color from a push constant) and
   `shaders/testtextured.vert` / `shaders/testtextured.frag` (UV passthrough, combined image sampler). The existing glslc tool glob
   (`build.cpp:209-217`) picks them up automatically; tests load the `.spv` via `createShaderModule` with the same out-dir-relative paths
   `ngen-view` uses.

7. **Render helpers** — RHI-specific, so they live in the suite, not the generic harness: `src/tests/rhi/rhitesthelpers.h` /
   `rhitesthelpers.cpp`. Create an `R8G8B8A8_UNORM` render-target texture with
   `ColorAttachment | TransferSrc` usage; a `readbackTexture(device, texture, extent) -> std::vector<uint8_t>` helper wrapping
   barrier → `copyTextureToBuffer` → submit → fence-wait → map; `expectPixel(pixels, extent, x, y, rgba)` with exact byte comparison. On a failed
   pixel expectation with `--dump-dir` set, write the readback as a PNG via `stb_image_write.h` (already vendored at `external/stb`).

8. **Draw tests** — `src/tests/rhi/drawtests.cpp`:

   - `clear-color` — `beginRendering` with a clear value, no draw, `endRendering`; every pixel equals the clear color.
   - `triangle-coverage` — draw one large solid triangle; the center pixel has the triangle color, a corner outside the coverage has the clear color.
   - `push-constant-color` — draw the same triangle twice with different push-constant colors; readback after each shows the respective color.
   - `depth-test` — two overlapping triangles at different depths with a `D32_SFLOAT` depth attachment: with depth test on, the near one wins
     regardless of draw order; with depth test off, the last-drawn one wins.
   - `descriptor-sampled-texture` — sample a 2×2 checkerboard texture onto a quad; pixels at texel centers match the checkerboard values.
   - `blit-roundtrip` — render a pattern, `blitTexture` same-size into a second texture, readback of the destination matches the source.

## Verification

- `./_out/ngen-build -p linux-vulkan -c debug ngen-test-rhi` builds; `ngen-view` still builds, and a headless run of `assets/three_cubes.usda` shows
  the same obs event mix as before the init-signature change (no regression from step 1).
- `./_out/linux-vulkan/debug/ngen-test-rhi --obs-output=/tmp/rhitest.jsonl` exits 0 **without** `SDL_VIDEODRIVER=offscreen` set — proving the
  surfaceless path uses no SDL video at all. The JSONL contains one `TestPassed` per registered test and zero `TestFailed`.
- `--test=buffer-copy` runs exactly that one test (one `TestStarted` in the JSONL).
- Negative harness check: temporarily invert one pixel expectation → the run exits nonzero and the JSONL `TestFailed` carries the message.
- Negative validation check: temporarily remove the barrier before `copyTextureToBuffer` in one test → that test fails via the validation counter
  delta while the others still pass.

## Deferred / follow-ups

- **Swapchain / acquire / present tests** — planned follow-up with its own plan (`docs/plan_test_swapchain.md`), reusing the `testharness` library:
  init SDL video with the offscreen driver, create a windowed device (`{.window = window, .enableValidation = true}`), exercise
  `createSwapchain` / `acquireNextImage` / `present` / `recreate`. Shape decision for that plan: a separate `ngen-test-swapchain` binary (leaning
  this — one device configuration per binary keeps the harness trivial and fits the `ngen-test-<system>` family) vs. a windowed test group inside
  `ngen-test-rhi`. Trigger: when this plan's phases land.
- **Golden-image comparison** — trigger: a class of rendering bugs analytic checks cannot express (gradients, blending, AA).
- **Second backend / more configs** — the suite already speaks only the abstract RHI; trigger: d3d12 backend work starts, or a release-config bug.
- **Renderer/frame-graph-level tests** on the same harness — separate plan when wanted.
- **Warning allowlist** — if a driver emits spurious validation warnings, add a per-test allowlist. Not built until it happens.
