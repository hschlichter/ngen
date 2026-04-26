#pragma once

#include "backend.hpp"
#include "path.hpp"
#include "target.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace build {

class Tool final : public Target {
public:
    using OutputFor = std::function<Path(const BuildVariant&, const Path&)>;

    using Target::Target;
    std::string kind() const override { return "tool"; }

    Tool& command(std::vector<std::string> argv) { argv_template = std::move(argv); return *this; }
    Tool& inputs(std::vector<Path> paths) { tool_inputs = std::move(paths); return *this; }
    Tool& outputs(std::vector<Path> paths) { tool_outputs = std::move(paths); return *this; }
    Tool& for_each(std::vector<Path> paths, OutputFor fn) { tool_inputs = std::move(paths); output_for = std::move(fn); return *this; }

    std::vector<std::string> argv_template;
    std::vector<Path> tool_inputs;
    std::vector<Path> tool_outputs;
    OutputFor output_for;
};

} // namespace build
