#pragma once

#include "traceevent.h"
#include "tracering.h"

#include <string>
#include <string_view>
#include <utility>

// TRACE_EVENT(category, type, name).text(message).field(key, value);
// TRACE_WARNING(category, type, name).text(message);
// TRACE_ERROR(category, type, name).text(message);
//
// Builds one event and pushes it into the process's ring when the full expression ends (at the semicolon). Always
// on: the ring is in memory, and whoever subscribes over RPC gets the events (src/trace/README.md). The trace is for
// flow and messages: what happened, and what went wrong. Data that can be asked for is a record, not an event.
//
// Every event is also printed on the console, prefixed with the local time: info on stdout, warnings and errors on
// stderr. Engine code says everything through the trace, so the console and the trace never differ.
//
// For an event whose fields are added in a loop, name the builder: trace::Builder event(trace::Level::Info, "Scene", "SceneOpened", path);

namespace trace {

class Builder {
public:
    Builder(Level level, std::string_view category, std::string_view type, std::string_view name) {
        event.level = level;
        event.category.assign(category);
        event.type.assign(type);
        event.name.assign(name);
    }

    Builder(const Builder&) = delete;
    auto operator=(const Builder&) -> Builder& = delete;
    Builder(Builder&&) = delete;
    auto operator=(Builder&&) -> Builder& = delete;

    ~Builder();

    // The message for people. Fields still carry the values a reader filters on.
    auto text(std::string_view message) -> Builder& {
        event.text.assign(message);
        return *this;
    }

    auto field(std::string_view key, bool v) -> Builder& { return add(key, FieldValue{v}); }
    auto field(std::string_view key, int v) -> Builder& { return add(key, FieldValue{(int64_t) v}); }
    auto field(std::string_view key, unsigned v) -> Builder& { return add(key, FieldValue{(int64_t) v}); }
    auto field(std::string_view key, long v) -> Builder& { return add(key, FieldValue{(int64_t) v}); }
    auto field(std::string_view key, unsigned long v) -> Builder& { return add(key, FieldValue{(int64_t) v}); }
    auto field(std::string_view key, long long v) -> Builder& { return add(key, FieldValue{(int64_t) v}); }
    auto field(std::string_view key, unsigned long long v) -> Builder& { return add(key, FieldValue{(int64_t) v}); }
    auto field(std::string_view key, float v) -> Builder& { return add(key, FieldValue{(double) v}); }
    auto field(std::string_view key, double v) -> Builder& { return add(key, FieldValue{v}); }
    auto field(std::string_view key, const char* v) -> Builder& { return add(key, FieldValue{std::string(v != nullptr ? v : "")}); }
    auto field(std::string_view key, std::string_view v) -> Builder& { return add(key, FieldValue{std::string(v)}); }
    auto field(std::string_view key, const std::string& v) -> Builder& { return add(key, FieldValue{v}); }

private:
    auto add(std::string_view key, FieldValue value) -> Builder& {
        event.fields.push_back({std::string(key), std::move(value)});
        return *this;
    }

    Event event;
};

} // namespace trace

#define TRACE_EVENT(category, type, name)                \
    ::trace::Builder {                                   \
        ::trace::Level::Info, (category), (type), (name) \
    }
#define TRACE_WARNING(category, type, name)                 \
    ::trace::Builder {                                      \
        ::trace::Level::Warning, (category), (type), (name) \
    }
#define TRACE_ERROR(category, type, name)                 \
    ::trace::Builder {                                    \
        ::trace::Level::Error, (category), (type), (name) \
    }
