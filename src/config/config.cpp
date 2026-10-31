#include "config.h"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <format>
#include <thread>

namespace config {

namespace {

// The registry: a function-local static, so values declared in any translation unit register whatever the order they are constructed in.
auto registry() -> std::vector<Value*>& {
    static std::vector<Value*> values;
    return values;
}

auto trim(std::string_view text) -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

auto parseDouble(std::string_view text) -> std::optional<double> {
    text = trim(text);
    double value = 0.0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

auto rangeText(const Range& range) -> std::string {
    if (range.min && range.max) {
        return std::format("{} to {}", *range.min, *range.max);
    }
    if (range.min) {
        return std::format("at least {}", *range.min);
    }
    return std::format("at most {}", *range.max);
}

auto checkRange(const std::string& name, double value, const Range& range) -> std::expected<void, std::string> {
    if ((range.min && value < *range.min) || (range.max && value > *range.max)) {
        return std::unexpected(std::format("{}: {} is outside {}", name, value, rangeText(range)));
    }
    return {};
}

// A float written so it reads back the same, and short: 0.5, not 0.500000.
auto floatText(double value) -> std::string {
    return std::format("{}", value);
}

} // namespace

auto typeName(Type type) -> const char* {
    switch (type) {
        case Type::Bool:
            return "bool";
        case Type::Int:
            return "int";
        case Type::Float:
            return "float";
        case Type::Enum:
            return "enum";
        case Type::Vec3:
            return "vec3";
    }
    return "unknown";
}

Value::Value(std::string name, std::string description) : valueName(std::move(name)), valueDescription(std::move(description)) {
    auto& values = registry();
    auto at = std::ranges::lower_bound(values, valueName, {}, &Value::name);
    assert((at == values.end() || (*at)->name() != valueName) && "a configuration value's name is declared twice");
    values.insert(at, this);
}

Value::~Value() {
    auto& values = registry();
    std::erase(values, this);
}

auto Value::checkThread() const -> void {
#ifndef NDEBUG
    // Values are the main thread's: the first thread to read or set one owns them all.
    static const std::thread::id owner = std::this_thread::get_id();
    assert(std::this_thread::get_id() == owner && "configuration values are read and set on the main thread only");
#endif
}

auto Value::changed() -> void {
    changeCount++;
}

Bool::Bool(std::string name, bool defaultValue, std::string description) : Value(std::move(name), std::move(description)), value(defaultValue), defaultValue(defaultValue) {
}

auto Bool::get() const -> bool {
    checkThread();
    return value;
}

auto Bool::set(bool next) -> void {
    checkThread();
    if (next != value) {
        value = next;
        changed();
    }
}

auto Bool::text() const -> std::string {
    return value ? "true" : "false";
}

auto Bool::defaultText() const -> std::string {
    return defaultValue ? "true" : "false";
}

auto Bool::setText(std::string_view text) -> std::expected<void, std::string> {
    text = trim(text);
    if (text == "true" || text == "on" || text == "1") {
        set(true);
        return {};
    }
    if (text == "false" || text == "off" || text == "0") {
        set(false);
        return {};
    }
    return std::unexpected(std::format("{}: expected true or false, got '{}'", name(), text));
}

Int::Int(std::string name, int64_t defaultValue, std::string description, Range range)
    : Value(std::move(name), std::move(description)), value(defaultValue), defaultValue(defaultValue), valueRange(range) {
}

auto Int::get() const -> int64_t {
    checkThread();
    return value;
}

auto Int::set(int64_t next) -> std::expected<void, std::string> {
    checkThread();
    if (auto inRange = checkRange(name(), (double) next, valueRange); !inRange) {
        return inRange;
    }
    if (next != value) {
        value = next;
        changed();
    }
    return {};
}

auto Int::text() const -> std::string {
    return std::to_string(value);
}

auto Int::defaultText() const -> std::string {
    return std::to_string(defaultValue);
}

auto Int::setText(std::string_view text) -> std::expected<void, std::string> {
    text = trim(text);
    int64_t parsed = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::unexpected(std::format("{}: expected a whole number, got '{}'", name(), text));
    }
    return set(parsed);
}

auto Int::reset() -> void {
    (void) set(defaultValue);
}

Float::Float(std::string name, double defaultValue, std::string description, Range range)
    : Value(std::move(name), std::move(description)), value(defaultValue), defaultValue(defaultValue), valueRange(range) {
}

auto Float::get() const -> double {
    checkThread();
    return value;
}

auto Float::set(double next) -> std::expected<void, std::string> {
    checkThread();
    if (!std::isfinite(next)) {
        return std::unexpected(std::format("{}: not a finite number", name()));
    }
    if (auto inRange = checkRange(name(), next, valueRange); !inRange) {
        return inRange;
    }
    if (next != value) {
        value = next;
        changed();
    }
    return {};
}

auto Float::text() const -> std::string {
    return floatText(value);
}

auto Float::defaultText() const -> std::string {
    return floatText(defaultValue);
}

auto Float::setText(std::string_view text) -> std::expected<void, std::string> {
    auto parsed = parseDouble(text);
    if (!parsed) {
        return std::unexpected(std::format("{}: expected a number, got '{}'", name(), trim(text)));
    }
    return set(*parsed);
}

auto Float::reset() -> void {
    (void) set(defaultValue);
}

Enum::Enum(std::string name, std::vector<std::string> choices, size_t defaultIndex, std::string description)
    : Value(std::move(name), std::move(description)), names(std::move(choices)), value(defaultIndex), defaultValue(defaultIndex) {
    assert(defaultIndex < names.size());
}

auto Enum::get() const -> size_t {
    checkThread();
    return value;
}

auto Enum::choice() const -> const std::string& {
    checkThread();
    return names[value];
}

auto Enum::set(size_t index) -> std::expected<void, std::string> {
    checkThread();
    if (index >= names.size()) {
        return std::unexpected(std::format("{}: choice {} past the {} choices", name(), index, names.size()));
    }
    if (index != value) {
        value = index;
        changed();
    }
    return {};
}

auto Enum::defaultText() const -> std::string {
    return names[defaultValue];
}

auto Enum::setText(std::string_view text) -> std::expected<void, std::string> {
    text = trim(text);
    auto found = std::ranges::find(names, text);
    if (found == names.end()) {
        std::string list;
        for (const auto& choice : names) {
            list += list.empty() ? choice : ", " + choice;
        }
        return std::unexpected(std::format("{}: '{}' isn't one of {}", name(), text, list));
    }
    return set((size_t) (found - names.begin()));
}

auto Enum::reset() -> void {
    (void) set(defaultValue);
}

Vec3::Vec3(std::string name, std::array<float, 3> defaultValue, std::string description)
    : Value(std::move(name), std::move(description)), value(defaultValue), defaultValue(defaultValue) {
}

auto Vec3::get() const -> std::array<float, 3> {
    checkThread();
    return value;
}

auto Vec3::set(std::array<float, 3> next) -> void {
    checkThread();
    if (next != value) {
        value = next;
        changed();
    }
}

auto Vec3::text() const -> std::string {
    return std::format("{},{},{}", floatText(value[0]), floatText(value[1]), floatText(value[2]));
}

auto Vec3::defaultText() const -> std::string {
    return std::format("{},{},{}", floatText(defaultValue[0]), floatText(defaultValue[1]), floatText(defaultValue[2]));
}

auto Vec3::setText(std::string_view text) -> std::expected<void, std::string> {
    std::array<float, 3> parsed = {};
    for (size_t i = 0; i < 3; i++) {
        auto comma = text.find(',');
        auto part = i < 2 ? text.substr(0, comma) : text;
        auto number = parseDouble(part);
        if (!number || (i < 2 && comma == std::string_view::npos)) {
            return std::unexpected(std::format("{}: expected x,y,z, got '{}'", name(), trim(text)));
        }
        parsed[i] = (float) *number;
        if (i < 2) {
            text.remove_prefix(comma + 1);
        }
    }
    set(parsed);
    return {};
}

auto all() -> const std::vector<Value*>& {
    return registry();
}

auto find(std::string_view name) -> Value* {
    const auto& values = registry();
    auto at = std::ranges::lower_bound(values, name, {}, [](const Value* value) -> std::string_view { return value->name(); });
    return at != values.end() && (*at)->name() == name ? *at : nullptr;
}

auto under(std::string_view prefix) -> std::vector<Value*> {
    std::vector<Value*> out;
    for (auto* value : registry()) {
        const auto& name = value->name();
        if (prefix.empty() || name == prefix || (name.starts_with(prefix) && name.size() > prefix.size() && name[prefix.size()] == '.')) {
            out.push_back(value);
        }
    }
    return out;
}

} // namespace config
