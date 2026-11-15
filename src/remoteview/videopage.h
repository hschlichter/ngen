#pragma once

#include <string_view>

// The watch page (src/remoteview/README.md): the files the remote view's listener serves, built into the program from
// src/remoteview/page. Plain HTML, CSS and JavaScript, no framework and nothing fetched from elsewhere, so it works on a network with no
// route out. One page for a phone and a desktop browser: it lays itself out by the room it has and takes touch, mouse and keys.
struct WatchPageFile {
    std::string_view name; // index.html, page.css, page.js
    std::string_view contentType;
    std::string_view body;
};

// The file a request's path names ("/" is index.html); null for a path that is not one of the page's files.
auto watchPageFile(std::string_view path) -> const WatchPageFile*;
