#include "remoteviewcvars.h"

namespace remoteviewcvars {

cvar::Bool enabled{"remoteview.enabled",
                   false,
                   "Serve the watch page and send the view's image to a browser that opens it, as live video. Off: no listener, no thread, "
                   "nothing in the frame."};

cvar::Enum source{"remoteview.source",
                  {"frame", "pattern"},
                  sourceFrame,
                  "What a watcher is sent: the presented frame, UI included, or a test pattern made without the renderer (colour bars, a moving "
                  "bar and the time it was drawn, which the watch page reads to measure latency)."};

cvar::String address{"remoteview.address",
                     "0.0.0.0",
                     "The IPv4 address the watch page is served on and the video is sent from: 0.0.0.0 for every interface, or one of this "
                     "machine's addresses to keep it to that network (its Tailscale address). Anyone who reaches it sees the picture."};

cvar::Int port{"remoteview.port", 8730, "The port the watch page is served on: http://<address>:<port>/.", {.min = 1, .max = 65535}};

cvar::Int height{"remoteview.height",
                 720,
                 "The video's height in pixels; its width follows the view's shape. The frame is scaled to it on the GPU before it is read back.",
                 {.min = 180, .max = 2160}};

cvar::Int fps{"remoteview.fps", 30, "Frames sent a second, at most: the view's frames in between are not captured.", {.min = 1, .max = 60}};

cvar::Int bitrateKbps{"remoteview.bitrateKbps", 4000, "The video's bitrate in kilobits a second. Fixed: it does not follow the network.", {.min = 250, .max = 50000}};

cvar::Bool control{"remoteview.control",
                   false,
                   "Let the watch page drive the view: its connection carries a control channel, and the page's calls run as the RPC endpoint's "
                   "do, every method. No authentication: whoever reaches the watch page can set cvars, write files and quit the view. Takes "
                   "effect when a watch page next connects."};

cvar::String pageDir{"remoteview.pageDir",
                     "",
                     "A directory to serve the watch page's files from (index.html, page.css, page.js), for changing the page without a "
                     "rebuild: src/remoteview/page. Empty: the files built into the program."};

} // namespace remoteviewcvars
