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
    auto kind() const -> std::string override { return "tool"; }

    auto command(std::vector<std::string> argv) -> Tool& {
        argv_template = std::move(argv);
        return *this;
    }
    auto inputs(std::vector<Path> paths) -> Tool& {
        tool_inputs = std::move(paths);
        return *this;
    }
    auto outputs(std::vector<Path> paths) -> Tool& {
        tool_outputs = std::move(paths);
        return *this;
    }
    auto for_each(std::vector<Path> paths, OutputFor fn) -> Tool& {
        tool_inputs = std::move(paths);
        output_for = std::move(fn);
        return *this;
    }
    auto global(bool flag = true) -> Tool& {
        is_global = flag;
        return *this;
    }

    std::vector<std::string> argv_template;
    std::vector<Path> tool_inputs;
    std::vector<Path> tool_outputs;
    OutputFor output_for;
    bool is_global = false;
};

} // namespace build
