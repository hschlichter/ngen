# 84. Frame images in Tracy only, from the render tools

Date: 2026-11-09

## Status

Accepted

Extends [ADR 81](0081_tracy_as_second_sink.md), which stays Accepted, with one exception to [ADR 82](0082_detailed_profiling.md)'s
"everything goes through the profile system": frame images go to Tracy only.

## Context

Tracy can show a small image of each frame under its frame ruler, so a spike in the timeline can be matched with what was on screen. The engine
had screenshots (`src/rendertools`), but a screenshot waits for its frame's fence, which every frame would make a stall.

## Decision

Locked with Henrik on 2026-11-09:

- **Tracy only.** The built-in collector keeps no images: they would dominate its memory, and agents and the windows already have screenshots
  (`screenshot`, `--screenshot`). Images are made only while a Tracy server is connected and `profile.tracy.frameImages` is on (default on).
- **In `src/rendertools`**, which owns screenshots and the readback path, as part of its render extension: the presented image is blitted to
  320 pixels wide and read back in its own frame's command buffer, then handed to Tracy at that slot's next fence, so nothing waits.
- **The presented image, UI included**, as the viewer saw it: a frame image is for recognising the moment, not for comparing pixels.
- **An image is placed by the main thread's frame** that built the snapshot (`profile::frameMarks`), carried in the render tools' snapshot data.
  Tracy places an image by an offset from its own frame count, so `frameMark` moves Tracy's count and the profiler's under one lock that
  `frameImage` also takes.

Rejected: images in the built-in collector (memory, and screenshots already serve agents); reading the backbuffer back at full size each frame
(the screenshot path's stall); placing images by the render thread's frame (Tracy's frames are the main thread's marks).

## Consequences

- Easier: a spike in a Tracy capture shows what was on screen; nothing changes without a server.
- Harder: the frame mark takes a lock every frame; with a server, the hand-off costs about 0.15 ms a frame on the render thread (Tracy copies
  each 230 KB image), and each frame slot holds about 476 KB.
- Harder: agents cannot see these images; they take a screenshot instead.

## Verification

- Sources: [plan_tracy_frame_images.md](../plan_tracy_frame_images.md), `src/rendertools/README.md`, `src/profile/README.md` ("Tracy").
- Code:
  - `src/profile/profile.h:175` (`frameMarks`), `:179` (`frameImage`); `src/profile/profile.cpp:589` (the counter moved under the lock in
    `frameMark`), `:703` (`frameImage`).
  - `src/rendertools/frameimage.h:18` (`FrameImages`); `src/rendertools/frameimage.cpp:25` (`record`), `:83` (`afterFence`).
  - `src/rendertools/rendertools.cpp:61` (the setting and frame number put in the snapshot), `:153` (recorded while connected).
- Differs from plan: the frame number travels in `RenderToolsFrame`, not `RenderSnapshot`; the shared lock was added after Tracy failed the
  first capture ("Multiple frame images were sent for a single frame"); targets are made on first use, which the plan had deferred.
