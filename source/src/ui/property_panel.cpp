#include "ui/property_panel.h"

#include "engine/node_registry.h"
#include "ui/editor_state.h"
#include "utils/file_dialog.h"
#include "utils/log.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace aiwrite::ui {
namespace {

using engine::Node;
using engine::Param;
using engine::ParamType;

std::vector<utils::FileFilter> filters_for(const Node& node)
{
    if (node.type == "ImageInput") {
        return {{"图片文件", "png,jpg,jpeg,bmp,webp"}, {"所有文件", "*"}};
    }
    return {{"所有文件", "*"}};
}

// 校验当前参数，返回错误文案（空 = 通过）
std::string param_error(const Param& param)
{
    std::string error;
    engine::Graph::validateParam(param, &error);
    return error;
}

void help_marker(const std::string& description)
{
    if (description.empty()) {
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

std::string param_label(const Param& param)
{
    return param.display_name + (param.is_required ? " *" : "");
}

// 绘制单个参数控件；返回 true 表示本次编辑结束且值已变化
// begin_edit：控件刚被激活（此时值尚未变化，外层可安全压撤销快照）
bool draw_param_widget(Node& node, Param& param, bool& begin_edit)
{
    bool edited             = false;
    const std::string label = param_label(param);
    (void)label;

    const auto note_activation = [&begin_edit]() {
        if (ImGui::IsItemActivated()) {
            begin_edit = true;
        }
    };

    ImGui::PushID(param.id.c_str());

    switch (param.type) {
    case ParamType::String: {
        std::string value         = param.text();
        ImGuiInputTextFlags flags = 0;
        if (param.is_secret) {
            flags |= ImGuiInputTextFlags_Password;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##value", param.display_name.c_str(), &value, flags) &&
            ImGui::IsItemDeactivatedAfterEdit()) {
            param.value = value;
            edited      = true;
        }
        note_activation();
        break;
    }
    case ParamType::Text: {
        std::string value = param.text();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextMultiline("##value", &value,
                                      ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5.0f)) &&
            ImGui::IsItemDeactivatedAfterEdit()) {
            param.value = value;
            edited      = true;
        }
        note_activation();
        break;
    }
    case ParamType::Int: {
        const double fallback =
            param.default_value.is_number() ? param.default_value.get<double>() : 0.0;
        int value = static_cast<int>(std::lround(param.number(fallback)));
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool has_range = param.min_value.has_value() && param.max_value.has_value();
        const bool touched =
            has_range ? ImGui::SliderInt("##value", &value, static_cast<int>(*param.min_value),
                                         static_cast<int>(*param.max_value))
                      : ImGui::InputInt("##value", &value);
        if (touched && ImGui::IsItemDeactivatedAfterEdit()) {
            param.value = value;
            edited      = true;
        }
        note_activation();
        break;
    }
    case ParamType::Float: {
        const double fallback =
            param.default_value.is_number() ? param.default_value.get<double>() : 0.0;
        float value = static_cast<float>(param.number(fallback));
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool has_range = param.min_value.has_value() && param.max_value.has_value();
        const bool touched =
            has_range ? ImGui::SliderFloat("##value", &value, static_cast<float>(*param.min_value),
                                           static_cast<float>(*param.max_value), "%.2f")
                      : ImGui::InputFloat("##value", &value, 0.1f, 1.0f, "%.2f");
        if (touched && ImGui::IsItemDeactivatedAfterEdit()) {
            param.value = value;
            edited      = true;
        }
        note_activation();
        break;
    }
    case ParamType::Bool: {
        bool value = param.flag();
        if (ImGui::Checkbox("##value", &value) && ImGui::IsItemDeactivatedAfterEdit()) {
            param.value = value;
            edited      = true;
        }
        note_activation();
        break;
    }
    case ParamType::Enum: {
        const std::string current = param.text();
        int index                 = 0;
        for (std::size_t i = 0; i < param.enum_options.size(); ++i) {
            if (param.enum_options[i] == current) {
                index = static_cast<int>(i);
                break;
            }
        }
        std::vector<const char*> items;
        items.reserve(param.enum_options.size());
        for (const std::string& option : param.enum_options) {
            items.push_back(option.c_str());
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        // 注意：Combo 在"点击下拉项"的当帧就返回 true，而该帧的 IsItemDeactivatedAfterEdit()
        // 并不成立（被点击的是弹出层里的选项，属于另一个 item），
        // 因此这里必须"值变化即写回"，否则会出现「选了 Web 但模式没变」的现象。
        if (!items.empty() &&
            ImGui::Combo("##value", &index, items.data(), static_cast<int>(items.size()))) {
            const int safe_index =
                std::clamp(index, 0, static_cast<int>(param.enum_options.size()) - 1);
            param.value = param.enum_options[static_cast<std::size_t>(safe_index)];
            edited      = true;
        }
        note_activation();
        break;
    }
    case ParamType::File:
    case ParamType::Directory: {
        std::string value = param.text();
        ImGui::SetNextItemWidth(-FLT_MIN - 90.0f);
        if (ImGui::InputTextWithHint("##value", "(未选择)", &value) &&
            ImGui::IsItemDeactivatedAfterEdit()) {
            param.value = value;
            edited      = true;
        }
        note_activation();
        ImGui::SameLine();
        if (ImGui::Button(param.type == ParamType::File ? "浏览…" : "选择…", ImVec2(80.0f, 0.0f))) {
            const std::string picked = (param.type == ParamType::File)
                                           ? utils::open_file(filters_for(node), param.text())
                                           : utils::pick_folder(param.text());
            if (!picked.empty()) {
                param.value = picked;
                edited      = true;
            }
        }
        break;
    }
    case ParamType::Color: {
        // 以 "#RRGGBB" 字符串存储
        const std::string hex = param.text();
        float rgb[3]          = {1.0f, 1.0f, 1.0f};
        if (hex.size() == 7 && hex[0] == '#') {
            for (int i = 0; i < 3; ++i) {
                rgb[i] = static_cast<float>(
                    std::strtol(hex.substr(static_cast<std::size_t>(1 + i * 2), 2).c_str(), nullptr, 16) /
                    255.0);
            }
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::ColorEdit3("##value", rgb) && ImGui::IsItemDeactivatedAfterEdit()) {
            char buffer[16] = {};
            std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X",
                          static_cast<int>(rgb[0] * 255.0f), static_cast<int>(rgb[1] * 255.0f),
                          static_cast<int>(rgb[2] * 255.0f));
            param.value = std::string(buffer);
            edited      = true;
        }
        note_activation();
        break;
    }
    }

    ImGui::PopID();
    return edited;
}

} // namespace

void draw_property_panel(const char* window_title, bool* open, Node* node,
                         PropertyEditResult& result)
{
    if (open != nullptr && !*open) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(380.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(window_title, open)) {
        ImGui::End();
        return;
    }

    if (node == nullptr) {
        ImGui::TextDisabled("未选中节点");
        ImGui::Separator();
        ImGui::TextWrapped("在画布中单击一个节点即可编辑其参数；"
                           "Ctrl+单击 或框选可多选，右键菜单可复制 / 删除。");
        ImGui::End();
        return;
    }

    engine::registerAllNodes();
    const engine::Definition* definition = engine::NodeRegistry::instance().find(node->type);

    // ---- 概览 ----
    ImGui::Text("节点 %s", node->id.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", node->type.c_str());
    ImGui::TextDisabled("分类: %s    状态: %s",
                        definition != nullptr ? engine::categoryName(definition->category) : "未知",
                        engine::nodeStateName(node->state));

    ImGui::TextUnformatted("标题");
    if (editor().request_focus_title) { // 双击节点 / 右键"改名" → 自动聚焦
        ImGui::SetKeyboardFocusHere();
        editor().request_focus_title = false;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##title", "节点标题", &node->title) &&
        ImGui::IsItemDeactivatedAfterEdit()) {
        result.renamed = true;
    }
    if (ImGui::IsItemActivated()) {
        result.begin_edit = true; // 改名前的快照
    }

    if (definition != nullptr && !definition->description.empty()) {
        ImGui::TextWrapped("%s", definition->description.c_str());
    }

    // ---- 校验汇总（设计 §6.3）----
    std::vector<std::string> errors;
    for (const Param& param : node->params) {
        const std::string error = param_error(param);
        if (!error.empty()) {
            errors.push_back(param.display_name + ": " + error);
        }
    }
    if (errors.empty()) {
        ImGui::TextColored(ImVec4(0.31f, 0.75f, 0.42f, 1.0f), "参数校验通过");
    }
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "参数校验：%d 项错误",
                           static_cast<int>(errors.size()));
        for (const std::string& error : errors) {
            ImGui::BulletText("%s", error.c_str());
        }
    }

    ImGui::Separator();

    // ---- 参数控件 ----
    if (node->params.empty()) {
        ImGui::TextDisabled("该节点没有参数");
    }
    for (Param& param : node->params) {
        ImGui::PushID(param.id.c_str());
        ImGui::TextUnformatted(param_label(param).c_str());
        help_marker(param.description);
        ImGui::PopID();

        bool param_begin_edit = false;
        if (draw_param_widget(*node, param, param_begin_edit)) {
            result.changed = true;
            log::info("参数变更: " + node->id + "." + param.id);
        }
        if (param_begin_edit) {
            result.begin_edit = true;
        }

        const std::string error = param_error(param);
        if (!error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "  %s", error.c_str());
        }
        ImGui::Spacing();
    }

    // ---- 底部操作 ----
    ImGui::Separator();
    if (ImGui::Button("重置为默认值", ImVec2(-FLT_MIN, 0.0f))) {
        for (Param& param : node->params) {
            param.reset_to_default();
        }
        result.changed = true;
        log::info("参数重置为默认值: " + node->id);
    }

    ImGui::End();
}

} // namespace aiwrite::ui
