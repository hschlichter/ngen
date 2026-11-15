# Remote view: ngen-view's image in a browser

**Status. In progress.** Built and checked headless against Firefox on this machine (2026-11-15), and watched by Henrik from his phone,
over Tailscale too (reported 2026-11-15). Open: a run with a window, the CI job's first run. Results are under "Built" and "Results".

The decisions below were reviewed by Henrik on 2026-11-15; each says whether it is locked. Claims about the code are marked **observed** (read in
the source, with the place) or **assumed**; claims about libraries and browsers name their source, or are listed under "Open questions" when no
primary source confirmed them.

## Current state

ngen is developed remotely: coding agents run ngen-view on the development machine, and the only way to show a person what it draws is a PNG
(`screenshot`, `--screenshot`). There is no live image outside the machine's own display.

What the engine has that this builds on:

- **Joining the frame.** Code outside the renderer's core joins `Renderer::render` as a `RenderExtension` (observed:
  `src/renderer/renderextension.h:62`, hooks in `src/renderer/README.md`, "Extensions"). ngen-view adds three, in order: `RenderTools`,
  `GizmoRender`, `UiRender` (observed: `src/apps/view.cpp:402`). An extension's main-thread data rides in `RenderSnapshot::extensions`.
- **A per-frame readback that doesn't stall.** `FrameImages` blits the presented image to a small target and copies it to a `GpuToCpu` buffer in
  the frame's own command buffer at `afterExecute`, then reads it at `afterFence` of that frame context, so nothing waits (observed:
  `src/rendertools/frameimage.cpp:25`, `:84`; [ADR 84](adr/0084_tracy_frame_images.md)). Screenshots are the other path and wait for their frame
  in `afterSubmit`, which every frame would make a stall.
- **When `afterFence` runs.** Once per `render`, for the frame context about to be reused (observed: `src/renderer/renderer.cpp:953`). There are as
  many frame contexts as swapchain images (observed: `src/renderer/renderer.cpp:216`), five on this machine (observed:
  [plan_tracy_frame_images.md](plan_tracy_frame_images.md), "Built"). Inferred: an image read this way is as many frames old as there are frame
  contexts, about 83 ms at 60 Hz with five. The RHI has no call that asks a fence without waiting (observed: `src/rhi/rhidevice.h:66`).
- **Backbuffer format.** A run without a window gets a UNORM backbuffer, a window sRGB (observed: `docs/roadmap.md:158`). `FrameImages` picks its
  target's format to match, so the bytes read back are what was shown (observed: `src/rendertools/frameimage.cpp:35`).
- **Headless.** `SDL_VIDEODRIVER=offscreen` runs ngen-view without a window: the same renderer, swapchain and UI, on a surface nothing
  displays, 2560 × 1440 by default (observed: `src/log/README.md:71`). Agents' runs end by `--frames`; a run that is watched has none and goes on
  until it is stopped. Not known, and read in step 3: how fast a headless run presents when nothing limits it.
- **RPC** is JSON-RPC over TCP on loopback, ports chosen by the OS, no authentication (observed: `src/rpc/core/rpcsocket.cpp:21`,
  `src/rpc/README.md`, "Known gaps"). Another device can't reach it, by design.
- **Cvars** belong to the main thread; what another thread needs is handed to it (observed: `src/cvar/README.md`, "Threads").
- **Tooling is compiled out of `gamerelease`** by `NGEN_INTROSPECTION` (observed: `build.cpp:64`, `src/apps/view.cpp:510`).
- **Dependencies.** Submodules compiled by `ngen-build` (Tracy's client, meshoptimizer, Dear ImGui: observed `build.cpp:114`, `:422`), one
  submodule built outside it into `external/openusd_build` (OpenUSD), and system packages through `pkg-config` (SDL3: observed `build.cpp:34`).
  CI builds in an Arch container and caches the OpenUSD build by its commit (observed: `.github/workflows/build.yml:75`).

This machine, observed 2026-11-15: AMD Radeon 890M on Mesa RADV, no NVIDIA hardware. RADV reports `VK_KHR_video_encode_h264` and a queue with
`QUEUE_VIDEO_ENCODE_BIT_KHR`; nothing is tested. Installed: `openh264` 2.7.0, `x264` 0.165, `libavcodec` 63, `libva` 1.24, `libsrtp` 2.8.1,
`openssl`, `cmake`, Firefox. Not installed: `libdatachannel`, `libjuice`, `nasm`, `meson`, Chrome or Chromium, `tailscale`, `vainfo`.

## Goals

1. ngen-view's presented image, live, in a plain browser on another device on a private network, Safari on an iPhone first. No remote desktop
   software, no capture of the desktop or a window.
2. It works headless (`SDL_VIDEODRIVER=offscreen`), which is how agents run ngen-view. Headless is the first case, not a variant: every
   verification line below is run headless, and a window is the second run.
3. The render thread never waits for it, and with no browser connected it costs nothing.
4. A picture on the phone comes before latency and efficiency. Both are measured, neither is tuned in this plan.

## Decisions

**Locked** means Henrik decided it on 2026-11-15, in words or by accepting the recommendation ("overall sounds good"); the second kind is marked
*accepted* so it is easy to reopen.

1. **The term: remote view.** Locked. The roadmap already uses *remote view* (`docs/roadmap.md:115`, `:123`), and this is that feature: ngen-view
   watched from another device. The **remote view** is the feature; the **watch page** is the browser page; a **watcher** is a connected
   browser. Library `src/remoteview`, cvars `remoteview.*`, record `remoteview`. Inside the library the parts that deal in pictures keep
   *video* in their names (`VideoEncoder`, `VideoPacket`), because that is what they are. Two roadmap items name "a remote view" as their
   trigger (a client-side asset cache, asset browsers listing from the server). Neither is triggered here: ngen-view still runs beside its
   asset server, and only pictures leave the machine.

2. **Where it sits on the roadmap.** Accepted. "Networking, remote connections" stays parked until step 8 (`docs/roadmap.md:45`): that entry is
   about the engine's own protocols (assets, RPC) reaching other machines. The remote view is a development tool that sends pictures one way,
   and goes under "Tools, build and CI" as its own item.

3. **WebRTC library: libdatachannel.** Accepted. MPL 2.0, C++17, with an H.264 RTP packetizer (`rtc::H264RtpPacketizer`), sender reports
   (`rtc::RtcpSrReporter`), retransmission (`rtc::RtcpNackResponder`) and a keyframe request handler (`rtc::PliHandler`) (source: its
   `include/rtc/*.hpp` on master, version 0.24.6). Its submodules are libjuice (MPL 2.0), usrsctp (BSD 3-Clause), libsrtp (BSD 3-Clause) and plog
   (MIT); the TLS library is external, OpenSSL by default. usrsctp is always built, used or not: there is no option to leave data channels out.
   Alternatives: Google's libwebrtc (the reference, but its own build system and hundreds of megabytes: out of proportion); GStreamer's
   `webrtcbin` (a framework and its plugin set as a dependency); Pion (Go). libdatachannel, because it is the only one a C++ program links as an
   ordinary library, and MPL 2.0 sits with ngen's MIT licence as long as changes to its own files are published.

4. **How libdatachannel is built: a submodule, built once with CMake (a).** Accepted.
   - **(a) Submodule, built once with CMake into `external/libdatachannel_build`**, static, as OpenUSD is built into `external/openusd_build`:
     `-DBUILD_SHARED_LIBS=OFF -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON`, OpenSSL from the system. A fresh clone runs one more command;
     CI gains a cached job keyed by the submodule's commit.
   - **(b) System packages**: `libdatachannel` 0.24.5 is in Arch's `extra` (depends on `openssl`, `libjuice`, `libsrtp`), found through
     `pkg-config` as SDL3 is. Nothing to build, and CI's Arch container installs it. The version is the distribution's, not ours.
   - **(c) Compiled by `ngen-build`** from the submodule, as Tracy's client is. Four C and C++ libraries with configure-time headers: a large
     build description to write and keep in step with upstream.

   (a), because dependencies are vendored as submodules and OpenUSD set the pattern for one that brings its own build. (b) stays the cheap
   path if a second "build this first" step turns out to be too much.

5. **Encoder for the first version: OpenH264, in software.** Accepted.
   - **OpenH264** (Cisco, BSD 2-Clause): made for real-time use, produces Constrained Baseline, takes I420, a small C interface
     (`WelsCreateSVCEncoder`, `ISVCEncoder::EncodeFrame`, `ForceIntraFrame`). 2.7.0 is installed here.
   - **x264**: GPL. It can't be linked into an MIT-licensed engine without relicensing the result. Rejected.
   - **libavcodec**: reaches VA-API and Vulkan Video through one interface, but it is FFmpeg as a dependency (LGPL or GPL by configuration) for
     one codec. Rejected for the first version.
   - **Vulkan Video** (`VK_KHR_video_encode_h264`) directly: the end state on this machine, since the image never leaves the GPU. It needs new RHI
     surface (video session, picture buffers, a video queue) and an optional extension, which needs approval under the Vulkan 1.3 baseline. Too
     much before a first picture.

   OpenH264 now and Vulkan Video as the named follow-up. Vendored as libdatachannel is: a submodule built into `external/openh264_build` with
   its own Makefile (`nasm` must be installed for the assembly).

   H.264's patent licence is set aside for now (Henrik, 2026-11-15). For the record: Cisco covers the fees only for the binary it distributes
   itself, not for a build from source (openh264.org FAQ). The remote view is kept out of `gamerelease` in any case (decision 9).

6. **H.264 stays, though Safari no longer requires it.** Accepted. Browsers must implement both VP8 and H.264 Constrained Baseline (RFC 7742), and Safari
   has had VP8 since 2019 (webkit.org, "On the road to WebRTC 1.0, including VP8"). VP8 through libvpx is the alternative.
   H.264, because it is what this machine's GPU encodes, so the software encoder is replaced later without changing what the browser
   receives, and Safari decodes it in hardware. Parameters: Constrained Baseline, level 3.1 (enough for 1280 × 720 at 30), `profile-level-id=42e01f`,
   `packetization-mode=1` (libdatachannel fragments large NAL units as FU-A, which needs mode 1), parameter sets in the stream before every
   keyframe (RFC 7742 forbids them in the SDP).

7. **Signalling: one HTTP request.** Accepted. The watch page creates an offer with one receive-only video transceiver and POSTs the SDP to `/offer`; the
   engine answers with its SDP in the response. This is the exchange WHEP uses. No trickle: the engine's candidates are its own addresses, known
   at once, and the browser's arrive in the offer or are learned from its connectivity checks. Reconnecting is a new POST.
   Alternative: libdatachannel's `rtc::WebSocketServer`, a persistent channel that later input could share. But libdatachannel has no HTTP
   server, so something must serve the watch page anyway, and with HTTP signalling that one listener does both and libdatachannel is built
   without WebSocket. Input, when it comes, travels in a WebRTC data channel, not in the signalling.

8. **The listener.** Locked: no authentication. A small HTTP/1.1 listener of our own in `src/remoteview` (`GET /`, `POST /offer`, `POST /report`, `GET /time`; one request per
   connection, bodies capped at 64 KB). Not `rpccore`'s server, which is loopback only on purpose.
   - Off by default: `remoteview.enabled`.
   - Bound to one address, `remoteview.address`, default `127.0.0.1`. To watch from the phone it is set to this machine's Tailscale address. It is
     never bound to every interface unless the value says `0.0.0.0`. libdatachannel's ICE binds the same address (`Configuration::bindAddress`).
   - A fixed port, `remoteview.port`, default 8730, so the phone's bookmark keeps working.
   - No authentication: what is exposed is the picture, read-only, to whoever reaches that address. Whether input needs any is decided when
     input is planned (Deferred).

9. **Where the code lives.** Accepted. A new library `src/remoteview`, linking `renderer`, `log`, `profile`, `cvar`, libdatachannel and the encoder; nothing in
   `renderer` or `rendertools` includes it. Its capture is its own render extension, `RemoteViewCapture`, added after `UiRender`.
   Alternative: capture inside `rendertools` beside `FrameImages`, since ADR 84 placed frame images there as the owner of "screenshots and the
   readback path". A separate extension, because `rendertools` is instrumentation that stays free of third-party libraries, and the blit and copy
   are thirty lines that read better beside their consumer than behind a shared abstraction made for two users.
   The library is built and linked only with `NGEN_INTROSPECTION`, so `gamerelease` contains neither it nor its dependencies.

10. **What the watcher sees: the presented image, UI included**. Accepted. It is taken as Tracy's frame images are. The use is watching an agent drive the engine, and
    the windows are part of that. A frame that takes a screenshot hides the UI for that frame, and the video shows that frame without it.
    Alternative: a `remoteview.ui` value. Deferred until someone wants the picture without the UI.

11. **Colour: converted on the CPU, BT.709 limited range, said in the stream.** Accepted. The video thread converts the display-encoded RGBA to I420.
    The encoder writes the matrix, primaries, transfer and range into the H.264 VUI (`SSpatialLayerConfig::bVideoSignalTypePresent` and its
    fields). What browsers assume when the stream doesn't say is not settled by any source found (Chromium's code points to both BT.601 and
    BT.709), so the stream says, and the test pattern's colour bars are read back in the browser to check that it is honoured (Verification).
    Alternative: conversion in a compute pass, so the readback is 1.5 bytes a pixel, not 4. Deferred; it disappears with Vulkan Video.

12. **The test pattern stays, as a source.** Accepted. `remoteview.source` is `frame` (the presented image) or `pattern` (colour bars, a moving bar, and the
    send time drawn as a row of black and white cells, generated on the video thread). Step 2 uses it to bring up the library, the build and the
    browser with no renderer involved; afterwards it is how latency is measured and how a black picture is told from a broken stream.
    Alternative: a separate test program. Rejected: a second executable to check a layer the library's links already keep.

13. **Capture hands over at the slot's next fence, as `FrameImages` does.** Accepted. And the delay that adds is measured. If it is more than the encode
    and network time together, the follow-up is a call that asks a fence without waiting, so a frame is read as soon as the GPU has it. That
    is a change to the RHI's interface, so it is not made before the number says so.

14. **Exceptions stop at the library's edge.** Accepted. libdatachannel's C++ interface throws (`setRemoteDescription`, `setLocalDescription`). Every call
    into it is in `videosender.cpp`, caught there, and returned as `std::expected<…, std::string>`, the form `rpccore` already uses.

## Scope

**In**

- `src/remoteview`: capture, colour conversion, the encoder interface with one OpenH264 encoder, the sender, the listener, the watch page.
- One watcher at a time. A second offer replaces the first.
- Video only, one way, 1280 × 720 at 30 frames a second by default, both settable while running.
- Connect, disconnect and reconnect without restarting ngen-view.
- Records, RPC methods, console commands, a window, log events, the library's README, glossary entries.
- libdatachannel and OpenH264 as dependencies, with what a fresh clone and CI do.
- Measurements: frame time with it off, on, and watched; latency from capture to display.

**Out**

- Input from the browser, audio, more than one watcher, more than one image at a time.
- A hardware encoder; conversion on the GPU.
- Adapting bitrate to the network (the bitrate is a fixed, settable value).
- STUN and TURN servers; reaching it from the public internet; HTTPS in the listener.
- The remote view in `gamerelease`, in `ngen-introspect` or in the asset server.

## Steps

Each step ends with something to look at.

### 1. Dependencies

- Submodules `external/libdatachannel` and `external/openh264`, each pinned to a release tag.
- Built once into `external/libdatachannel_build` and `external/openh264_build` (git-ignored), by the commands in the root `README.md` beside
  "Building OpenUSD".
- `build.cpp`: a `remoteview` static library, inside the same condition that defines `NGEN_INTROSPECTION`:

  ```cpp
  auto remoteview =
      cxx::static_library("remoteview")
          .sources(glob({.include = "src/remoteview/*.cpp"}))
          .public_include({"src/remoteview"})
          .include({
              "src/log",
              "src/profile",
              "src/rhi",
              "external/libdatachannel_build/include",
              "external/openh264_build/include",
          })
          .lib_search("external/libdatachannel_build/lib")
          .lib_search("external/openh264_build/lib")
          .link(renderer)
          .link(logLib)
          .link(profile)
          .link(cvar);
  ```

  The exact static libraries to name (`datachannel`, `juice`, `srtp2`, `usrsctp`, `openh264`, and `ssl`, `crypto` from the system) are read
  from what the two builds install, not guessed here.
- `.github/workflows/build.yml`: one cached job for both builds, keyed by the two submodule commits, as the `openusd` job is.
- Shows: `./ngen-cli build` links ngen-view with both libraries in debug and release, and `gamerelease` builds without them.

### 2. The pattern in a browser, no renderer

New files in `src/remoteview`:

- `videotypes.h`: what moves between the parts.

  ```cpp
  // One image on its way to the encoder: RGBA as displayed, top row first.
  struct VideoImage {
      std::vector<uint8_t> rgba;
      uint32_t width = 0;
      uint32_t height = 0;
      uint64_t frame = 0;
      uint64_t captureNs = 0; // steady clock, when the frame that made it was recorded
  };

  // Planar Y'CbCr 4:2:0, BT.709, limited range.
  struct VideoPlanes {
      std::vector<uint8_t> y;
      std::vector<uint8_t> cb;
      std::vector<uint8_t> cr;
      uint32_t width = 0;
      uint32_t height = 0;
  };

  // One encoded frame: an H.264 access unit with start codes.
  struct VideoPacket {
      std::vector<uint8_t> data;
      bool keyframe = false;
      uint64_t captureNs = 0;
  };

  struct VideoSettings {
      uint32_t width = 1280;
      uint32_t height = 720;
      uint32_t framesPerSecond = 30;
      uint32_t bitrate = 4'000'000;
  };
  ```

- `videoencoder.h`: the interface a hardware encoder that takes pictures from the CPU would also implement.

  ```cpp
  class VideoEncoder {
  public:
      virtual ~VideoEncoder() = default;
      virtual auto name() const -> const char* = 0;
      virtual auto start(const VideoSettings& settings) -> std::expected<void, std::string> = 0;
      // `keyframe`: the watcher has nothing to decode against (it just connected, or asked).
      virtual auto encode(const VideoPlanes& planes, bool keyframe, VideoPacket& packet) -> std::expected<void, std::string> = 0;
      virtual auto stop() -> void = 0;
  };

  auto makeVideoEncoderOpenH264() -> std::unique_ptr<VideoEncoder>;
  ```

  Vulkan Video would not implement this: it replaces capture and encoder together and hands `VideoPacket`s to the sender. The seam that
  survives is the packet, and that is why the sender takes nothing else.

- `videoencoderopenh264.cpp`: `SEncParamExt` with `iUsageType = CAMERA_VIDEO_REAL_TIME`, one spatial layer, Constrained Baseline at level 3.1,
  frame skipping off, no periodic keyframes (they come on connect and on request), the colour description of decision 11.
- `videoconvert.h/.cpp`: `convertToPlanes(const VideoImage&, VideoPlanes&)`.
- `videopattern.h/.cpp`: `drawPattern(VideoImage&, uint64_t nowNs)`.
- `videosender.h/.cpp`: the only file that includes libdatachannel.

  ```cpp
  enum class VideoWatcherState : uint8_t {
      None,
      Connecting,
      Connected,
  };

  class VideoSender {
  public:
      // The answer to a watch page's offer. Replaces the watcher there was.
      auto answer(const std::string& offerSdp, const std::string& bindAddress) -> std::expected<std::string, std::string>;
      auto send(const VideoPacket& packet) -> bool;
      auto state() const -> VideoWatcherState;
      // True once after the watcher asked for a keyframe, or connected.
      auto takeKeyframeRequest() -> bool;
      auto close() -> void;
  };
  ```

  The answer accepts the offered H.264 payload type whose format line has `profile-level-id=42e01f` and `packetization-mode=1`, and fails with a
  message when there is none.
- `videohttp.h/.cpp`: the listener of decision 8, on its own thread ("Remote View Http"), polling in slices of `profile::idleSlice` as `RpcServer`
  does.
- `videopage.cpp`: the watch page as one string. One `<video muted playsinline autoplay>`, a status line, plain JavaScript, no framework. Once
  a second it POSTs to `/report` what the browser knows (`getStats`: frames decoded and dropped, size; its user agent; with the pattern as
  source, the latency it reads off the picture, corrected by the clock difference it measures against `GET /time`).
- `remoteview.h/.cpp`: `RemoteView`, what ngen-view owns.

  ```cpp
  class RemoteView {
  public:
      auto init() -> void;
      // Main thread, once a frame: reads the remoteview.* cvars, starts or stops the listener and the video thread when they changed, logs what
      // happened since the last frame, and updates status().
      auto update() -> void;
      // Main thread: whether the frame being built should be captured (a watcher is connected, the source is the frame, and one is due).
      auto wantsFrame() const -> bool;
      // Render thread: a captured image. Replaces one the video thread hasn't taken yet, and counts it as dropped.
      auto submit(VideoImage&& image) -> void;
      auto status() const -> const RemoteViewStatus&;
      auto requestKeyframe() -> void;
      auto disconnect() -> void;
      auto shutdown() -> void;
  };
  ```

  Its thread ("Remote View Encode") sleeps until an image arrives or, with the pattern as source, until the next frame is due; converts, encodes,
  sends. One image waits at most; a newer one replaces it. Nothing queues, so latency can't grow.
- `remoteviewcvars.h/.cpp`: `remoteview.enabled` (Bool, false), `remoteview.source` (Enum: frame, pattern), `remoteview.address` (see Open questions: cvars have no
  string type), `remoteview.port` (Int, 8730), `remoteview.height` (Int, 720, 180–2160), `remoteview.fps` (Int, 30, 1–60), `remoteview.bitrate` (Int, kilobits a
  second, 4000).
- `src/apps/view.cpp`: a `RemoteView` made beside the renderer, `update()` in the main loop, `shutdown()` at exit, all under
  `NGEN_INTROSPECTION`.
- Shows: with `remoteview.source pattern`, a desktop browser on this machine opens `http://127.0.0.1:8730/` and plays the pattern.

### 3. The real image

- `remoteviewcapture.h/.cpp`: `RemoteViewCapture : RenderExtension`, added after `UiRender` in `view.cpp`.

  ```cpp
  // The remote view's data for one frame, in RenderSnapshot::extensions.
  struct RemoteViewRequest : RenderExtensionData {
      bool capture = false;
      uint32_t height = 0;
  };

  class RemoteViewCapture : public RenderExtension {
  public:
      explicit RemoteViewCapture(RemoteView& output);
      // Main thread, where RenderTools::fillSnapshot is called.
      auto fillSnapshot(RenderSnapshot& snapshot) -> void;

      auto init(const RenderExtensionContext& context) -> bool override;
      auto beginFrame(Renderer& renderer, RenderSnapshot& snapshot, RenderFrame& frame) -> void override;
      auto afterFence(Renderer& renderer, uint32_t frameContext) -> void override;
      auto afterExecute(Renderer& renderer, RhiCommandBuffer* cmd, const RenderFrame& frame) -> void override;
      auto destroy() -> void override;
  };
  ```

  - `fillSnapshot` (main thread, `src/apps/view.cpp:600`): `capture = output.wantsFrame()`, the height from `remoteview.height`.
  - `afterExecute`: when `capture`, the blit from the backbuffer to this slot's target and the copy to its `GpuToCpu` buffer, with the barriers
    `FrameImages::record` uses. The target is `remoteview.height` high, as wide as the backbuffer's shape makes it, both rounded down to even
    numbers; sRGB when the backbuffer is. Targets and buffers are made on first use and remade when the size changes.
  - `afterFence(slot)`: when the slot recorded an image, one copy out of the mapped buffer into a `VideoImage`, then `output.submit`.
  - Memory: at 1280 × 720, 3.7 MB for a target and 3.7 MB for its buffer per slot, 37 MB with five slots, only once a watcher has connected.
- Shows: with `remoteview.source frame`, the browser plays what ngen-view presents, with and without a window.

### 4. Visibility

The records, methods, commands, window and log events under "Visibility" below, `src/remoteview/README.md` (what it is, where in the frame capture
sits, the threads, the rules: free when off, nothing waits, one image at most), the root `README.md` link and usage, `REFERENCE.md`,
`GLOSSARY.md` (*remote view*, *watch page*, *watcher*).

### 5. Measure and record

- The measurements under "Verification", their numbers written into this plan.
- An ADR for the decisions above that hold, a row in `docs/adr/README.md`, the roadmap item, this plan's status.

## Built

Where the code differs from the steps above (2026-11-15):

- **The engine offers, the page answers**, in two requests (`POST /offer` returns `{session, sdp}`, `POST /answer` takes the page's SDP), not
  one request with the page's offer (decision 7). libdatachannel's documented path is to offer, and the browser then picks from the one codec
  the engine names. Still plain HTTP on the one listener, still no trickle.
- **A silent watcher is dropped after five seconds.** Killing the browser left the engine capturing, encoding and sending for it: the
  connection itself had not noticed after nine seconds. The watch page's once-a-second report is the sign of life now
  (`RemoteViewWatcherSilent`). A phone that puts the page away is dropped the same way and connects again when it comes back.
- **A page that is replaced stops.** Two pages open at once took the watcher from each other every two seconds (found when Firefox restored a
  tab). Each offer has a session number; a page told that another has it closes its connection and says so.
- **The page reconnects on the engine's word**, not only the browser's: `remoteview-disconnect` took nine seconds to be noticed by Firefox,
  and takes about three now.
- **The capture's targets and buffers are given back** five seconds after the last capture, so an unwatched view holds none.
- **Frame counts are since the remote view was turned on**, not since the watcher connected.
- **`remoteview.grab`** (a third method, and `POST /frame`): the frame the watch page shows, as a PNG. It is how an agent sees what the
  watcher sees, since nothing here drives Firefox's window.
- **The status is its own header** (`remoteviewstatus.h`), so the window reads it without the library's threads or dependencies, and
  `gamerelease` (where the window says there is no remote view) links nothing of libdatachannel, OpenH264 or OpenSSL.
- **The encoder's level is left to OpenH264** (not fixed at 3.1), since `remoteview.height` goes up to 2160; four slices, so it uses four
  threads.
- **`RemoteViewWatcherConnected` carries the address only**; the user agent arrives with the page's first report and is in the record.
- **The console commands are hyphenated**, as the others with two words are: `remoteview-keyframe`, `remoteview-disconnect`, `remoteview-grab`;
  the commands are registered from `src/view/remoteviewcommands.cpp` by `view.cpp` under `NGEN_INTROSPECTION`.
- **A headless run with no `--frames` limits itself** to `view.frameLimit`, 60 frames a second (`ViewFrame::limitFrameRate`), added after the
  first results named the unlimited rate as a gap. Debug, three cubes: 508 frames a second and the GPU 97 % busy without the limit, a 16.67 ms
  frame and 21 % with it; `--frames=600` still takes 1.3 s. It makes decision 13's delay real: with the frame as the source, capture to send is
  92 ms at 60 frames a second against 17 ms unlimited. The pattern's latency is the same either way (26 to 29 ms).
- **The frame is read as soon as the GPU has it** (2026-11-15, the follow-up below): `RhiDevice::fenceSignaled`, `Renderer::frameContextDone`,
  and `RemoteViewCapture::afterFence` asking every frame. Capture to send at 60 frames a second: 21 ms, from 92 (debug, three cubes,
  headless, Firefox, 386 frames, none replaced, no warnings with `--fail-on-validation`).
- **Start-up flags** (the gap Visibility named): `--cvar <name>=<value>` for any cvar, and `--remote-view[=<address>[:<port>]]`, which sets the
  address, the port and `remoteview.enabled`. An unknown cvar or a bad value stops the program, so `--remote-view` in `gamerelease` is an error.
- **`remoteview.address` defaults to `0.0.0.0`**, not `127.0.0.1` (decision 8), by Henrik's choice on 2026-11-15: the loopback default gave a
  page and no video. The record's `urls` and the log give the watch page at each of the machine's addresses.
- **`cvar::String`** was added first (`src/cvar`), with its editor in the Cvars window and its JSON in the `cvars` record.
- **`127.0.0.1` serves the page but no video**: Firefox does not connect to a loopback candidate. `0.0.0.0` and the machine's own address
  both work.

## Visibility

- **Records**: `remoteview`: enabled, source, the listener's address, port and URL; the watcher's state, address and user agent; width, height,
  frames a second, bitrate; the encoder's name; counts since the watcher connected (captured, replaced before encoding, encoded, sent,
  keyframes, keyframe requests received); times of the last second (hand-off copy, conversion, encode, capture to send); what the watch page
  last reported (frames decoded and dropped, its size, the latency it measured, when); the last error. `cvars` lists the `remoteview.*` values.
- **RPC methods and console commands**: `cvar.set` and `cvar` for every setting. New: `remoteview.keyframe` / `remoteview keyframe` (the next frame is a
  keyframe) and `remoteview.disconnect` / `remoteview disconnect` (drops the watcher), registered in a new `src/view/remoteviewcommands.cpp` in `viewcommands`.
- **Window**: "Remote View" (`src/ui/remoteviewwindow.h/.cpp`, under the menu where Counters is): the URL to open, the watcher's state, the `remoteview`
  record's counts and times, editors for the `remoteview.*` values, and one line that says where it sits: captured after the UI is drawn, read back
  when the frame's slot comes round, encoded and sent on the video thread.
- **Log**, category `RemoteView`: `RemoteViewListening` (address, port, URL), `RemoteViewStopped`, `RemoteViewEncoderStarted` (name, size, rate, bitrate),
  `RemoteViewWatcherConnected` (address, user agent), `RemoteViewWatcherDisconnected` (seconds, frames sent), and warnings for an offer without a usable
  H.264 format, an encoder failure and a listener that couldn't bind.
- **Profiler**: the "Remote View Encode" and "Remote View Http" threads as tracks, zones for conversion, encode and send; `RemoteViewCapture`'s hooks are timed as
  every extension's are (`RemoteViewCapture.afterFence`, `RemoteViewCapture.afterExecute`).
- **Gaps, named**: no start-up flag for `remoteview.enabled` unless cvars can already be set from the command line (to check in step 2; if not, it
  is a gap of the cvar system and is listed, not patched here). `ngen-introspect` shows the `remoteview` record as it shows any record, with no
  window of its own.

## Verification

Every line is run headless (`SDL_VIDEODRIVER=offscreen`, as the `run-headless` skill says) on `assets/three_cubes.usda` unless it names another
scene, with Firefox on this machine as the watcher. The lines marked *also with a window* are repeated with one. What an agent checks:

- **Off costs nothing.** `remoteview.enabled` false: no `RemoteView` log event, no thread named "Remote View", no `remoteview.capture` allocation in the `memory`
  record, `RemoteViewCapture.afterExecute` and `.afterFence` under 0.001 ms in the `zones` record.
- **On, unwatched, costs nothing in the frame.** `remoteview.enabled` true, no watcher: `RemoteViewListening` logged once; still no `remoteview.capture`
  allocation and no blit zone; frame time within noise of off.
- **The pattern arrives.** `remoteview.source pattern`, a browser on this machine connected: `RemoteViewWatcherConnected` logged; in the `remoteview` record,
  sent rises by 30 ± 1 a second and the watch page's reported frames decoded rises with it; the page reports 1280 × 720.
- **The colours are right.** The watch page reads the pattern's colour bars from a decoded frame; each is within 6 of 255 of what was drawn.
  (This is the check that the stream's colour description is honoured.)
- **The frame arrives.** `remoteview.source frame`: a frame grabbed from the watch page shows the three cubes and the UI, compared by eye with
  `--screenshot --show-ui` of the same run; captured, encoded and sent rise together. *Also with a window.*
- **A headless run that is watched.** No `--frames`: the run goes on, the watcher connects ten minutes in, and the picture arrives. The
  headless frame rate is written here; captured still rises by 30 ± 1 a second whatever it is. If nothing limits the rate and the GPU runs
  flat out for a picture sent 30 times a second, that is named as a gap with its number, not fixed in this plan.
- **Headless and a window look the same.** The pattern's colour check above passes in both, and the same scene and camera give the same
  brightness in the watch page, though the backbuffer is UNORM in one and sRGB in the other.
- **Nothing queues.** With the encoder slowed (a debug sleep of 100 ms, for the test only), replaced-before-encoding rises and capture-to-send
  stays under 250 ms.
- **Reconnect.** The page closed and reopened three times: three `RemoteViewWatcherConnected`, three `RemoteViewWatcherDisconnected`, frames decoded
  rising after each, ngen-view never restarted. `remoteview disconnect` does the same from the engine's side.
- **Settings while running.** `cvar remoteview.height 360`: the page reports 640 × 360 within two seconds, no reconnect needed, or one logged
  reconnect if the browser requires it (which of the two is recorded here).
- **The render thread doesn't wait.** Release, Sponza, same camera, three runs each of off / on unwatched / watched: median frame time and
  `WaitFence` from the profile records, written here. Watched, `RemoteViewCapture.afterFence` (the copy) is under 1 ms; the frame time rises by no
  more than that plus the blit's GPU time.
- **Latency**, with the pattern as source, as the watch page measures it: in Firefox on this machine, and reported by the phone. *Also with a
  window*, where the delay of decision 13 is largest.
- **Clean.** No warnings or errors in the log, `--fail-on-validation` passes with a watcher connected.

What Henrik checks (locked: the phone is his to verify), and what makes it readable to an agent afterwards:

- Safari on the iPhone over Tailscale shows the picture. The `remoteview` record then holds a watcher whose user agent names iPhone, with frames
  decoded rising and a latency. Until that has been reported back, the phone is unverified.

## Results

2026-11-15, this machine (Radeon 890M, RADV), headless, Firefox 160 headless as the watcher on the machine's LAN address. Debug on
`three_cubes.usda` unless a line says release.

| Check | Result |
|---|---|
| Off costs nothing | **Passed.** No `RemoteView` event, no thread, no `remoteview.capture` allocation; release Sponza: frame 3.655 ms CPU, 3.839 ms GPU; `RemoteViewCapture.afterExecute` and `.afterFence` 0.0001 ms each. |
| On, unwatched | **Passed.** `RemoteViewListening` once; no allocation; release Sponza: 3.655 ms CPU, 3.840 ms GPU (the same). |
| The pattern arrives | **Passed.** 30 frames a second sent and decoded, 1280 × 720 reported by the page. |
| The colours are right | **Passed, with a note.** Red and green within 1 of 255 of what was drawn; blue up to 9 low (191 read as 182 to 189). BT.601 would move red by about 16, so the BT.709 description is honoured; the blue error is unexplained (Firefox's conversion is the suspect, not checked). The plan's "within 6" is not met for blue. |
| The frame arrives | **Passed headless.** `remoteview.grab` from Firefox shows the three cubes with the menu bar and status bar, and Sponza in release. Not run with a window. |
| A headless run that is watched | **Passed, shortened.** Runs without `--frames` were watched for up to a minute, the watcher connecting 5 to 35 s in; not ten minutes. Headless frame rate: about 500 a second on three cubes, 270 on Sponza in release, GPU 97 % busy. Captured stays at 30 a second. The gap this named (nothing limited a headless run's rate) is closed: see Built, `view.frameLimit`. |
| Headless and a window look the same | **Not run** (no window was opened on this machine's desktop). |
| Nothing queues | **Not shown.** At 2560 × 1440 in debug the encode thread still kept up (18 ms convert, 7 ms encode), so nothing was replaced; `replaced` stayed 0 in every run. The one-image mailbox is in the code (`RemoteView::submit`) but was never exercised. |
| Reconnect | **Passed.** Three `remoteview-disconnect`s: the page back in about 3 s each time, frames decoded rising after each, ngen-view never restarted. Firefox killed: the watcher dropped after 5 s. A second offer: the first page stops. |
| Settings while running | **Passed.** `remoteview.height 360`: the page shows 640 × 360 without a reconnect; one `RemoteViewEncoderStarted`. |
| The render thread doesn't wait | **Passed.** Release Sponza, watched: 3.55 to 3.64 ms CPU (within noise of off), 3.86 ms GPU (+0.02); `RemoteViewCapture.afterFence` 0.63 ms for each captured frame, 0.08 ms averaged over all frames; `WaitFence` 0.0015 ms (0.0009 off). |
| Latency | **Pattern, Firefox on this machine: 19 ms (release), 23 to 46 ms (debug).** With the frame as the source, capture to send is 23 ms in release at 270 frames a second, of which about 18 ms is the wait for the frame context to come round (decision 13); at 60 frames a second that wait would be about 83 ms. Not measured on the phone. |
| Clean | **Passed.** No warnings or errors in the log from the remote view, `--fail-on-validation` passes with a watcher connected. (Sponza's own `cull.instances` growth warnings at load are there with the remote view off.) |
| `gamerelease` | **Passed.** Builds; the binary holds no symbol of libdatachannel or OpenH264 and loads neither `libssl` nor `libcrypto`. |
| Memory given back | **Passed.** Ten `remoteview.capture` allocations while watched, none 12 s after the watcher left. |
| The window | **Passed.** `window remoteview on` and a screenshot with `--show-ui` show it with the live numbers. |
| CI | **Not run.** The `remoteview` job in `.github/workflows/build.yml` is written after the OpenUSD job and has never executed. |
| The phone | **Passed (Henrik, 2026-11-15).** Reported as working from his phone, and over Tailscale. The browser, the record's contents and the latency were not reported. |

## Open questions

1. **Does Safari on iOS connect from a page served over plain `http://` on a non-loopback address?** `RTCPeerConnection` is not restricted to
   secure contexts in the specification or in WebKit's interface definition, but no primary source confirms the connection completes there.
   If it doesn't: `tailscale serve` puts HTTPS with a real certificate in front of the listener for the page and the signalling, while the
   video still goes directly. That needs no change to the engine, so it is the fallback, not HTTPS in the listener.
   *Answered by Henrik's check on 2026-11-15: the phone connected from the plain `http://` page, with no fallback. This covers question 2 as well.*
2. **Safari's own candidates.** Without camera permission Safari hides its addresses behind `.local` names. The engine offers its plain
   address and should learn Safari's from its connectivity checks; whether libjuice does this without trouble is unconfirmed. This and
   question 1 are the two things only the phone can answer, which is why step 2 ends with a check on the phone, not step 5.
3. **Does Firefox on this machine decode H.264 over WebRTC?** Firefox is the only browser used locally (locked). It runs headless and the
   watch page's reports are the evidence; a frame is grabbed by the page itself (a canvas, POSTed with the report on request), since nothing
   here drives Firefox's window. Untested until step 2. If Firefox has no H.264 decoder, the local check falls back to the engine's own
   counters, and the phone becomes the first real watcher.
4. **`remoteview.address` needs a string, and cvars have none** (Bool, Int, Float, Enum, Vec3: `src/cvar/README.md`). Accepted: add
   `cvar::String`, as its own small change before step 2.
5. **Default port.** 8730 is arbitrary.

## Deferred / follow-ups

- **Vulkan Video encode** (`VK_KHR_video_encode_h264`). Trigger: the software encoder's CPU time, or the readback's, shows in the frame or
  starves the job workers. Needs approval for the extension and an RHI plan of its own.
- **Reading a frame as soon as the GPU has it** (a fence asked without waiting). Trigger: decision 13's measurement. **Done** on Henrik's go-ahead, 2026-11-15: see
  "Built". It was triggered by 92 ms from capture to send at 60 frames a second, about 75 ms of it the wait for the frame context.
- **Input from the watch page** over a data channel, carrying the console commands and RPC methods that exist. Trigger: wanting to move the
  camera from the phone. Brings the token of decision 8 with it.
  **Planned**: [plan_remote_control.md](plan_remote_control.md).
- **Conversion on the GPU.** Trigger: the conversion or the hand-off copy is the largest part of the video thread's time, and Vulkan Video is
  not near.
- **More than one watcher.** Trigger: two people watching one session.
- **Bitrate following the network.** Trigger: a picture that breaks up over a mobile connection.
- **A picture without the UI** (`remoteview.ui`). Trigger: someone wants it.
- **Audio.** Trigger: the engine has audio.
