#include "jsonlinesfilesink.h"

#include <chrono>
#include <format>

namespace obs {

namespace {

// Escapes a UTF-8 string for embedding in a JSON string literal. Handles the
// mandatory escapes (\", \\, control chars < 0x20); high bytes are passed
// through as-is (the stream is UTF-8, which is valid inside JSON).
auto appendEscaped(std::string& out, std::string_view s) -> void {
    for (char c : s) {
        auto uc = static_cast<unsigned char>(c);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (uc < 0x20) {
                    std::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(uc));
                } else {
                    out += c;
                }
        }
    }
}

auto appendFieldValue(std::string& out, const FieldValue& v) -> void {
    if (std::holds_alternative<bool>(v)) {
        out += std::get<bool>(v) ? "true" : "false";
    } else if (std::holds_alternative<int64_t>(v)) {
        std::format_to(std::back_inserter(out), "{}", std::get<int64_t>(v));
    } else if (std::holds_alternative<double>(v)) {
        std::format_to(std::back_inserter(out), "{}", std::get<double>(v));
    } else {
        out += '"';
        appendEscaped(out, std::get<std::string>(v));
        out += '"';
    }
}

} // namespace

bool JsonLinesFileSink::open(const std::string& path) {
    m_stream.open(path, std::ios::out | std::ios::trunc);
    if (!m_stream.is_open()) {
        return false;
    }
    // Metadata record: schema identifier, schema version, wall-clock start.
    // Subsequent ts_ns values are monotonic since program start — wall time is
    // only in this header so readers can correlate runs.
    auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string line;
    line.reserve(128);
    line += R"({"schema":"ngen.obs","schema_version":1,"started_at_ns":)";
    line += std::to_string(wall);
    line += "}\n";
    m_stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    return true;
}

void JsonLinesFileSink::write(const Observation& obs) {
    if (!m_stream.is_open()) {
        return;
    }
    std::string line;
    line.reserve(256);
    line += R"({"ts_ns":)";
    line += std::to_string(obs.ts_ns);
    line += R"(,"thread":")";
    appendEscaped(line, obs.thread);
    line += R"(","category":")";
    appendEscaped(line, obs.category);
    line += R"(","type":")";
    appendEscaped(line, obs.type);
    line += R"(","name":")";
    appendEscaped(line, obs.name);
    line += R"(","fields":{)";
    bool first = true;
    for (const auto& kv : obs.fields) {
        if (!first) {
            line += ',';
        }
        first = false;
        line += '"';
        appendEscaped(line, kv.key);
        line += "\":";
        appendFieldValue(line, kv.value);
    }
    line += "}}\n";
    m_stream.write(line.data(), static_cast<std::streamsize>(line.size()));
}

void JsonLinesFileSink::flush() {
    if (m_stream.is_open()) {
        m_stream.flush();
    }
}

} // namespace obs
