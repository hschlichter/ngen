# Remote control: the watch page drives the view

**Status. In progress.** Built on 2026-11-15 and checked headless against Firefox at three window sizes, with the page's self-test, and
used by Henrik from Safari on his iPhone ("works well", 2026-11-15). Open: a desktop browser by hand. Results are under "Built" and "Results".

Extends [plan_remote_view.md](plan_remote_view.md) ([ADR 88](adr/0088_remote_view_over_webrtc.md)), whose deferred item "Input from the watch
page" this is. Claims about the code are marked **observed** (read in the source, with the place) or **assumed**.

## Current state

The remote view sends ngen-view's presented image to one browser as H.264 over WebRTC. The watch page shows the video and a line of status,
and sends a report once a second. It can't change anything in the engine.

What this builds on:

- **Everything the view does is an RPC method or a record.** Methods are registered in an `RpcRegistry` with typed parameters (`Bool`, `Int`,
  `Float`, `String`, `Vec3`, `Enum` with its values, `Array`, `Object`, `Any`), a summary and a result description, and `rpc.describe`
  returns all of it (observed: `src/rpc/rpcregistry.h:17`, `:58`). Records are listed and read by `introspect.list` and `introspect.get`
  (observed: `src/rpc/README.md`, "Records").
- **Cvars describe themselves.** `cvar.get` returns each value's name, type, value, default, description, range and choices (observed:
  `src/view/cvarcommands.cpp:197`).
- **The methods a phone wants exist**: `view.camera.get/set/orbit/frame/use/list` (observed: `src/view/cameracommands.cpp:110` on),
  `view.pick {x, y}` at a framebuffer pixel and `view.select` (observed: `src/view/scenecommands.cpp:461`, `:402`), `view.screenshot` with
  `inline` (observed: `src/view/displaycommands.cpp:212`), `view.window` and the `windows` record (observed: `src/ui/uicommands.cpp:26`,
  `:44`), `cvar.set`.
- **Calls run on the main thread.** `RpcEndpoint::drain` runs queued calls through `RpcRegistry::invoke` for at most 2 ms a frame, and a
  handler answers through an `RpcResponder`, at once or frames later (observed: `src/rpc/rpcendpoint.h:36`, `src/rpc/core/rpcresponder.h:22`).
  `invoke` takes a name, parameters and a responder, and knows nothing of sockets (observed: `src/rpc/rpcregistry.h:56`).
- **RPC is loopback only, with no authentication**, by design (observed: `src/rpc/README.md`, "Known gaps"). The remote view's listener is
  reachable from the network and has no authentication either, because it exposes a picture and nothing else (ADR 88).
- **The log stream is tied to the RPC server.** `LogStream` sends `log.events` to `RpcServer` connections (observed: `src/log/logstream.h:31`).
  A second receiver needs its own reader of the ring.
- **No method runs a console line.** `ConsoleCommands::run` takes a command and its text and logs failures; it is called for script lines
  and start-up flags only (observed: `src/rpc/consolecommands.h:24`, `src/view/viewcommands.cpp:95`).
- **The engine makes the WebRTC offer** (observed: `src/remoteview/videosender.cpp:46`), so it can add a data channel to it. libdatachannel
  is linked with its SCTP library already (observed: `build.cpp:603`).
- **The watch page is one raw string** of 284 lines (observed: `src/remoteview/videopage.cpp`). The build has no way to embed a file
  (observed: no rule in `build.cpp`). The compiler is GCC 16, which accepts `#embed`.
- **The frame is sent 21 ms after it was recorded** at the headless limit of 60 frames a second, since the capture asks the frame's fence
  without waiting (observed: plan_remote_view.md, "Built"; debug, three cubes).
- **The page is laid out for nothing in particular**: a video and a line of text, the same on a phone and on a desktop.

## Goals

1. From a phone and from a desktop browser, both first-class: move the camera, pick an object, switch view modes, set cvars, run console
   commands, read records and the log. Touch on one, mouse and keyboard on the other, one page.
2. The page's panels are generated from `rpc.describe`, `introspect.list` and `cvar.get`. A new method, record or cvar appears on the phone
   with no change to the page.
3. The page looks like a tool made on purpose: video first, readable on a phone held in one hand, and using the room a desktop window
   has, with panels beside the video, not a phone's layout stretched.
4. Control is off until asked for, as the picture is.
5. Headless first, as the remote view is.

## Decisions

**Locked** is what Henrik decided on 2026-11-15. The rest are recommendations; pushback welcome.

1. **Access: no authentication, and off by default.** Locked: no pairing code (Henrik, 2026-11-15, after a draft that had one).
   Recommended with it: `remoteview.control` (Bool, off) turns control on, apart from `remoteview.enabled`, so showing someone the picture
   doesn't hand them the engine. `--remote-control[=<address>[:<port>]]` sets both from the start. With control on, whoever reaches the
   address can call every method: set cvars, write files to paths the methods take, quit the view. The defence is the network it is bound
   to (`remoteview.address`, a Tailscale address), as for the picture.
2. **Transport: a data channel named `control`, on the connection the video uses.** Reliable and ordered. One data channel message is one
   JSON-RPC 2.0 message as text, the same messages `src/rpc/core/rpcprotocol.h` makes; no frame header and no attachments.
   Alternatives: `POST /rpc` on the listener is simpler, but the listener serves one request a connection, so the log would be polled and
   every camera move would open a socket. A WebSocket needs libdatachannel rebuilt with it and a second connection to authenticate.
3. **Everything in the registry.** No allow-list. A watcher is trusted as a local process is trusted on loopback:
   `view.quit`, `view.screenshot {path}` and `view.dumpTexture` included. I lean this way because an allow-list is a second place to
   register every method, and Goal 2 says there is none. Alternative: an `RpcMethodDesc::remote` flag, default true, cleared on the methods
   that write to a given path.
4. **Calls run on the main thread, beside the loopback endpoint's.** A new `RemoteControl` queues what the data channel delivers and runs
   it in `drain()`, called right after `RpcEndpoint::drain`, with its own 2 ms budget. Handlers can't tell the two apart.
5. **Results too large for a data channel message fail, and say so.** Each side announces its largest message in the SDP
   (`a=max-message-size`); libdatachannel's default is 256 KiB. A larger reply is answered with an error that names its size. Pictures
   don't travel this way: the page fetches them over HTTP (decision 9). Alternative: splitting messages. Not until a record needs it.
6. **The log: `log.subscribe` answered by the gateway itself**, from a cursor into the ring, sent as `log.events` in the same shape
   `LogStream` sends. At most 64 KiB of events a frame; what the ring overwrites meanwhile is counted in `dropped`.
7. **Records are polled**, as they are everywhere (`src/rpc/README.md`, "Known gaps"). The page reads the records of the open panel twice a
   second and the status strip's four times a second. No subscription is added here.
8. **Camera: a relative method, `view.camera.move`.** The page sends what the fingers did (`forward`, `right`, `up` in metres, `yaw`,
   `pitch` in degrees), at most one call in flight, the latest gesture replacing what waits. The engine applies it to the camera it has.
   Alternative: the page keeps the camera and sends `view.camera.set`. That fights a flight, a follow or the debug camera, and jumps when a
   call is lost in a reconnect.
9. **Pictures over HTTP.** `GET /file/<name>` serves a PNG the watcher asked for with `view.screenshot`, from a small table in memory
   (one entry, replaced by the next). The page offers it as a download. It is as open as the video is.
10. **The page becomes files**, `src/remoteview/page/index.html`, `page.css` and `page.js`, put into the program with `#embed`. No
    framework, no bundler, nothing fetched from the internet: it must work on a Tailscale network with no route out.
    `remoteview.pageDir` (String, empty) serves the files from a directory when set, so the page is edited without a rebuild.
    Alternative: more raw strings. They have no editor support and a length limit per literal.
11. **Terms.** *Remote control* is the feature. The *control channel* is the data channel. The page stays the *watch page*.
12. **Still one watcher.** Control belongs to the watcher. A second page that opens takes the video and the control with it.
13. **One page for phone and desktop, by the room and the pointer it has, not by the device's name.** The layout follows the window's
    width (CSS container and media queries); input goes through Pointer Events, so touch, mouse and pen share one path, and what differs
    is decided by `pointerType`. No user-agent sniffing, and no second page. Alternative: `/` for the phone and `/desktop` for the rest.
    Two pages drift apart, and a tablet or a narrow desktop window is neither.

## Scope

In:

- The control channel and `RemoteControl` dispatching to the view's registry.
- `console.run`, `view.camera.move`, and the frame's size in the `remoteview` record.
- The watch page rebuilt: layout, status strip, and the panels Scene, Render, Cvars, Console, Methods, Records.
- Camera and picking by touch, and by mouse and keyboard.
- The page's files embedded, and `remoteview.pageDir`.
- A self-test in the page, so a headless Firefox verifies it.

Out:

- **Touch forwarded to the engine's own UI** (Dear ImGui windows used from the phone). Deferred.
- HTTPS in the listener, more than one watcher, audio, a bitrate that follows the network.
- The asset server and other processes: the gateway serves the view's registry only.

## Steps

### 1. The control channel

- `src/remoteview/videosender.h/.cpp`: `offer` creates the data channel `control` before the offer when asked to. New:

  ```cpp
  using ControlHandler = std::function<void(uint32_t session, std::string message)>;

  auto setControlHandler(ControlHandler handler) -> void;
  auto sendControl(uint32_t session, std::string_view message) -> bool;
  auto controlMessageLimit() const -> size_t;
  ```

  The handler runs on libdatachannel's thread and only queues.
- `src/remoteview/remotecontrol.h/.cpp`, new. The `remoteview` library links `rpc`.

  ```cpp
  class RemoteControl {
  public:
      RemoteControl(VideoSender& sender, const RpcRegistry& registry);

      // Main thread, after RpcEndpoint::drain. Runs queued calls for at most drainBudgetNs, then sends waiting log events.
      auto drain() -> void;
      auto setEnabled(bool enabled) -> void;
      auto status() const -> const RemoteControlStatus&;

      static constexpr int64_t drainBudgetNs = 2'000'000;
  };
  ```

  `RemoteControlStatus`: `enabled`, `open` (the channel is up), `calls`, `failed`, `lastMethod`, `logSubscribed`.
- `log.subscribe` and `log.unsubscribe` handled as decision 6 says. *Assumed: the ring can be read from a cursor by a second reader, as
  `LogStream::pump` does. Read `src/log/logring.h` first.*
- A reply larger than the watcher's `max-message-size` fails with `-32000`, "result is N bytes, over the channel's M".
- `src/remoteview/remoteviewcvars.cpp`: `remoteview.control`.
- `src/apps/view.cpp`: `RemoteControl` made after the registry, `drain()` after the endpoint's; `--remote-control[=<address>[:<port>]]`.

### 2. The methods the page needs

- `console.run {line}` in `src/view/viewcommands.cpp`: splits the line into command and text as a script line is split, and calls
  `ConsoleCommands::run`. `run` gains an overload that takes a responder, so the method's result or error is the answer, not only a log
  line. Console command `run` is not added: a console line that runs a console line says nothing.
- `view.camera.move {forward, right, up, yaw, pitch, camera?}` in `src/view/cameracommands.cpp`, console `camera-move`. It ends a flight,
  as moving the camera with the keys does. *Assumed: there is one place that applies key and mouse movement to the camera; the method
  calls it. Read `src/view/` for it first.*
- The `remoteview` record gains `control` (the status above) and `frame: {width, height}`, the size `view.pick`'s pixels are in.

### 3. The page as files, and its shell

- `src/remoteview/page/index.html`, `page.css`, `page.js`; `videopage.cpp` embeds them and serves `/page.css` and `/page.js`.
  *To check first: that `ngen-build` rebuilds `videopage.cpp` when an embedded file changes (the compiler's dependency file must list
  it). If it doesn't, the files are named as inputs in `build.cpp`.*
- `remoteview.pageDir`.
- Layout, by the window's width:
  - **Narrow** (a phone upright): the video across the top at its own shape, a status strip on it, and below it a sheet with tabs that
    drags up over the video.
  - **Short and wide** (a phone on its side): the video fills the screen, the sheet is a drawer from the right.
  - **Wide** (a desktop, a tablet): the video left at its own shape, as large as fits; a column of panels right, two side by side when
    there is room (Cvars beside Console); the status strip along the top. The column's width is dragged and remembered. The video goes
    full screen on a double click or `F`.
- The status strip: connection, frame ms, GPU ms, frames a second, the video's size and bitrate, latency with the pattern as the source.
- Look: dark, one accent colour, system font, numbers in tabular figures, no scrolling of the page itself. Controls are 44 px with a
  coarse pointer and 28 px with a fine one (`@media (pointer: coarse)`), safe-area insets respected, hover states only where there is
  hover.
- The video's height is a choice in the strip (360, 720, 1080, the view's own), which sets `remoteview.height`: 720 suits a phone and is
  soft in a desktop window.
- Control off: the page is the video and the strip.
- A small client in `page.js`: `call(method, params) -> Promise`, notifications by method name, calls rejected when the channel closes.
- What the page does today stays: reconnecting, the report, the grab, the pattern's latency.

### 4. Generated panels

- **Cvars**: `cvar.get {}` once, then the open group twice a second. Groups by the name's first part. An editor by type: a switch, a
  slider with its range, a choice list, three number fields, a text field. A changed value is marked and can be reset. A search field.
- **Console**: a line that calls `console.run`, its history kept in `localStorage` (arrow up and down with a keyboard), and the log
  under it from `log.subscribe`, coloured by severity, filterable by category.
- **Methods**: `rpc.describe` once. A list by area; a method opens a form made from its parameters, with a JSON field for `Array`,
  `Object` and `Any`, and shows the result as a tree.
- **Records**: `introspect.list`, each record a tree, refreshed while open.
- **Render**: view mode, debug view, the overlays and shadows, as shortcuts to their cvars. The one hand-written panel besides Scene.

### 5. Camera and picking

- **Touch**: one finger drags to look; two fingers drag to move sideways and up; a pinch moves forward and back; a tap picks; a double
  tap frames the selection (`view.camera.frame`).
- **Mouse and keyboard**, with the pointer over the video: the left button drags to look, the middle or right button drags to move
  sideways and up, the wheel moves forward and back; W A S D and Q E fly while held, Shift faster; a click that didn't drag picks; F
  frames the selection; a backtick puts the cursor in the console line. Keys go to the camera only while the video has the focus, so
  typing in a panel never flies.
- Both become `view.camera.move`: the page sends what the gesture or the held keys add up to each animation frame.
- A pick is `view.pick` at the video's point scaled to `frame.width` × `frame.height`. The **Scene** panel then shows the selection from
  the record that holds it, with its LOD (`view.lod.inspect`) and assets (`view.assetsOf`), and lists the scene's cameras
  (`view.camera.list`, `view.camera.use`).
- A screenshot button: `view.screenshot`, then the download of decision 9.

### 6. The self-test, visibility and documents

- `http://…/#test` makes the page run a fixed list: `rpc.ping`, a cvar set and read back, `console.run`, a call that must fail, and
  synthesized input of both kinds: a touch drag, pinch and tap, and a mouse drag, wheel, held key and click. It also reports which
  layout it chose and whether anything overflows the window. The results go in the report and show in the record as `page.test`.
- The Remote View window, the READMEs, `REFERENCE.md`, `GLOSSARY.md`, the `run-headless` skill.
- ADR 89, "Builds on 88": the listener now carries control, with no authentication, behind a switch of its own.

## Built

Steps 1 to 6, on 2026-11-15. What differs from the steps above:

- **`RemoteControl` takes the `RemoteView`, not the sender** (`remotecontrol.h`), and is made in `src/apps/view.cpp` after the registry.
  The sender got `setControlHandler`, `sendControl`, `controlOpen`, `controlMessageLimit` and `controlBuffered`.
- **`remoteview.screenshot`** is new and not in the steps: `view.screenshot` into the temporary directory, the PNG then served at
  `GET /file/<name>`. It replaces "the page calls `view.screenshot`", whose default path is the working directory.
- **The log's reader was there to use**: `LogRing::read` takes a cursor (`src/log/logring.h`), and `logging::eventJson` was made public so
  the page gets the events in the loopback stream's shape.
- **`#embed` works and is tracked**: the compiler is Clang 23, which takes it in C++23 as an extension (the warning is silenced in
  `videopage.cpp`) and lists the embedded files in its dependency file, so a changed page file rebuilds.
- **Double click frames the selection**, as a double tap does; the plan had it for full screen in one place and F for framing in
  another. Full screen is the strip's button.
- **A watcher's session number starts at the clock.** Found by the first test: a page left open from an earlier run held the same small
  number as the new run's watcher, was taken for it, and its reports replaced the watcher's. Only the watcher's report is kept now.
- **The hint over the video goes after ten seconds.**
- **Profiling the strip**: the view's frame and GPU time come from the `profile` record once a second, in the wide layout only (it is
  38 KB).
- **`RemoteControlPaired` and the rest of the pairing went** with decision 1.
- **The mouse and the keys are the view's own controls** (asked for by Henrik, 2026-11-15), where step 5 had invented a set: the right
  button looks, the wheel sets the fly speed, the left button picks as it goes down, Left Alt orbits, Z, R, F and G do what they do in
  the view. `view.camera.move` took wheel steps, turns around the pivot while orbiting and can move the controlled camera;
  `view.camera.orbit.start` and `.stop` are Left Alt as calls, held by asking again. Touch keeps its gestures.
- **The wide layout is a dock** (asked for by Henrik after using the page on a desktop, 2026-11-15: the fixed column left room unused).
  Panels sit in areas left, right and under the video, or float; they are dragged by their tabs and titles, resized by the bars between,
  remembered in `localStorage`, and reset from the strip. It replaces "two side by side when there is room". A panel is still one
  panel: two records side by side needs panels opened more than once, which is not built.

## Results

Debug unless it says release; headless; Firefox 160 headless on this machine; three cubes unless it says Sponza.

| Check | Result |
|---|---|
| Off is free | **Passed.** `--remote-view` alone: the offer has no `m=application` section and `control` false; with `remoteview.control` set, one, and true. |
| On | **Passed.** `RemoteControlOpened` once, `control.open` true, the self-test done with 18 of 18 passed. |
| The dock | **Passed.** In the wide layout the self-test docks Cvars left, under the video and right, floats it, drags Records left by its tab and the Cvars window under the video by its title, and reads the layout back from storage; a rearranged layout was also seen to survive a reload of the page. |
| Both sizes | **Passed.** 19 of 19 at 1920 × 1080 (wide), 18 of 18 at 500 × 758 (narrow) and at 844 × 390 (landscape), none overflowing. Firefox would not make a window narrower than 500 px, so the phone's own width is Henrik's check. |
| Both inputs | **Passed** in each: touch drag 20° for 100 px, pinch 4.00 m; the mouse's right drag 15° and its left drag nothing; the wheel 5 → 6.25 → 5 m/s; Left Alt orbiting at a constant distance from a picked pivot, the wheel then 15 % nearer, held for 2.5 s and ended on release; R and Z; a held W 1.5 m in 0.3 s, a key in a field nothing. |
| Calls are the same calls | **Passed.** `remoteview.bitrateKbps` set from the page logs the encoder's restart at 3210 kbit/s; `console.run`, `view.camera.move` and `remoteview.screenshot` answer over loopback as on the channel. |
| Picking | **Passed.** A tap and a click at the centre pick `/World/Cube_1`, and the Scene panel shows it. |
| Log | **Passed.** The failing call's `RpcCallFailed` reaches the page; 85 to 177 lines arrived. |
| Too large | **Passed.** `view.screenshot {inline}` answers "the result is 449260 bytes, over the control channel's 262144", `control.tooLarge` 1, and the next call works. |
| Cost | **Passed.** Release, Sponza, Cvars panel open in the wide layout: `RemoteControlDrain` 0.055 ms a frame on average (median 0.002); with no watcher 0.0004 ms. Frame time is held at the 60 a second limit either way. |
| `gamerelease` | **Passed.** Builds; `--remote-control` is refused; no TLS library is loaded. The one `remoteview.control` string in it is the Remote View window's cvar name. |
| The log | **Passed.** No warnings or errors but the self-test's own failing call, with `--fail-on-validation`. |
| `remoteview.pageDir` | **Passed.** The page's files come from the directory, and a missing one is a 404 that says so. |
| The picture | Looked at: screenshots of the page in the wide and narrow layouts, taken through Firefox's remote protocol. |
| The phone | **Passed (Henrik, 2026-11-15).** Safari on an iPhone, narrow layout, coarse pointer, as its report says; the control channel open, calls made, none failed. "Works well." |
| A desktop browser by hand | **Open (Henrik).** |
| Another browser than Firefox | Safari on iOS: see the phone. Chrome has not opened the page. |
| The capture delay with a moving camera | **Not judged.** 20 to 23 ms from capture to send; how a drag feels is Henrik's check. |

## Visibility

- **Record** `remoteview`: `control` (enabled, whether the channel is open, calls run and failed, the last method, whether the log is
  subscribed), `frame` (width, height), `page.test`, and in the page's report the layout it chose and its pointer kind.
- **Methods and console commands**: `console.run`; `view.camera.move` / `camera-move`.
- **Cvars**: `remoteview.control`, `remoteview.pageDir`. **Flag**: `--remote-control[=<address>[:<port>]]`.
- **Window**: Remote View gains a Control section: the Control box, whether the channel is open, the counts, the last method.
- **Log**, category `RemoteView`: `RemoteControlOpened`, `RemoteControlClosed`; `RpcCallFailed` for a failing call, as the loopback
  endpoint logs it. The line that says where the watch page is says whether control is on.
- **Profiler**: `RemoteControlDrain` on the main thread, with each call's zone inside as the registry makes them.
- **Where it sits in the frame**: `src/remoteview/README.md`, "Where it sits in the frame", gains the drain.
- **Gap, named**: the watch page is itself a view of the engine that no record describes beyond the report. The self-test is what makes
  it readable to an agent.

## Verification

Headless, with headless Firefox on this machine, the log recorded with `introspect log`. Each line is pass or fail.

- **Off is free.** `remoteview.control` off: the offer's SDP has no `m=application` section, and the record's `control.enabled` is false.
- **On.** `#test`: `RemoteControlOpened` once, `control.open` true, `page.test` all passed.
- **Both sizes.** The self-test passes in a window of 390 × 844 and of 1920 × 1080 (Firefox's `--window-size`): the first reports the
  narrow layout and the second the wide one, and neither reports overflow.
- **Both inputs.** In each run the synthesized touch input and the synthesized mouse and keyboard input pass.
- **Calls are the same calls.** A cvar set from the page reads back the same over loopback (`introspect get view cvars`), and the other way.
- **Camera.** The self-test's drag, by touch and by mouse, changes `view.camera.get`'s yaw by the expected amount; its pinch, its wheel
  and a held W move the position along the view direction. A key pressed while a panel's field has the focus moves nothing.
- **Picking.** The self-test's tap, and its click, at the centre of `three_cubes.usda` logs `Picked` with a cube's prim path, and the Scene panel's
  text, sent in the report, names the same prim.
- **Log.** A `console.run` of an unknown command arrives in the page as a `ScriptCommandFailed` event.
- **Too large.** A call whose result is over the limit fails with the size in its message, and the channel stays open.
- **Cost.** Release, Sponza, headless: main-thread frame time with control on and an idle page within noise of control off; with the Cvars panel
  open, `RemoteControlDrain` under 0.2 ms a frame on average.
- **`gamerelease`** builds and contains no `control` strings.
- **The log** has no warnings or errors on a clean run, with `--fail-on-validation`.

What Henrik checks: on the phone, the layout upright and on its side, that the camera feels right under the fingers, and that a tap
picks what was tapped; in a desktop browser, the wide layout, flying with mouse and keys, and a click picking what was clicked.

## Open questions

1. **Is `remoteview.control` as a second switch wanted, or should `--remote-view` carry control too?** Built with the second switch:
   the picture is harmless to show and control is not, and the flag `--remote-control` makes it one word either way. Still Henrik's to
   overrule.
2. **Does the control channel reach the phone's browser as the video does?** Same connection, same addresses, so probably. SCTP over
   DTLS is a second protocol on it, and only the phone answers this. Firefox on this machine: yes. Safari on the iPhone: yes.
3. **The largest message each browser announces.** With Firefox the channel's limit is 262144 bytes (libdatachannel's own). The largest
   record here is `zones` at 83 KB, so nothing the page reads is over it on these scenes; a large world's records may be.
4. **Does the camera feel right at 21 ms from capture to send?** Encode, network and decode come on top. Measured end to end with the
   pattern in step 5; if a drag trails the finger, the next cut is waking the render thread when the GPU finishes a frame.

## Deferred / follow-ups

- **Touch forwarded to the engine's UI**: taps and drags on the video become pointer events for Dear ImGui, so every window is usable
  from the phone. Trigger: a window is wanted on the phone that a generated panel doesn't cover.
- **Record subscriptions.** Trigger: polling shows in `RemoteControlDrain`, or a panel needs more than four updates a second.
- **Split messages** for large results. Trigger: open question 3.
- **An allow-list** (`RpcMethodDesc::remote`), **a pairing code, or HTTPS.** Trigger: the page is used on a network that is not Henrik's
  own. A pairing code was drafted and dropped on 2026-11-15: six digits shown on the machine, traded by the page for a token.
- **Pointer lock** for mouse look without holding a button. Trigger: flying a large world from the desktop.
- **Other processes through the gateway** (the asset server's records). Trigger: wanting them on the phone.
- **Two fingers, two sticks**: a game-pad layout for flying. Trigger: the gestures of step 5 are not enough to fly a world.
