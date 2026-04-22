#pragma once

#include "observation.h"
#include "observationbus.h"

#include <string>
#include <string_view>
#include <utility>

// OBS_EVENT(category, type, name).field(key, value).field(key, value);
//
// Returns a builder whose destructor emits to the process-wide bus. The chain
// is a single full-expression — the observation is submitted when the
// semicolon runs.
//
// Under -DOBSERVABILITY_DISABLED the macro expands to `while (false) NoopBuilder{}`,
// which preserves the fluent chain for compilation and type checking but elides
// every .field() call at runtime (and lets the optimizer drop the whole thing).
//
// When enabled but the category is silenced, the macro still short-circuits
// before constructing the builder — .field() arguments are not evaluated. Treat
// .field() arguments as pure (same rule as assert()).

namespace obs::detail {

// No-op replacement for Builder when observability is disabled. Accepts any
// argument via templated .field() without evaluating it meaningfully. Stays in
// a dead `while (false)` branch so arguments aren't even executed.
struct NoopBuilder {
    template <class T> constexpr NoopBuilder& field(std::string_view, const T&) noexcept { return *this; }
};

// Active builder. Accumulates fields into a stack-resident Observation and
// pushes it to the bus on destruction.
class Builder {
public:
    Builder(std::string_view category, std::string_view type, std::string_view name) {
        m_obs.category.assign(category);
        m_obs.type.assign(type);
        m_obs.name.assign(name);
    }

    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;
    Builder(Builder&&) = delete;
    Builder& operator=(Builder&&) = delete;

    ~Builder() { bus().emit(std::move(m_obs)); }

    Builder& field(std::string_view key, bool v) {
        m_obs.fields.push_back({std::string(key), FieldValue{v}});
        return *this;
    }
    Builder& field(std::string_view key, int v) {
        m_obs.fields.push_back({std::string(key), FieldValue{static_cast<int64_t>(v)}});
        return *this;
    }
    Builder& field(std::string_view key, unsigned v) {
        m_obs.fields.push_back({std::string(key), FieldValue{static_cast<int64_t>(v)}});
        return *this;
    }
    Builder& field(std::string_view key, long v) {
        m_obs.fields.push_back({std::string(key), FieldValue{static_cast<int64_t>(v)}});
        return *this;
    }
    Builder& field(std::string_view key, unsigned long v) {
        m_obs.fields.push_back({std::string(key), FieldValue{static_cast<int64_t>(v)}});
        return *this;
    }
    Builder& field(std::string_view key, long long v) {
        m_obs.fields.push_back({std::string(key), FieldValue{static_cast<int64_t>(v)}});
        return *this;
    }
    Builder& field(std::string_view key, unsigned long long v) {
        m_obs.fields.push_back({std::string(key), FieldValue{static_cast<int64_t>(v)}});
        return *this;
    }
    Builder& field(std::string_view key, float v) {
        m_obs.fields.push_back({std::string(key), FieldValue{static_cast<double>(v)}});
        return *this;
    }
    Builder& field(std::string_view key, double v) {
        m_obs.fields.push_back({std::string(key), FieldValue{v}});
        return *this;
    }
    Builder& field(std::string_view key, const char* v) {
        m_obs.fields.push_back({std::string(key), FieldValue{std::string(v ? v : "")}});
        return *this;
    }
    Builder& field(std::string_view key, std::string_view v) {
        m_obs.fields.push_back({std::string(key), FieldValue{std::string(v)}});
        return *this;
    }
    Builder& field(std::string_view key, const std::string& v) {
        m_obs.fields.push_back({std::string(key), FieldValue{v}});
        return *this;
    }

private:
    Observation m_obs;
};

} // namespace obs::detail

#ifdef OBSERVABILITY_DISABLED
#define OBS_EVENT(cat, type, name)                                                                                                                             \
    while (false)                                                                                                                                              \
        ::obs::detail::NoopBuilder {                                                                                                                           \
        }
#else
#define OBS_EVENT(cat, type, name)                                                                                                                             \
    if (!::obs::bus().categoryEnabled(cat)) {                                                                                                                  \
    } else                                                                                                                                                     \
        ::obs::detail::Builder {                                                                                                                               \
            (cat), (type), (name)                                                                                                                              \
        }
#endif
