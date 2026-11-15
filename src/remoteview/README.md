# Remote view

The `remoteview` library: ngen-view's image as live video in a browser on another device, a phone first. It is for watching a session that runs
somewhere else, an agent's headless run included, without remote desktop software and without capturing a desktop or a window: the image is taken
from the GPU inside the engine, encoded as H.264 and sent over WebRTC. One watcher at a time, video only.

With **remote control** on, the watch page also drives the view: the camera by touch or by mouse and keys, picking, cvars, console
commands, every method and record, on a phone and in a desktop browser alike.

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

`--remote-control` in place of `--remote-view` also lets the page drive the view (`remoteview.control`). There is no authentication:
use it on a network that is yours.

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
| Remote control | `remotecontrol.h/.cpp` (`RemoteControl`) | the watch page's calls, queued from the control channel and run through the view's method registry on the main thread |
| Watch page | `page/index.html`, `page.css`, `page.js`; `videopage.h/.cpp` | the page's files, and their place in the program (`#embed`) |
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
3. **Render thread, `afterFence`** of the next frame, as a rule: each frame the extension asks which captured frames the GPU has finished
   (`Renderer::frameContextDone`, a fence asked without waiting), copies each one's buffer to a `VideoImage` and hands it to
   `RemoteView::submit`. A frame the GPU is still on is asked about again next frame, and is read at the latest when the renderer comes
   round to its frame context, which it waits for anyway. Nothing waits for the GPU on the remote view's account.
4. **Encode thread** ("Remote View Encode"): converts the image to Y'CbCr, encodes it, hands the packet to the sender.
5. **libdatachannel's threads**: packets become RTP and leave.

The watch page's requests are served on the listener's thread ("Remote View Http").

Remote control is in the frame once: **`RemoteControl::drain` on the main thread, right after the RPC endpoint's calls run**
(`runFrameCommands`). libdatachannel's thread only queues the page's messages; `drain` runs them through the same `RpcRegistry` for at
most 2 ms a frame, at least one, and then sends the log events the page is waiting for (64 KiB a frame at most). A method that answers
frames later (a pick, a screenshot) answers on the channel when it does.

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
- **No authentication.** Whoever reaches the address sees the picture. That is why it is off until asked for, why it can be kept to one
  address, and why this is not the RPC endpoint, which stays on loopback.
- **Control has its own switch.** With `remoteview.control` off (the default) nothing is taken from the watcher but its report, and the
  offer carries no control channel. With it on, whoever reaches the address can call every method the view has: set cvars, write files
  to paths the methods take, quit the view. The defence is the network the listener is bound to.
- **A call from the page is the same call.** `RemoteControl` has no methods of its own but the log's subscription: it hands the page's
  JSON-RPC to the registry the loopback endpoint uses, so nothing is registered twice and the page can do exactly what an agent can.
- **An answer larger than a control message fails**, with its size in the error (256 KiB with the browsers tried). Pictures go over
  HTTP instead (`remoteview.screenshot`, `GET /file/…`).
- **The page is made from what the engine describes**: its Cvars, Methods and Records panels from `cvar.get`, `rpc.describe` and
  `introspect.list`. A new cvar, method or record needs no change to the page.
- **One page for a phone and a desktop.** It lays itself out by the window's size and takes input through Pointer Events; nothing asks
  what device it is.

## The watch page and the listener

The engine offers, the page answers:

| Request | Does |
|---|---|
| `GET /`, `/page.css`, `/page.js` | the watch page's files: built into the program, or from `remoteview.pageDir` when that is set |
| `POST /offer` | a new connection for this page, replacing the watcher there was: `{session, sdp, control}` with the engine's addresses in the SDP, and the control channel in it when remote control is on |
| `POST /answer` | the page's answer SDP; the connection comes up once the browser's checks reach the engine |
| `POST /report` | once a second: the browser's user agent, its connection state, frames decoded and dropped, the size shown, the layout and pointer kind the page chose, its self-test's results, and with the pattern as the source the latency and the colour bars it reads. Answered with the source, whether this page is still the watcher, whether a frame is wanted, and whether control is on. Only the watcher's report is kept |
| `GET /file/<name>` | the file last shared with the page: the screenshot `remoteview.screenshot` took |
| `POST /frame` | the frame the page shows, as a PNG, when `remoteview.grab` asked for one |
| `GET /time` | the engine's clock, for the page's latency |

No STUN or TURN server is involved: the engine gives its own address and the browser reaches it, so both must be on one network (a LAN, a
Tailscale network). The page reconnects by itself two seconds after its connection goes, or after the engine drops it, and when remote
control is turned on or off, since the channel comes with a connection. A watcher's session number starts at the clock, so a page left
open from an earlier run is never taken for this run's watcher.

## The watch page

`page/index.html`, `page.css` and `page.js`: plain HTML, CSS and JavaScript, no framework, nothing fetched from elsewhere. To change it
without a rebuild, set `remoteview.pageDir` to `src/remoteview/page` and reload the page.

It picks one of three layouts from the window's size:

| Layout | When | Looks like |
|---|---|---|
| narrow | taller than wide (a phone upright) | the video on top, the panels under it; Panels puts them over the video |
| landscape | wider than tall, and smaller than wide needs (a phone on its side) | the video fills the screen, the strip lies on it, the panels are a drawer |
| wide | at least 900 × 500 (a desktop, a tablet) | the video in the middle and a dock around it: panels left, right and under the video, or floating over it |

**The dock** (wide only). Each panel is in one of three areas, left, right and under the video, or is a window floating over the page.
An area has its own tabs, so several panels show at once; an area with no panel takes no room. A panel is moved by dragging its tab, or
its window's title: the places it can go light up, and it docks in the area it is dropped on, at the edge it is dropped near, or
becomes a window where it is let go. The bars between the areas and the video resize them, a window resizes from its corner. The
layout is remembered in the browser (`localStorage`), per browser, and **Reset layout** in the strip puts it back: the console under
the video, the rest on the right. On a phone the panels are a sheet or a drawer, as before, and the dock's layout is left alone.

The strip on top shows the connection, frames a second, the view's frame and GPU time (wide), the video's size, and has the video's
height, a screenshot to download, Reset layout (wide) and full screen. The panels, with remote control on:

| Panel | From | Does |
|---|---|---|
| Scene | the `status` record, `view.assetsOf`, `view.camera.list` | the selection and its assets, frame it or the scene, the camera's pose, the scene's cameras |
| Render | `cvar.get`, a short list of names | view mode, shadows, culling, overlays, the video's settings |
| Cvars | `cvar.get` | every cvar by group with an editor for its type, searched, changed ones marked with a reset |
| Console | `console.run`, `log.subscribe` | a line that runs a console command, with its history, and the live log under it |
| Methods | `rpc.describe` | every method with a form made from its parameters, and its answer as a tree |
| Records | `introspect.list`, `introspect.get` | every record as a tree, refreshed while open |

On the video, **the mouse and the keys are the view's own controls** (`pollInput` in `src/apps/view.cpp`), so what is learned at the
machine works in the browser. Touch has no buttons or keys and has gestures of its own:

| | Mouse and keys, as in the view | Touch |
|---|---|---|
| Look | the right button held | one finger drags |
| Fly | W A S D, Q and E, Shift for five times the speed | two fingers drag (sideways and up), pinch (forward and back) |
| Fly speed | the wheel, 1.25 times a notch | |
| Pick | the left button, as it goes down | tap |
| Orbit | Left Alt held: around what is under the pointer; the wheel then moves toward it | |
| Frame the selection | F | double tap |
| Deselect, select the parent | Z, R | |
| Follow the ground | G | |
| Console | a backtick (the page's own) | |

The keys go to the camera only while the video has the focus, and not with Ctrl held. All of it becomes calls: `view.camera.move` (what
the pointer, the wheel and the held keys added up to, once an animation frame, one call in flight at a time, on the camera the view's
own controls move), `view.pick` at the frame's pixel under the pointer, `view.camera.orbit.start` and `.stop` for Left Alt (the page
asks again twice a second while it is held, so an orbit ends by itself if the page goes away), `view.select`, `view.camera.frame` and
the `view.camera.follow` cvar.

**The page tests itself** when opened at `<address>/#test`: it runs calls, a cvar set and read back, a failing call, the
log, synthesized touch, mouse and key input against the camera (look, fly, the wheel's speed, orbiting, R and Z), picking by both, a screenshot, a result over the message limit and, in the
wide layout, panels docked in each area, floated, dragged by tab and by title and remembered, and puts
each check's result in its report. The `remoteview` record shows it as `page.test`, so an agent verifies the page with a headless
browser and no way to touch it.

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
  `frame`, the size it is scaled from and `view.pick`'s pixels are in; `control` (on, the channel open, calls run and failed, answers too
  large, messages dropped, the message limit, the last method, the log subscribed and events sent);
  frames captured, replaced, encoded and sent since it was turned on; the last second's times (the render thread's copy, conversion,
  encode, capture to send); what the watch page last reported, with its layout, pointer and self-test; the last error.
- **Cvars** `remoteview.enabled`, `.source`, `.address`, `.port`, `.height`, `.fps`, `.bitrateKbps`, `.control`, `.pageDir`.
- **Start-up flags**: `--remote-view[=<address>[:<port>]]` sets the address (every interface without one), the port and `remoteview.enabled`;
  `--remote-control[=<address>[:<port>]]` the same and `remoteview.control`; `--cvar <name>=<value>` sets any of the others. In a
  configuration without the remote view (`gamerelease`) the flags are errors.
- **Methods and console commands**: `remoteview.keyframe` / `remoteview-keyframe`, `remoteview.disconnect` / `remoteview-disconnect`,
  `remoteview.grab {path}` / `remoteview-grab <path>` (what the watcher sees, after decoding, as a PNG), `remoteview.screenshot` /
  `remoteview-screenshot` (a screenshot the watch page downloads). Made for the page and useful to anyone: `console.run {line}`,
  `view.camera.move` / `camera-move`,
  `view.camera.orbit.start` / `orbit-start`, `view.camera.orbit.stop` / `orbit-stop`.
- **Window**: Remote View, under Windows > Introspection, or `window remoteview on`; its Control section has the switch, the channel,
  the calls and the last method.
- **Log**, category `RemoteView`: `RemoteViewListening`, `RemoteViewStopped`, `RemoteViewEncoderStarted`, `RemoteViewWatcherConnected`,
  `RemoteViewWatcherDisconnected`, `RemoteViewWatcherSilent`, `RemoteControlOpened`, `RemoteControlClosed`, and the warning
  `RemoteViewFailed` (the listener can't bind, an offer or an answer failed, the encoder failed). A call from the page that fails is an
  `RpcCallFailed` warning with `from: remote control`, as a failing loopback call is.
- **Profiler**: the "Remote View Encode" and "Remote View Http" tracks; zones `RemoteViewConvert`, `RemoteViewEncode`, `RemoteViewSend`;
  `RemoteControlDrain` on the main thread, each call's zone inside it;
  the extension's hooks as every extension's (`RemoteViewCapture.afterExecute`, `.afterFence`), with `RemoteViewCopy` inside; the GPU zone
  `RemoteViewCapture`.

## Building its libraries

libdatachannel and OpenH264 are submodules built outside the build system, into `external/libdatachannel_build` and
`external/openh264_build`, as OpenUSD is. The commands are in the root `README.md`. libdatachannel is built static, without WebSocket, and uses
the system's OpenSSL.

## Known gaps

- **The frame is read a frame after it was recorded**, when the render thread next asks: about 16 ms at 60 frames a second, of the 21 ms
  from capture to send (debug, three cubes, headless). Asking sooner needs something to wake the render thread between frames.
- **Conversion and encoding are on the CPU.** This machine's GPU reports H.264 encode through Vulkan Video; nothing uses it.
- **The bitrate doesn't follow the network**, and there is one watcher.
- **Remote control has no authentication**, and the page and the offer travel as plain HTTP, so someone on the path could put
  themselves in the middle. A Tailscale network encrypts the path; a shared LAN does not.
- **Records are polled** by the page, twice a second for the open panel; nothing is subscribed to but the log.
- **The engine's own windows can't be used from the page**: they show in the video, and a tap picks through them.
