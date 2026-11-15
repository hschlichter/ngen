#pragma once

#include <string_view>

// The watch page: the one HTML page the remote view's listener serves at "/". Plain JavaScript, no framework and nothing fetched from
// elsewhere. It asks the engine for an offer, answers it, plays the video, reconnects when the connection goes, and tells the engine
// once a second what the browser decoded.
auto videoWatchPage() -> std::string_view;
