#pragma once

#include "cvar.h"

// The remote view's cvars (src/cvar/README.md). They belong to the main thread; RemoteView::update reads them once a frame and hands
// what changed to its threads.
namespace remoteviewcvars {

extern cvar::Bool enabled;    // remoteview.enabled
extern cvar::Enum source;     // remoteview.source
extern cvar::String address;  // remoteview.address
extern cvar::Int port;        // remoteview.port
extern cvar::Int height;      // remoteview.height
extern cvar::Int fps;         // remoteview.fps
extern cvar::Int bitrateKbps; // remoteview.bitrateKbps
extern cvar::Bool control;    // remoteview.control
extern cvar::String pageDir;  // remoteview.pageDir

constexpr size_t sourceFrame = 0;
constexpr size_t sourcePattern = 1;

} // namespace remoteviewcvars
