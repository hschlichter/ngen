#include "sessionscript.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>

namespace {

auto trim(std::string_view s) -> std::string_view {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

auto SessionScript::addLine(std::string_view rawLine, std::string& error) -> bool {
    auto line = trim(rawLine);
    if (line.empty() || line.front() == '#') {
        return true;
    }
    auto space = line.find(' ');
    if (space == std::string_view::npos) {
        error = "expected '<frame> <verb> [args]'";
        return false;
    }
    uint64_t frame = 0;
    auto frameText = line.substr(0, space);
    auto [ptr, ec] = std::from_chars(frameText.data(), frameText.data() + frameText.size(), frame);
    if (ec != std::errc{} || ptr != frameText.data() + frameText.size()) {
        error = "frame number expected before the verb";
        return false;
    }
    auto rest = trim(line.substr(space + 1));
    auto verbEnd = rest.find(' ');
    auto verb = rest.substr(0, verbEnd);
    auto args = verbEnd == std::string_view::npos ? std::string_view{} : trim(rest.substr(verbEnd + 1));
    add(frame, std::string(verb), std::string(args));
    return true;
}

auto SessionScript::add(uint64_t frame, std::string verb, std::string args) -> void {
    pending.push_back({.frame = frame, .verb = std::move(verb), .args = std::move(args)});
    std::stable_sort(pending.begin(), pending.end(), [](const SessionCommand& a, const SessionCommand& b) { return a.frame < b.frame; });
}

auto SessionScript::loadFile(const char* path, std::string& error) -> bool {
    std::ifstream file(path);
    if (!file) {
        error = std::string("cannot open ") + path;
        return false;
    }
    std::string line;
    int lineNumber = 0;
    while (std::getline(file, line)) {
        lineNumber++;
        std::string lineError;
        if (!addLine(line, lineError)) {
            error = std::string(path) + ":" + std::to_string(lineNumber) + ": " + lineError;
            return false;
        }
    }
    return true;
}

auto SessionScript::takeDue(uint64_t frame, std::vector<SessionCommand>& out) -> void {
    out.clear();
    auto it = pending.begin();
    while (it != pending.end() && it->frame <= frame) {
        out.push_back(std::move(*it));
        ++it;
    }
    pending.erase(pending.begin(), it);
}

auto SessionScript::lastFrame() const -> uint64_t {
    return pending.empty() ? 0 : pending.back().frame;
}

auto parseCameraPose(std::string_view text, float out[5]) -> bool {
    size_t start = 0;
    for (int i = 0; i < 5; i++) {
        auto comma = text.find(',', start);
        auto part = trim(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        float value = 0.0f;
        auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (ec != std::errc{} || ptr != part.data() + part.size()) {
            return false;
        }
        out[i] = value;
        if (comma == std::string_view::npos) {
            return i == 4;
        }
        start = comma + 1;
    }
    return true;
}

auto formatCameraPose(const float pose[5]) -> std::string {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f,%.2f,%.2f", pose[0], pose[1], pose[2], pose[3], pose[4]);
    return buf;
}
