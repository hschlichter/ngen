#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace trace {

// How much an event matters: Info narrates flow, Warning is something that went wrong but was worked around, Error is
// something that failed.
enum class Level : uint8_t {
    Info,
    Warning,
    Error,
};

// "info", "warning", "error": the level as it travels.
auto levelName(Level level) -> const char*;

using FieldValue = std::variant<bool, int64_t, double, std::string>;

struct Field {
    std::string key;
    FieldValue value;
};

// One structured event. The ring assigns seq, and the time and thread when the producer left them empty.
struct Event {
    uint64_t seq = 0;  // per process, counting from 1
    uint64_t tsNs = 0; // CLOCK_MONOTONIC, which every process on the machine shares
    std::string thread;
    Level level = Level::Info;
    std::string category;
    std::string type;
    std::string name;
    std::string text; // the message for people; may be empty
    std::vector<Field> fields;
};

// CLOCK_MONOTONIC in nanoseconds; the profiler's clock too.
auto now() -> uint64_t;

// The calling thread's label in events.
auto threadLabel() -> const std::string&;

} // namespace trace
