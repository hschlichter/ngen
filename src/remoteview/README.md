# Remote view

The `remoteview` library: ngen-view's image as live video in a browser on another device, a phone first. It is for watching a session that runs
somewhere else, an agent's headless run included, without remote desktop software and without capturing a desktop or a window: the image is taken
from the GPU inside the engine, encoded as H.264 and sent over WebRTC. One watcher at a time, video only, one way.

It is tooling, as the RPC endpoint is: only ngen-view's `NGEN_INTROSPECTION` code uses it, so `gamerelease` links none of it and none of its
libraries. It is off until `remoteview.enabled` is set.

## Using it

```sh
./ngen-cli asset-server &
SDL_VIDEODRIVER=offscreen ./ngen-cli view assets/three_cubes.usda --remote-view          # or with a window
```

The log says where the watch page is (`remote view: watch page at http://192.168.2.223:8730/`); open that on the other device, or in a browser
on this machine. `--remote-view` listens on every interface; `--remote-view=<address>` or `--remote-view=<address>:<port>` keeps it to one of
this machine's addresses (its Tailscale address) and so to that network. A browser can't connect its video to a loopback address, so
`127.0.0.1` serves the page and nothing more.

The flag sets cvars, and a running view is turned on the same way: `cvar remoteview.enabled on` in the console,
`./ngen-cli introspect call view cvar.set '{"values": {"remoteview.enabled": true}}'`, or the Enabled box in the Remote View window
(Windows > Introspection), which shows the addresses and everything below.

## What is here

| What | Files | Does |
|---|---|---|
| The remote view | `remoteview.h/.cpp` (`RemoteView`), `remoteviewstatus.h` | owns the listener, the watcher's connection and the encode thread; driven by the main thread once a frame |
| Capture | `remoteviewcapture.h/.cpp` (`RemoteViewCapture`) | a render extension: the presented image scaled and read back, handed over without waiting |
| Types | `videotypes.h` | `VideoImage` (RGBA as displayed), `VideoPlanes` (Y'CbCr 4:2:0), `VideoPacket` (an H.264 access unit) |
| Conversion | `videoconvert.h/.cpp` | RGBA to Y'CbCr 4:2:0, BT.709, limited range |
| Encoder | `videoencoder.h`, `videoencoderopenh264.cpp` | the `VideoEncoder` interface and its one implementation, OpenH264 in software |
| Sender | `videosender.h/.cpp` (`VideoSender`) | WebRTC through libdatachannel: one peer connection, one H.264 track. The only file that includes libdatachannel |
| Listener | `videohttp.h/.cpp` (`VideoHttp`) | a small HTTP/1.1 server for the watch page and its requests |
| Watch page | `videopage.h/.cpp` | the one HTML page, as a string in the program |
| Test pattern | `videopattern.h/.cpp` | the second source: colour bars, a moving bar, and the time it was drawn |
| Cvars | `remoteviewcvars.h/.cpp` | `remoteview.*` |

Its commands and record are registered by `src/view/remoteviewcommands.cpp` (the `viewcommands` library), its window is
`src/ui/remoteviewwindow.cpp`.

## Where it sits in the frame

1. **Main thread, once a frame** (`RemoteView::update`, before the snapshot is built): reads the `remoteview.*` cvars, starts and stops the
   listener and the encode thread when they changed, logs what the other threads reported, refreshes the status the record and the window read.
   Then `RemoteViewCapture::fillSnapshot` asks whether a frame is due (a watcher is connected, the source is the frame, and 1/`remoteview.fps`
   seconds have passed) and, if so, puts a capture request in the `RenderSnapshot`.
2. **Render thread, `afterExecute`** of a frame that carries a request: the backbuffer, after the UI is drawn, is blitted to a target of the
   video's size and copied to a `GpuToCpu` buffer, in the frame's own command buffer. `RemoteViewCapture` is added after `UiRender` for that reason.
3. **Render thread, `afterFence`** of that frame context, when the renderer comes round to it again: the buffer is copied to a `VideoImage`
   and handed to `RemoteView::submit`. Nothing waited for the GPU: the renderer had waited for that context anyway.
4. **Encode thread** ("Remote View Encode"): converts the image to Y'CbCr, encodes it, hands the packet to the sender.
5. **libdatachannel's threads**: packets become RTP and leave.

The watch page's requests are served on the listener's thread ("Remote View Http").

## Rules

- **Free when off.** `remoteview.enabled` off: no listener, no thread, no request in the snapshot. On with no watcher: a listener and an
  idle thread, and still nothing in the frame: no blit, no target, no buffer. The targets and buffers are made when the first frame is
  captured and given back five seconds after the last.
- **Nothing in a frame waits.** The render thread's part is a blit and a copy on the GPU, and one `memcpy` of the image. Encoding and the
  network are on other threads.
- **One image at most.** The encode thread holds one image waiting. A newer one replaces it (the record's `replaced` counts them), so an
  encoder or a network that falls behind drops frames and the picture is never late by more than a frame.
- **One watcher.** A page that opens takes the place of the one that was watching, and that one stops and says so; they don't take it from
  each other in turn. A watcher whose page has reported nothing for five seconds is dropped (a closed browser, a phone put away): the
  connection itself notices much later, and every frame until then would be encoded for nobody.
- **The picture is the presented image, UI included**, as the person at the machine would see it. A frame that takes a screenshot hides the
  UI for that frame, and the video shows that frame without it.
- **libdatachannel stays in `videosender.cpp`.** Its interface throws; every call into it is there, caught there and returned as
  `std::expected`. Nothing else in the engine sees its types.
- **No authentication.** Whoever reaches the address sees the picture and nothing more: nothing is taken from the watcher but its report.
  That is why it is off until asked for, why it can be kept to one address, and why this is not the RPC endpoint, which stays on loopback.

## The watch page and the listener

The engine offers, the page answers:

| Request | Does |
|---|---|
| `GET /` | the watch page |
| `POST /offer` | a new connection for this page, replacing the watcher there was: `{session, sdp}` with the engine's addresses in the SDP |
| `POST /answer` | the page's answer SDP; the connection comes up once the browser's checks reach the engine |
| `POST /report` | once a second: the browser's user agent, its connection state, frames decoded and dropped, the size shown, and with the pattern as the source the latency and the colour bars it reads. Answered with the source, whether this page is still the watcher, and whether a frame is wanted |
| `POST /frame` | the frame the page shows, as a PNG, when `remoteview.grab` asked for one |
| `GET /time` | the engine's clock, for the page's latency |

No STUN or TURN server is involved: the engine gives its own address and the browser reaches it, so both must be on one network (a LAN, a
Tailscale network). The page reconnects by itself two seconds after its connection goes, or after the engine drops it.

## The video

H.264 Constrained Baseline (`profile-level-id=42e01f`, `packetization-mode=1`), which every browser must decode; one keyframe when a
watcher connects and whenever it asks, none on a timer. The stream says its own colour (BT.709 matrix, limited range), so a browser doesn't
have to guess. The size is `remoteview.height` and the width the view's shape gives it, both even; changing it restarts the encoder, not the
connection. The bitrate is fixed at `remoteview.bitrateKbps`: nothing follows the network.

`VideoEncoder` is the seam for another encoder that takes pictures from memory. One that reads the GPU's image itself (Vulkan Video) would
replace capture and encoder together and hand `VideoPacket`s to the sender, which takes nothing else.

## The test pattern

`remoteview.source pattern` sends colour bars, a moving bar and the time, made on the encode thread with no renderer involved. It tells a
broken connection from a black frame, and it is how latency is measured: the time is drawn as 32 black and white cells, the watch page reads
them from each decoded frame and compares with its own clock, corrected by the difference it measures against `GET /time`. The page also reads
the bars' colours back, which shows whether the browser honoured the stream's colour description.

## At runtime

- **Record** `remoteview`: whether it listens and the watch page's URL at each address it is reached at; the watcher (state, address, how many connected and went); the video as encoded;
  frames captured, replaced, encoded and sent since it was turned on; the last second's times (the render thread's copy, conversion,
  encode, capture to send); what the watch page last reported; the last error.
- **Cvars** `remoteview.enabled`, `.source`, `.address`, `.port`, `.height`, `.fps`, `.bitrateKbps`.
- **Start-up flags**: `--remote-view[=<address>[:<port>]]` sets the address (every interface without one), the port and `remoteview.enabled`;
  `--cvar <name>=<value>` sets any of the others. In a configuration without the remote view (`gamerelease`) the flag is an error.
- **Methods and console commands**: `remoteview.keyframe` / `remoteview-keyframe`, `remoteview.disconnect` / `remoteview-disconnect`,
  `remoteview.grab {path}` / `remoteview-grab <path>` (what the watcher sees, after decoding, as a PNG).
- **Window**: Remote View, under Windows > Introspection, or `window remoteview on`.
- **Log**, category `RemoteView`: `RemoteViewListening`, `RemoteViewStopped`, `RemoteViewEncoderStarted`, `RemoteViewWatcherConnected`,
  `RemoteViewWatcherDisconnected`, `RemoteViewWatcherSilent`, and the warning `RemoteViewFailed` (the listener can't bind, an offer or an answer
  failed, the encoder failed).
- **Profiler**: the "Remote View Encode" and "Remote View Http" tracks; zones `RemoteViewConvert`, `RemoteViewEncode`, `RemoteViewSend`;
  the extension's hooks as every extension's (`RemoteViewCapture.afterExecute`, `.afterFence`), with `RemoteViewCopy` inside; the GPU zone
  `RemoteViewCapture`.

## Building its libraries

libdatachannel and OpenH264 are submodules built outside the build system, into `external/libdatachannel_build` and
`external/openh264_build`, as OpenUSD is. The commands are in the root `README.md`. libdatachannel is built static, without WebSocket, and uses
the system's OpenSSL.

## Known gaps

- **The frame is read when its frame context comes round**, which is as many frames later as there are frame contexts (five). A headless
  view that is watched runs at `view.frameLimit`, 60 frames a second (`src/view/README.md`, `limitFrameRate`), so the frame is sent 92 ms
  after it was recorded, against 17 ms with the limit off and the GPU flat out (debug, three cubes). The RHI has no call that asks a fence
  without waiting; with one the frame would be read a frame or two later, not five.
- **Conversion and encoding are on the CPU.** This machine's GPU reports H.264 encode through Vulkan Video; nothing uses it.
- **The bitrate doesn't follow the network**, and there is one watcher.
