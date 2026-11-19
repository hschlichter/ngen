# 88. The remote view: the view's image as H.264 over WebRTC, from inside the engine

Date: 2026-11-15

## Status

Accepted

Builds on [ADR 84](0084_tracy_frame_images.md)'s readback (blit and copy in the frame's own command buffer, read at the frame context's next
fence), which stays as it is in `src/rendertools`.

## Context

ngen is developed remotely: agents run ngen-view on the development machine, mostly headless, and the only way to show a person what it draws
was a PNG. Watching a session live from a phone needed the image to leave the machine as video, without remote desktop software and without
capturing a desktop or a window, because a headless run has neither. RPC is loopback only by design, so nothing in the engine was reachable
from another device.

## Decision

Reviewed with Henrik on 2026-11-15 (locked: the name, no authentication, Firefox as the local check, the phone check is his; the rest accepted
as recommended).

- **A library of its own, `src/remoteview`, with its capture as its own render extension** (`RemoteViewCapture`, added after the UI's), not
  inside `rendertools`: instrumentation stays free of third-party libraries. It repeats ADR 84's blit and copy at the video's size.
- **Tooling.** Only ngen-view's `NGEN_INTROSPECTION` code uses it; `gamerelease` links none of it, nor its libraries.
- **WebRTC through libdatachannel** (MPL 2.0), a submodule built once with CMake into `external/libdatachannel_build`, static, without
  WebSocket, on the system's OpenSSL: the pattern OpenUSD set for a dependency with its own build.
- **H.264 Constrained Baseline, encoded in software by OpenH264** (BSD 2-Clause), a submodule built into `external/openh264_build`, behind a
  `VideoEncoder` interface. H.264 because this machine's GPU can encode it later; its patent licence was set aside for now.
- **The stream says its own colour**: BT.709, limited range, converted on the CPU.
- **Signalling over a small HTTP listener of our own**, which also serves the watch page. It is off by default, listens on every interface
  unless given one address, and has no authentication: what it exposes is the picture, read-only.
- **One watcher, one image waiting.** A newer image replaces the one the encoder hasn't taken, so latency can't build up.
- **The presented image, UI included.**
- **A test pattern as a second source**, not a test program: it brings up the connection without the renderer, and carries the time the watch
  page measures latency from.
- **The watch page reports back** once a second what the browser decoded, so the `remoteview` record shows what the watcher sees, on a device
  no agent can reach.
- **libdatachannel's exceptions stop in `videosender.cpp`.**

Rejected: Google's libwebrtc and GStreamer (a build system or a framework for one track); x264 (GPL); libavcodec (FFmpeg for one codec);
Vulkan Video first (new RHI surface and an optional extension before any picture); capture in `rendertools`; a WebSocket for signalling (an
HTTP listener is needed for the page anyway); a separate test program.

## Consequences

- Easier: a headless run can be watched live from a phone; what the watcher decodes is a record an agent reads.
- Easier: a later hardware encoder replaces one class; a GPU-only path replaces capture and encoder and keeps the sender.
- Harder: a fresh clone builds two more libraries before the engine, and CI caches them; OpenSSL becomes a system dependency of ngen-view in
  debug and release.
- Harder: a port on every interface, with no authentication, once it is turned on. It is off by default and can be kept to one address.
- Harder: `cvar::String` exists now, for the address; the cvar library had no text type.
- Cost, release, Sponza, headless, this machine: nothing measurable while off or unwatched; watched, 0.63 ms on the render thread for each
  captured frame (the copy) and 0.02 ms of GPU time a frame; conversion 2.1 ms and encode 1.8 ms on the encode thread; 37 MB of targets and
  buffers while watched (computed: five frame contexts at 1280 × 720, a target and a buffer each).

## Verification

- Sources: [plan_remote_view.md](../plan_remote_view.md), `src/remoteview/README.md`.
- Code:
  - `src/remoteview/remoteview.h:24` (`RemoteView`); `src/remoteview/remoteview.cpp:100` (`update`), `:240` (`takeFrameDue`), `:254` (`submit`),
    `:326` (`encodeLoop`), `:489` (`serve`), `:29` (the silent watcher's five seconds).
  - `src/remoteview/remoteviewcapture.cpp:24` (`fillSnapshot`), `:60` (`afterExecute`: blit and copy), `:122` (`afterFence`: which frames are done), `:145` (`read`: the hand-off).
  - `src/remoteview/videosender.cpp:46` (`offer`), `:149` (`accept`), `:166` (`send`).
  - `src/remoteview/videoencoderopenh264.cpp:57` (the profile), `:67` (the colour description).
  - `src/apps/view.cpp:440` (made), `:454` (the extension added after the UI's), `:562` (commands), `:669` (`update`), `:240` (`--cvar`),
    `:252` (`--remote-view`).
  - `build.cpp:279` (the library), `:603` (its libraries on ngen-view's link); `src/cvar/cvar.h:161` (`cvar::String`).
- Differs from plan: the engine offers and the watch page answers, in two requests, where the plan had the page offer in one (libdatachannel's
  documented path is to offer); a silent watcher is dropped after five seconds and a replaced page stops, neither of which the plan had; the
  capture's targets are given back five seconds after the last capture; the frame counts are since the remote view was turned on, not since
  the watcher connected; `remoteview.address` defaults to every interface, and `--remote-view` and `--cvar` turn it on from the start; a headless
  view with no `--frames` limits itself to `view.frameLimit` (`src/view/viewframe.cpp:484`), which the plan had only named as a gap; a captured frame is read as soon as the GPU has
  finished it (`RhiDevice::fenceSignaled`, `src/rhi/rhidevice.h:69`; `Renderer::frameContextDone`), a frame later as a rule, where ADR 84's
  readback and the plan read it when its frame context came round, five frames later.
- Henrik watched it from his phone, over Tailscale too (reported 2026-11-15). Not verified when this was written: a run with a window, and
  the CI job.
