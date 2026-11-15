#include "videopage.h"

#include <array>

namespace {

// The page's files, built into the program as they are in src/remoteview/page. #embed is C23's and C++26's, which the compiler takes
// in C++23 as an extension and says so.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
constexpr char indexHtml[] = {
#embed "page/index.html"
};
constexpr char pageCss[] = {
#embed "page/page.css"
};
constexpr char pageJs[] = {
#embed "page/page.js"
};
#pragma clang diagnostic pop

constexpr std::array<WatchPageFile, 3> files = {{
    {.name = "index.html", .contentType = "text/html; charset=utf-8", .body = std::string_view(indexHtml, sizeof(indexHtml))},
    {.name = "page.css", .contentType = "text/css; charset=utf-8", .body = std::string_view(pageCss, sizeof(pageCss))},
    {.name = "page.js", .contentType = "text/javascript; charset=utf-8", .body = std::string_view(pageJs, sizeof(pageJs))},
}};

} // namespace

auto watchPageFile(std::string_view path) -> const WatchPageFile* {
    if (path == "/") {
        return &files[0];
    }
    for (const auto& file : files) {
        if (path.size() == file.name.size() + 1 && path.front() == '/' && path.substr(1) == file.name) {
            return &file;
        }
    }
    return nullptr;
}
