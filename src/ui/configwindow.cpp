#include "configwindow.h"

#include "config.h"

#include <imgui.h>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

// A value's group: the first two parts of its name ("render.shadow" for "render.shadow.cascades").
auto groupOf(const std::string& name) -> std::string_view {
    auto first = name.find('.');
    if (first == std::string::npos) {
        return name;
    }
    auto second = name.find('.', first + 1);
    return std::string_view(name).substr(0, second == std::string::npos ? first : second);
}

auto drawEditor(config::Value& value) -> void {
    auto label = std::format("##{}", value.name());
    ImGui::SetNextItemWidth(-FLT_MIN);
    switch (value.type()) {
        case config::Type::Bool: {
            auto& typed = static_cast<config::Bool&>(value);
            bool on = typed.get();
            if (ImGui::Checkbox(label.c_str(), &on)) {
                typed.set(on);
            }
            break;
        }
        case config::Type::Int: {
            auto& typed = static_cast<config::Int&>(value);
            int64_t number = typed.get();
            const auto& range = typed.range();
            bool edited = false;
            if (range.min && range.max) {
                auto min = (int64_t) *range.min;
                auto max = (int64_t) *range.max;
                edited = ImGui::SliderScalar(label.c_str(), ImGuiDataType_S64, &number, &min, &max);
            } else {
                edited = ImGui::InputScalar(label.c_str(), ImGuiDataType_S64, &number);
            }
            if (edited) {
                (void) typed.set(number);
            }
            break;
        }
        case config::Type::Float: {
            auto& typed = static_cast<config::Float&>(value);
            double number = typed.get();
            const auto& range = typed.range();
            bool edited = false;
            if (range.min && range.max) {
                double min = *range.min;
                double max = *range.max;
                // A range over three orders of magnitude (a distance) slides logarithmically; a range from 0 can't.
                auto flags = min > 0.0 && max / min >= 1000.0 ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None;
                edited = ImGui::SliderScalar(label.c_str(), ImGuiDataType_Double, &number, &min, &max, "%.3g", flags);
            } else {
                edited = ImGui::InputDouble(label.c_str(), &number, 0.0, 0.0, "%.3g");
            }
            if (edited) {
                (void) typed.set(number);
            }
            break;
        }
        case config::Type::Enum: {
            auto& typed = static_cast<config::Enum&>(value);
            if (ImGui::BeginCombo(label.c_str(), typed.choice().c_str())) {
                const auto& choices = typed.choices();
                for (size_t i = 0; i < choices.size(); i++) {
                    if (ImGui::Selectable(choices[i].c_str(), i == typed.get())) {
                        (void) typed.set(i);
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case config::Type::Vec3: {
            auto& typed = static_cast<config::Vec3&>(value);
            auto v = typed.get();
            if (ImGui::DragFloat3(label.c_str(), v.data(), 0.005f, 0.0f, 0.0f, "%.3f")) {
                typed.set(v);
            }
            break;
        }
    }
}

} // namespace

auto drawConfigWindow(bool& show, ConfigWindowState& state) -> void {
    if (!show) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Configuration", &show)) {
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##filter", "filter by name", state.filter.data(), state.filter.size());
    ImGui::SameLine();
    ImGui::Checkbox("changed only", &state.onlyChanged);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu values", config::all().size());

    std::string_view filter(state.filter.data());
    // Groups in name order, each value under its own group: two-part names ("render.prepass") sort between three-part groups.
    std::vector<std::string_view> groups;
    for (auto* value : config::all()) {
        auto group = groupOf(value->name());
        if (std::ranges::find(groups, group) == groups.end()) {
            groups.push_back(group);
        }
    }
    std::ranges::sort(groups);
    if (ImGui::BeginTable("##config", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("reset", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        for (auto group : groups) {
            auto shown = [&](const config::Value* value) {
                const auto& name = value->name();
                return groupOf(name) == group && (filter.empty() || name.find(filter) != std::string::npos) && (!state.onlyChanged || !value->isDefault());
            };
            if (std::ranges::none_of(config::all(), shown)) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool open = ImGui::TreeNodeEx(std::string(group).c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_NoTreePushOnOpen);
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton(std::format("all##{}", group).c_str())) {
                for (auto* value : config::all()) {
                    if (groupOf(value->name()) == group) {
                        value->reset();
                    }
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Put every value in %s back to its default.", std::string(group).c_str());
            }
            if (!open) {
                continue;
            }
            for (auto* value : config::all()) {
                if (!shown(value)) {
                    continue;
                }
                const auto& name = value->name();
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                auto shortName = std::string_view(name).substr(std::min(name.size(), group.size() + 1));
                if (value->isDefault()) {
                    ImGui::Text("  %.*s", (int) shortName.size(), shortName.data());
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "* %.*s", (int) shortName.size(), shortName.data());
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\n\n%s (%s), default %s", value->description().c_str(), name.c_str(), config::typeName(value->type()), value->defaultText().c_str());
                }
                ImGui::TableSetColumnIndex(1);
                drawEditor(*value);
                ImGui::TableSetColumnIndex(2);
                if (!value->isDefault() && ImGui::SmallButton(std::format("reset##{}", name).c_str())) {
                    value->reset();
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
