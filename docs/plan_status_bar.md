# Status bar, and screenshots without UI

**Status. Landed.**

A bar along the bottom of ngen-view: frame rate, frame time and GPU time on the left; memory and asset activity on the right. Headless
screenshots no longer include the UI, so the bar's changing numbers don't reach them; `--show-ui` keeps the UI in.

## Current state

- **The Performance window** reads `profile::frameHistory` (`FrameStats`: CPU frame interval and the latest known GPU frame time).
- **Memory:** `RhiDevice::memoryHeaps()` reports the device memory the engine allocated per heap, and the driver's budget and usage where it
  reports them; it is mutex-guarded, so the main thread can call it. Nothing reports the process's own memory.
- **Assets:** `AssetClient` keeps per-id state but no counts. `asset.ready` doesn't say whether the server ran a packer or served its cache.
- **Screenshots** copy the backbuffer at the end of `Renderer::render`, after the editor UI pass, so they include the menu bar. The path is set from
  the main thread (`Renderer::requestScreenshot`) and read late in `render`.

## Scope

**In**

- **`drawStatusBar(const StatusBarData&)`** (`src/ui/statusbar.*`), drawn by `EditorUI` as an imgui viewport side bar at the bottom. Left: fps,
  frame time and GPU time, averaged over the last second. Right: CPU memory (the process's resident set), GPU memory (device memory the engine
  allocated), and the asset counts.
- **Asset statistics** in `AssetClient::stats()`: requested, received, packed, cached, failed, in flight, and bytes received.
- **`packed` in `asset.ready`**: true when the server ran a packer for the asset, false when it served its cache.
- **Screenshots without UI** by default: the frame that takes a screenshot skips the editor UI pass. **`--show-ui`** keeps the UI in screenshots.
- **A View menu toggle** for the bar.

**Out**

- The driver's budget and per-heap breakdown in the bar; the Memory window has them.
- History or graphs in the bar; the Performance window has them.

## Decisions

Locked with Henrik:

1. Screenshots have no UI unless `--show-ui` is given.
2. Asset counts are tracked by the client, with the server saying per asset whether it packed it.
3. Memory shows CPU and GPU separately.

Proposed:

4. **The frame that takes a UI-less screenshot skips the editor UI pass**, instead of copying the image before the UI is drawn. One frame on
   screen shows no UI when a screenshot is taken interactively; headless runs are unaffected. Copying before the UI pass would need a frame graph
   copy pass into a readback buffer, which the frame graph doesn't have.
5. **The view assembles `StatusBarData`** (it has the device and the asset client), and the UI library only draws it. The UI library gains no
   dependency on the RHI device or the asset client.
6. **CPU memory is the resident set** from `/proc/self/statm`; GPU memory is the sum of `allocated` over every heap.

## Steps

1. **Asset server:** `Outcome::packed`, set when the packer ran; `asset.ready` carries `packed`.
2. **`AssetClient`:** count requested ids, received and failed assets, packed or cached, bytes received; in flight is requested minus received
   minus failed. `auto stats() const -> AssetClientStats`.
3. **Renderer:** take the screenshot path at the start of `render`; skip `editorUIPass` that frame unless `setScreenshotsShowUi(true)`.
4. **ngen-view:** `--show-ui`; build `StatusBarData` each frame from `profile::frameHistory`, `/proc/self/statm`, `memoryHeaps()` and
   `AssetClient::stats()`.
5. **UI:** `src/ui/statusbar.h/.cpp`, drawn from `EditorUI::draw`; the toggle in the View menu.
6. **Docs:** the run-headless skill (`--show-ui`), `src/asset/README.md` (`packed`, `stats()`), `src/renderer/README.md` (screenshots).

## Verification

- **Screenshots without UI:** a three_cubes screenshot differs from the old baseline only in the menu bar's rows; those become the new baselines.
- **`--show-ui`:** a screenshot with it matches the old baseline except for the bar's rows at the bottom.
- **Asset counts:** on Sponza with an empty cache the bar's counts match the server's trace (requests, packed, cached, failed); a second run shows
  the same assets as cached, none packed.
- **Memory:** the CPU figure matches `ps -o rss` for the process within a few percent, and the GPU figure matches the Memory window's total.
- **Numbers:** fps × frame time ≈ 1000, and GPU time matches the Performance window.

## Results

- **Screenshots without UI:** each of the six headless screenshots differs from the old baseline only in rows 0–18, the menu bar; they are the new
  baselines.
- **`--show-ui`:** a three_cubes screenshot with it differs from the old baseline only in rows 1421–1439, the status bar.
- **Asset counts:** Sponza on an empty cache showed `43 requested 43 packed 0 cached 0 failed 0 in flight 2.49 GB`; the server's trace had three
  requests of 17, 1 and 25 assets, all packed, 138 KB + 416 MB + 2,133 MB. three_cubes on a warm cache showed 18 requested, 18 cached.
- **Memory:** CPU 3.15 GB against `ps` 3,199 MB (3.12 GB, sampled a moment later); GPU 2.84 GB against the heaps' allocated total in
  `introspect.memory`, 2,907 MB.
- **Numbers:** 845.5 fps with 1.18 ms frames on three_cubes; GPU time comes from the same `FrameStats` as the Performance window.
- All three configurations and the examples build.

Deviations from the plan:

- **The screenshot path is taken at the start of `render`**, and put back if the frame is skipped (swapchain out of date), so the frame that drops
  the UI is the frame that is read back.
- **An empty asset now arrives** in `AssetClient`: with no `asset.data` before its `asset.ready`, it used to be taken for "ready without data" and
  failed.

Added after landing: **triangle counts** on the left, `drawn / scene tris`. Drawn is the camera's triangles after GPU culling, which the cull
shader already counted per region (`counterPrimitives`, regions 0 and 1) and the readback already copied; `CullResult` now carries it as
`cameraTriangles`. Scene is every instance's submesh triangles, summed on the render thread from the GPU instance list (`sceneTriangles`). On Sponza
the bar showed `447.9K / 3.75M`; the render debug dump's GeometryPass draws add up to 447,895 triangles (65 draws, 342 of 407 instances culled),
and with `cull off` to 3,747,022 (407 draws), where the bar showed `3.75M / 3.75M`.

Also added: **triangles in the Culling window**, in a table with the instance counts: scene, drawn and culled for the camera, and per shadow
cascade drawn, culled and triangles (`CullResult::cascadeTriangles`, regions `2 + 2c` and `3 + 2c`). On Sponza: 3,747,022 / 447,895 / 3,299,127
(88%) for the camera, and cascades of 846,869, 2,334,039 and 3,521,487, which add up to the render debug dump's ShadowPass total, 6,702,395. The
`window` verb gains `culling`, to open it headless.
