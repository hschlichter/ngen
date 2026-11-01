#pragma once

#include <string>

// Prim paths as the World, the view's placement and the tools write them: "/" is the root, "/World/Tree" a prim. Variant selections
// in a path ("/asset{shape=a}Geom") are the composer's, which has its own rules for them.
namespace primpath {

// "/World" and "Tree" → "/World/Tree".
auto child(const std::string& parent, const std::string& name) -> std::string;
// A path relative to a content's root ("/" for the root itself) made full where the content is placed: "/World" and "/Tree/Leaf" →
// "/World/Tree/Leaf".
auto join(const std::string& base, const std::string& relative) -> std::string;
// Whether `path` is `base` or under it; everything is under "/".
auto isAtOrUnder(const std::string& path, const std::string& base) -> bool;

} // namespace primpath
