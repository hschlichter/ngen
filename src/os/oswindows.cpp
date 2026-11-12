#include "os.h"

#include <array>
#include <atomic>
#include <windows.h>

#include <psapi.h>

namespace os {

namespace {

auto widen(std::string_view text) -> std::wstring {
    if (text.empty()) {
        return {};
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int) text.size(), nullptr, 0);
    std::wstring out((size_t) length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int) text.size(), out.data(), length);
    return out;
}

// One argument as the C runtime's command-line parser (CommandLineToArgvW's rules) reads it back: in double quotes when it is empty or
// holds whitespace or a quote; inside them, a run of backslashes is doubled before a quote and at the end.
auto quoteArgument(std::string_view value) -> std::string {
    if (!value.empty() && value.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        return std::string(value);
    }
    std::string out = "\"";
    size_t backslashes = 0;
    for (char ch : value) {
        if (ch == '\\') {
            backslashes++;
            continue;
        }
        if (ch == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(backslashes, '\\');
            out += ch;
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

auto commandLine(const std::vector<std::string>& argv) -> std::wstring {
    std::string line;
    for (const auto& arg : argv) {
        if (!line.empty()) {
            line += ' ';
        }
        line += quoteArgument(arg);
    }
    return widen(line);
}

std::atomic<HANDLE> g_stopEvent = nullptr;
std::atomic<DWORD> g_stopType = 0;

auto WINAPI consoleControl(DWORD type) -> BOOL {
    HANDLE event = g_stopEvent.load();
    if (event == nullptr) {
        return FALSE;
    }
    g_stopType = type;
    SetEvent(event);
    return TRUE;
}

} // namespace

auto processId() -> int {
    return (int) GetCurrentProcessId();
}

auto processAlive(int pid) -> bool {
    if (pid <= 0) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
    if (process == nullptr) {
        // A process this user may not open is still running.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD code = 0;
    bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

auto executablePath() -> std::expected<std::filesystem::path, std::string> {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), (DWORD) buffer.size());
        if (length == 0) {
            return std::unexpected("GetModuleFileName failed");
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    std::error_code ec;
    auto path = std::filesystem::canonical(std::filesystem::path(buffer), ec);
    if (ec) {
        return std::unexpected(ec.message());
    }
    return path;
}

auto residentBytes() -> uint64_t {
    PROCESS_MEMORY_COUNTERS counters = {};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0;
    }
    return (uint64_t) counters.WorkingSetSize;
}

auto setThreadName(const char* name) -> void {
    SetThreadDescription(GetCurrentThread(), widen(name).c_str());
}

auto blockStopSignals() -> void {
    if (g_stopEvent.load() == nullptr) {
        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    SetConsoleCtrlHandler(consoleControl, TRUE);
}

auto waitForStopSignal() -> std::string {
    WaitForSingleObject(g_stopEvent.load(), INFINITE);
    switch (g_stopType.load()) {
        case CTRL_C_EVENT:
            return "Ctrl+C";
        case CTRL_BREAK_EVENT:
            return "Ctrl+Break";
        case CTRL_CLOSE_EVENT:
            return "console closed";
        default:
            return "console event";
    }
}

auto runProcess(const std::vector<std::string>& argv) -> ProcessResult {
    ProcessResult result;
    SECURITY_ATTRIBUTES security = {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &security, 0)) {
        result.output = "cannot create a pipe";
        return result;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    // The child inherits its pipe's write end and nothing else: a child started at the same time on another thread must not hold this pipe
    // open, or reading it would wait for that child too.
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<std::byte> attributeStorage(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize);
    std::array<HANDLE, 1> inherited = {writeEnd};
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), sizeof(HANDLE) * inherited.size(), nullptr, nullptr);

    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullptr;
    startup.StartupInfo.hStdOutput = writeEnd;
    startup.StartupInfo.hStdError = writeEnd;
    startup.lpAttributeList = attributes;

    auto line = commandLine(argv);
    PROCESS_INFORMATION info = {};
    BOOL created = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(writeEnd);
    if (!created) {
        CloseHandle(readEnd);
        result.output = "cannot run " + argv[0];
        return result;
    }
    CloseHandle(info.hThread);

    std::array<char, 4096> buffer = {};
    while (true) {
        DWORD count = 0;
        if (!ReadFile(readEnd, buffer.data(), (DWORD) buffer.size(), &count, nullptr) || count == 0) {
            break;
        }
        result.output.append(buffer.data(), count);
    }
    CloseHandle(readEnd);
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    result.exitCode = (int) code;
    return result;
}

auto runProcessInteractive(const std::filesystem::path& program, std::span<const std::string> args) -> int {
    std::vector<std::string> argv = {program.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    auto line = commandLine(argv);
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info = {};
    if (!CreateProcessW(program.wstring().c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
        return 127;
    }
    CloseHandle(info.hThread);
    // Ctrl+C reaches the child on the shared console; this process waits for it to finish rather than stopping first.
    SetConsoleCtrlHandler(nullptr, TRUE);
    WaitForSingleObject(info.hProcess, INFINITE);
    SetConsoleCtrlHandler(nullptr, FALSE);
    DWORD code = 127;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return (int) code;
}

} // namespace os
