// The one place the build system includes <windows.h>: lean, and without the `min` / `max` macros that break
// `std::min` / `std::max` in every header that follows. Windows only; the runner's POSIX code paths never include it.

#pragma once

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <string_view>

namespace ngen::run::win32 {

inline auto widen(std::string_view text) -> std::wstring {
    if (text.empty()) {
        return {};
    }
    int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

inline auto last_error_text() -> std::string {
    DWORD code = ::GetLastError();
    char* buffer = nullptr;
    DWORD length = ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                    nullptr,
                                    code,
                                    0,
                                    reinterpret_cast<char*>(&buffer),
                                    0,
                                    nullptr);
    std::string text = length > 0 ? std::string(buffer, length) : "error " + std::to_string(code);
    if (buffer) {
        ::LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

} // namespace ngen::run::win32

#endif
