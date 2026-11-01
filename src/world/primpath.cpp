#include "primpath.h"

namespace primpath {

auto child(const std::string& parent, const std::string& name) -> std::string {
    if (parent == "/") {
        return "/" + name;
    }
    return parent + "/" + name;
}

auto join(const std::string& base, const std::string& relative) -> std::string {
    if (relative == "/") {
        return base;
    }
    if (base == "/") {
        return relative;
    }
    return base + relative;
}

auto isAtOrUnder(const std::string& path, const std::string& base) -> bool {
    if (base == "/") {
        return true;
    }
    return path == base || (path.size() > base.size() && path.starts_with(base) && path[base.size()] == '/');
}

} // namespace primpath
