# 89. Remote control: the watch page drives the view through the method registry

Date: 2026-11-15

## Status

Accepted

Builds on [ADR 88](0088_remote_view_over_webrtc.md), whose listener exposed a picture and nothing else.

## Context

The remote view showed ngen-view's image on another device, and that was all the device could do. Everything the view does is an RPC method,
a record or a cvar, described by the engine itself (`rpc.describe`, `introspect.list`, `cvar.get`), but RPC is loopback only by design, so a
phone could watch a session and not move its camera. The page was one string in the program, laid out for nothing in particular.

## Decision

Reviewed with Henrik on 2026-11-15 (locked: no pairing code, no authentication; a phone and a desktop browser are both the target; the rest
accepted as recommended, with the second switch still his to overrule).

- **A WebRTC data channel named `control`, on the watcher's connection**, carries JSON-RPC 2.0 messages as text. The engine offers it, so
  it is in the offer when remote control is on and absent when it is off.
- **`RemoteControl` runs the page's calls through the view's `RpcRegistry` on the main thread**, right after the loopback endpoint's, with
  a 2 ms budget of its own. It has no methods but the log's subscription, so a call from the page is the call an agent makes.
- **No authentication, behind a switch of its own.** `remoteview.control`, off by default, apart from `remoteview.enabled`: showing the
  picture doesn't hand over the engine. With it on, whoever reaches the address can call every method.
- **Every method, no allow-list.**
- **An answer over the channel's message limit fails with its size**; nothing is split. Pictures go over HTTP (`remoteview.screenshot`,
  `GET /file/<name>`).
- **The log is sent from a cursor into the ring**, a bounded amount a frame, in the loopback stream's shape. Records are polled.
- **The page's panels are made from what the engine describes**, and the page is three files built into the program with `#embed`.
- **One page for a phone and a desktop**: its layout follows the window's size, its input goes through Pointer Events.
- **The camera is moved by a relative method**, `view.camera.move`, so the page keeps no camera of its own.
- **The page tests itself** (`#test`) and reports the results, so an agent verifies it through the `remoteview` record.

Rejected: a pairing code (drafted, dropped by Henrik); `POST /rpc` on the listener (one request a connection: the log polled, a socket for
every camera move); a WebSocket (libdatachannel rebuilt, a second connection); an allow-list of methods (a second place to register each);
the page keeping the camera and calling `view.camera.set` (fights a flight and the follow mode); two pages, one for each kind of device.

## Consequences

- Easier: a session is driven from a phone or a desktop browser; a new cvar, method or record is on the page without touching it.
- Easier: the page is verified by an agent, headless, at any window size.
- Harder: with `remoteview.control` on, an unauthenticated port can set cvars, write files to paths the methods take and quit the view.
  The page and the offer travel as plain HTTP, so someone on the path could sit in the middle. The defence is the network it is bound to.
- Harder: the `remoteview` library links `rpc`; the watch page is 2,000 lines of JavaScript and CSS with no tests but its own.
- Harder: `console.run`, `view.camera.move` and `remoteview.screenshot` exist for the page; `logging::eventJson` is public.
- Cost, release, Sponza, headless, this machine: `RemoteControlDrain` 0.055 ms a frame with the Cvars panel open, 0.0004 ms with no watcher.

## Verification

- Sources: [plan_remote_control.md](../plan_remote_control.md), `src/remoteview/README.md`.
- Code:
  - `src/remoteview/remotecontrol.h:20` (`RemoteControl`); `src/remoteview/remotecontrol.cpp:50` (`drain`), `:99` (`run`), `:169` (`sendLog`).
  - `src/remoteview/videosender.cpp:140` (the channel in the offer), `:302` (`sendControl`).
  - `src/remoteview/remoteviewcvars.cpp:32` (`remoteview.control`); `src/remoteview/videopage.cpp:12` (the page's files embedded).
  - `src/remoteview/page/page.js:91` (`chooseLayout`), `:1430` (`flushMove`: the camera's calls), `:1501` (`runSelfTest`).
  - `src/view/viewcommands.cpp:86` (`console.run`); `src/view/cameracommands.cpp:287` (`view.camera.move`);
    `src/view/remoteviewcommands.cpp:153` (`remoteview.screenshot`).
  - `src/apps/view.cpp:569` (made), `:622` (`drain`, after the endpoint's calls).
- Differs from plan: `RemoteControl` takes the remote view, not the sender; `remoteview.screenshot` is new; a double click frames the
  selection where the plan had it for full screen; a watcher's session number starts at the clock and only the watcher's report is kept,
  after a page left open from an earlier run was taken for the watcher.
- Henrik used it from Safari on his iPhone (2026-11-15). Not verified when this was written: a desktop browser by hand, Chrome.
