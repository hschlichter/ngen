#pragma once

#include "remoteviewstatus.h"

// The Remote View window (src/remoteview/README.md): the address to open on the other device, the watcher, the video as it is encoded,
// what was captured, encoded and sent, what each step costs, and what the watch page says it decoded; remote control (whether the
// watch page may drive the view, its channel, the calls it made); with the remoteview.* cvars to turn it on and set it. `status` and
// `control` are null in a program without a remote view.
auto drawRemoteViewWindow(bool& show, const RemoteViewStatus* status, const RemoteControlStatus* control) -> void;
