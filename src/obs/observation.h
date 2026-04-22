#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace obs {

using FieldValue = std::variant<bool, int64_t, double, std::string>;

struct KeyValue {
    std::string key;
    FieldValue value;
};

struct Observation {
    uint64_t ts_ns = 0;
    std::string thread;
    std::string category;
    std::string type;
    std::string name;
    std::vector<KeyValue> fields;
};

} // namespace obs
