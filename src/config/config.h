#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Configuration (src/config/README.md): the engine's settings, each a typed value declared in code by the system it configures, with a
// dotted name, a default and a description. Read and set on the main thread only; a value counts its changes, so code that acts on one
// compares the count it last saw.
namespace config {

enum class Type : uint8_t {
    Bool,
    Int,
    Float,
    Enum,
    Vec3,
};

auto typeName(Type type) -> const char*;

struct Range {
    std::optional<double> min;
    std::optional<double> max;
};

// What every value has, whatever its type: what the record, RPC and the Configuration window read. Declared values register themselves
// by name when constructed and leave the registry when destroyed.
class Value {
public:
    Value(std::string name, std::string description);
    virtual ~Value();
    Value(const Value&) = delete;
    auto operator=(const Value&) -> Value& = delete;

    auto name() const -> const std::string& { return valueName; }
    auto description() const -> const std::string& { return valueDescription; }
    // Counts every set that changed it, a reset included.
    auto changes() const -> uint64_t { return changeCount; }
    auto isDefault() const -> bool { return text() == defaultText(); }

    virtual auto type() const -> Type = 0;
    // The value as the verb takes it and a file would hold it: true/false, a number, a choice's name, "x,y,z".
    virtual auto text() const -> std::string = 0;
    virtual auto defaultText() const -> std::string = 0;
    // Parses and checks (range, choices); unchanged on an error, which says why.
    virtual auto setText(std::string_view text) -> std::expected<void, std::string> = 0;
    virtual auto reset() -> void = 0;

protected:
    auto checkThread() const -> void;
    auto changed() -> void;

private:
    std::string valueName;
    std::string valueDescription;
    uint64_t changeCount = 0;
};

class Bool : public Value {
public:
    Bool(std::string name, bool defaultValue, std::string description);
    auto get() const -> bool;
    auto set(bool next) -> void;

    auto type() const -> Type override { return Type::Bool; }
    auto text() const -> std::string override;
    auto defaultText() const -> std::string override;
    auto setText(std::string_view text) -> std::expected<void, std::string> override;
    auto reset() -> void override { set(defaultValue); }

private:
    bool value;
    bool defaultValue;
};

class Int : public Value {
public:
    Int(std::string name, int64_t defaultValue, std::string description, Range range = {});
    auto get() const -> int64_t;
    auto set(int64_t next) -> std::expected<void, std::string>;
    auto range() const -> const Range& { return valueRange; }

    auto type() const -> Type override { return Type::Int; }
    auto text() const -> std::string override;
    auto defaultText() const -> std::string override;
    auto setText(std::string_view text) -> std::expected<void, std::string> override;
    auto reset() -> void override;

private:
    int64_t value;
    int64_t defaultValue;
    Range valueRange;
};

class Float : public Value {
public:
    Float(std::string name, double defaultValue, std::string description, Range range = {});
    auto get() const -> double;
    auto set(double next) -> std::expected<void, std::string>;
    auto range() const -> const Range& { return valueRange; }

    auto type() const -> Type override { return Type::Float; }
    auto text() const -> std::string override;
    auto defaultText() const -> std::string override;
    auto setText(std::string_view text) -> std::expected<void, std::string> override;
    auto reset() -> void override;

private:
    double value;
    double defaultValue;
    Range valueRange;
};

// One of named choices, held as its index.
class Enum : public Value {
public:
    Enum(std::string name, std::vector<std::string> choices, size_t defaultIndex, std::string description);
    auto get() const -> size_t;
    auto choice() const -> const std::string&;
    auto set(size_t index) -> std::expected<void, std::string>;
    auto choices() const -> const std::vector<std::string>& { return names; }

    auto type() const -> Type override { return Type::Enum; }
    auto text() const -> std::string override { return choice(); }
    auto defaultText() const -> std::string override;
    auto setText(std::string_view text) -> std::expected<void, std::string> override;
    auto reset() -> void override;

private:
    std::vector<std::string> names;
    size_t value;
    size_t defaultValue;
};

// Three floats: a colour or a vector.
class Vec3 : public Value {
public:
    Vec3(std::string name, std::array<float, 3> defaultValue, std::string description);
    auto get() const -> std::array<float, 3>;
    auto set(std::array<float, 3> next) -> void;

    auto type() const -> Type override { return Type::Vec3; }
    auto text() const -> std::string override;
    auto defaultText() const -> std::string override;
    auto setText(std::string_view text) -> std::expected<void, std::string> override;
    auto reset() -> void override { set(defaultValue); }

private:
    std::array<float, 3> value;
    std::array<float, 3> defaultValue;
};

// Every value declared, sorted by name.
auto all() -> const std::vector<Value*>&;
auto find(std::string_view name) -> Value*;
// The values whose names are `prefix` or start with `prefix` followed by a dot; every value for an empty prefix.
auto under(std::string_view prefix) -> std::vector<Value*>;

} // namespace config
