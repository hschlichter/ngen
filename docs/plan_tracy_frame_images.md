# Frame images in Tracy

**Status. Draft.**

## Current state

Tracy ([ADR 81](adr/0081_tracy_as_second_sink.md)) can show a small image of each frame under its frame ruler, so a spike in the timeline can be
matched with what was on screen. The engine sends none. Screenshots exist (`src/rendertools/screenshot.*`, `RenderTools::recordScreenshot`): a
full-size copy of the backbuffer to a readback buffer, written as PNG in `afterSubmit`, which waits for the frame's fence (`waitForFrame`) — fine
for one screenshot, a stall if done every frame.

## Goals

1. While a Tracy server is connected, every presented frame reaches Tracy as a small image at its frame, without stalling the render thread.
2. Nothing runs while no server is connected.

## Decisions

Locked with Henrik on 2026-11-09.

1. **Tracy only.** The built-in collector keeps no images; agents and the window already have screenshots (`screenshot`, `--screenshot`).
2. **In `src/rendertools`**, which owns screenshots and the readback path, as part of its render extension.
3. **The presented image, UI included**, as the viewer saw it: a frame image is for recognising the moment, not for comparing pixels.

## Scope

**In**

- A per-slot downscale and readback in `RenderTools`, recorded only while Tracy is connected; the image sent to Tracy after the slot's fence.
- `profile::frameImage` (Tracy's `___tracy_emit_frame_image`) and `profile::tracyConnected()` for gating, in `src/profile`.
- The profile frame index of a rendered frame in the `RenderSnapshot`, to give Tracy the right frame.
- A config value `profile.tracy.frameImages` (default on) to turn them off when they are in the way.

**Out**

- Images in the built-in collector or the Performance window.
- Images in gamerelease (no Tracy).

## Steps

1. **Profile API.** `src/profile/profile.h`:

   ```cpp
   // True while a Tracy server is connected; false without TRACY_ENABLE.
   auto tracyConnected() -> bool;
   // An RGBA image of the frame `frameIndex` (a frame-mark count, see frameCount); dimensions divisible by 4. Tracy only.
   auto frameImage(const uint8_t* rgba, uint16_t width, uint16_t height, uint64_t frameIndex) -> void;
   // Frame marks so far.
   auto frameCount() -> uint64_t;
   ```

   `frameImage` computes Tracy's offset as `frameCount() - frameIndex`, clamped to 0–255, and drops images older than that.

2. **Which frame.** `ViewFrame` stores `profile::frameCount()` in the `RenderSnapshot` it builds (`profileFrame`); the render thread passes it
   through `RenderFrame`.

3. **Downscale and readback.** `src/rendertools/frameimage.h/.cpp`, used by `RenderTools`:
   - At `init`: per frame slot, a target texture of 320 × (320 × height / width, rounded down to a multiple of 4), RGBA8, and a readback buffer
     of its size; recreated when the swapchain extent changes.
   - At `afterExecute`, when `profile::tracyConnected()` and `profile.tracy.frameImages`: a linear blit from the backbuffer to the slot's
     target (the blit converts BGRA to RGBA), then a copy to the slot's readback buffer, with the barriers around it. The frame is marked as
     having an image.
   - At `afterFence(slot)`: if the slot's frame has an image, map the buffer and `profile::frameImage(…, profileFrame)`. No wait.
   - Group Upload zones for the blit and copy (`PROFILE_GPU_ZONE_DETAIL`), a CPU zone for the hand-off ([plan_profile_detail.md](plan_profile_detail.md)).

4. **Config value.** `profile.tracy.frameImages` (`config::Bool`, default true) in `src/profile/profileconfig.cpp`; the main thread puts it in the
   `RenderSnapshot`.

5. **Docs.** `src/rendertools/README.md` (frame images beside screenshots, where they sit in the frame), `src/profile/README.md` (Tracy section).

## Visibility

- **Records**: `config` lists `profile.tracy.frameImages`. No other record: the images exist only in Tracy, and the engine's own screenshots
  (`screenshot`) are the agent's way to see a frame.
- **RPC methods and verbs**: `config.set`/`config` for the value. No new method.
- **Windows**: the Configuration window lists the value. Tracy shows the images.

## Verification

- With `tracy-capture` connected for 300 frames of Sponza, release: the capture holds one frame image per frame, give or take the frames in flight
  at connect and disconnect (Tracy's GUI shows them; Henrik's visual check that the image under a frame is that frame, by moving the camera with a
  script at a known frame).
- Without a server, `profile.gpu.detail` on: no frame-image GPU zone in the `zones` record, and no GPU memory in the `memory` record beyond the
  per-slot targets.
- With a server: the RenderTools `afterFence` hook zone ([plan_profile_detail.md](plan_profile_detail.md), step 7) grows by less than 0.2 ms
  per frame (`zones` record), and `WaitFence` does not grow.
- `config profile.tracy.frameImages off`: the images stop at the next frame.
- Trace: no warnings or errors; validation clean (`--fail-on-validation`).

## Deferred / follow-ups

- **Larger images on request** (a config value for the width). Trigger: 320 pixels too small to recognise a moment.
- **Allocating the targets only on first connect.** Trigger: the per-slot targets' memory (about 300 KB per slot) matters.
