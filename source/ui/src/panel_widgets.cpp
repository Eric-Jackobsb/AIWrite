
#include "property_panel.h"

#include "panel_internal.h"

#include "asset_store.h"
#include "file_dialog.h"
#include "log.h"
#include "paths.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// ============================================================================
//  参数面板 · 参数控件域（CLN-12 拆分 · 2026-10-20）
//  自 property_panel.cpp 原样搬移（行为等价，仅分文件）
// ============================================================================


namespace aiwrite::ui {

using engine::Node;
using engine::Param;
using engine::ParamType;

namespace {
// ImGui 的 Slider* 只接受 half-range 内的数值（imgui_widgets.cpp SliderBehavior 的
//  IM_ASSERT(p_max <= T_MAX / 2)）——参数定义一旦越界，控件一渲染就 abort。
//  这里给出安全上限；越界范围自动降级为 Input* + 手动裁剪（见 draw_param_widget）。
constexpr double kSliderLimitInt   = 1073741823.0;  // IM_S32_MAX / 2
constexpr double kSliderLimitFloat = 1.7e38;         // ≈ FLT_MAX / 2
std::vector<utils::FileFilter> filters_for(const Node& node)
{
    if (node.type == "ImageInput") {
        return {{"图片文件", "png,jpg,jpeg,bmp,webp"}, {"所有文件", "*"}};
    }
    return {{"所有文件", "*"}};
}
// 立即写回参数值（**不依赖"失焦判定"**）；返回是否发生变化
//  * 背景：原先统一写 `Widget(...) && IsItemDeactivatedAfterEdit()`，而"值发生变化"与"控件失焦"
//    往往不在同一帧（多行文本尤其明显）→ 会出现「改了文本内容，运行结果还是旧值」。
//    现改为：值变化即写回模型；`edited` 只在编辑结束时通知外层（日志/状态栏不刷屏）。
bool write_param(Param& param, const nlohmann::json& value)
{
    if (param.value == value) {
        return false;
    }
    param.value = value;
    return true;
}
// ============================================================================
//  M_8nodes_extension L1-a（§3.2 / §3.4 / §5.1）：二级分组 + 网页版忽略标注 + StringList 编辑
// ============================================================================

// StringList 值（JSON 字符串数组）→ 逗号分隔文本（编辑形态）
std::string string_list_to_text(const Param& param)
{
    if (!param.value.is_array()) {
        return {};
    }
    std::string joined;
    for (const auto& item : param.value) {
        if (!item.is_string()) {
            continue;
        }
        const std::string entry = item.get<std::string>();
        if (entry.empty()) {
            continue;
        }
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += entry;
    }
    return joined;
}
// 逗号分隔文本 → StringList 值（trim 每项、去空、保序）
nlohmann::json string_list_from_text(const std::string& text)
{
    nlohmann::json array = nlohmann::json::array();
    std::string    current;
    const auto     flush = [&array, &current]() {
        // 去掉首尾空白（含 \r，兼容 CRLF 粘贴）
        const auto begin = current.find_first_not_of(" \t\r\n");
        const auto end   = current.find_last_not_of(" \t\r\n");
        if (begin != std::string::npos && end != std::string::npos) {
            array.push_back(current.substr(begin, end - begin + 1));
        }
        current.clear();
    };
    for (const char ch : text) {
        if (ch == ',') {
            flush();
        }
        else {
            current += ch;
        }
    }
    flush();
    return array;
}
// M_8nodes_extension L1-c（§4.1）：N-01 文件导入 —— UTF-8 有效性校验（简单序列检查）
//  * 非法 UTF-8 序列（GBK / ANSI 编码的文件）**不静默乱码**：返回 false 给可操作提示
bool valid_utf8(const std::string& text)
{
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        int extra = 0;
        if ((lead & 0xE0) == 0xC0) {
            extra = 1;
        }
        else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
        }
        else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
        }
        else {
            return false; // 非法的首字节
        }
        if (i + static_cast<std::size_t>(extra) >= text.size()) {
            return false; // 截断的多字节序列
        }
        for (int k = 1; k <= extra; ++k) {
            const unsigned char cont = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
            if ((cont & 0xC0) != 0x80) {
                return false; // 续字节非法
            }
        }
        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}
bool import_text_file(const std::string& path, std::string* content, std::string* error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        *error = "无法读取文件: " + path;
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();
    if (!valid_utf8(text)) {
        *error = "文件不是 UTF-8 编码（可能是 GBK / ANSI）：请先转换为 UTF-8 再导入（避免乱码）";
        return false;
    }
    *content = std::move(text);
    return true;
}
} // namespace
std::string paramLabel(const Param& param)
{
    return param.display_name + (param.is_required ? " *" : "");
}
// 绘制单个参数控件（PS-09-G1 方案乙：公共入口，主属性面板 / 上级节点编辑器共用）
//  返回 edited = 本次编辑结束（失焦/回车）；begin_edit = 控件刚被激活（值尚未变化，
//  外层在此刻压撤销快照）；changed_now = 值已写回模型（编辑过程中每帧都可能为 true）
bool draw_param_widget(Node& node, Param& param, bool& begin_edit, bool& changed_now,
                       const std::vector<std::string>* enum_override)
{
    bool edited             = false;
    const std::string label = paramLabel(param);
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
        const bool touched =
            ImGui::InputTextWithHint("##value", param.display_name.c_str(), &value, flags);
        if (touched) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    case ParamType::Text: {
        std::string value = param.text();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool touched =
            ImGui::InputTextMultiline("##value", &value,
                                      ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5.0f));
        if (touched) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();

        // ---- M_8nodes_extension L1-c（§4.1）：N-01 文本输入「从文件导入…」----
        //  * 已有内容 → 二次确认（避免误操作丢稿）；空内容 → 直接导入
        //  * UTF-8 读取；非 UTF-8（GBK/ANSI）→ 可操作提示，不静默乱码
        if (node.type == "TextInput" && param.id == "text") {
            // 确认状态机：只对当前节点 / 当前参数生效
            static std::string confirm_node;
            static std::string confirm_param;
            static std::string pending_path;
            const bool         confirming = (confirm_node == node.id && confirm_param == param.id &&
                                             !pending_path.empty());
            ImGui::Spacing();
            if (ImGui::Button(confirming ? "确认覆盖已有内容" : "从文件导入…", ImVec2(140.0f, 0.0f))) {
                if (confirming) {
                    // 第二次点击 = 确认覆盖
                    confirm_node.clear();
                    confirm_param.clear();
                    const std::string path = pending_path;
                    pending_path.clear();
                    std::string content;
                    std::string error;
                    if (import_text_file(path, &content, &error)) {
                        changed_now = write_param(param, content);
                        log::info("[文本输入] 从文件导入（覆盖）：" + path + "（" +
                                  std::to_string(content.size()) + " 字节）");
                    }
                    else {
                        log::error("[文本输入] 文件导入失败：" + error);
                        ImGui::OpenPopup("##import_error");
                    }
                }
                else {
                    const std::string picked =
                        utils::open_file({{"文本文件", "txt,md"}, {"所有文件", "*"}}, param.text());
                    if (!picked.empty()) {
                        if (param.text().empty()) {
                            // 空内容 → 直接导入
                            std::string content;
                            std::string error;
                            if (import_text_file(picked, &content, &error)) {
                                changed_now = write_param(param, content);
                                log::info("[文本输入] 从文件导入：" + picked + "（" +
                                          std::to_string(content.size()) + " 字节）");
                            }
                            else {
                                log::error("[文本输入] 文件导入失败：" + error);
                                ImGui::OpenPopup("##import_error");
                            }
                        }
                        else {
                            // 已有内容 → 进入确认状态（显示确认按钮）
                            confirm_node    = node.id;
                            confirm_param   = param.id;
                            pending_path    = picked;
                        }
                    }
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(confirming
                                      ? "再次点击将**覆盖**当前「文本」内容（不可恢复）；点其它处取消"
                                      : "读取 .txt / .md 文件内容到「文本」（UTF-8；已有内容会先询问）");
            }
            if (ImGui::BeginPopup("##import_error")) {
                ImGui::TextWrapped("文件导入失败（详见日志）");
                ImGui::EndPopup();
            }
        }
        break;
    }
    case ParamType::Int: {
        const double fallback =
            param.default_value.is_number() ? param.default_value.get<double>() : 0.0;
        int value = static_cast<int>(std::lround(param.number(fallback)));
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool has_range = param.min_value.has_value() && param.max_value.has_value();
        // 修复（崩溃）：SliderInt 只支持 ±IM_S32_MAX/2；范围越界（如 0..2147483647）时
        //  改用 InputInt，并把输入裁回参数声明的范围，避免 ImGui 断言 abort。
        const bool in_slider_range =
            has_range && (*param.min_value >= -kSliderLimitInt) && (*param.max_value <= kSliderLimitInt);
        const bool touched =
            in_slider_range ? ImGui::SliderInt("##value", &value, static_cast<int>(*param.min_value),
                                              static_cast<int>(*param.max_value))
                            : ImGui::InputInt("##value", &value);
        if (touched) {
            if (has_range) {
                const double clamped = std::clamp(static_cast<double>(value), *param.min_value, *param.max_value);
                value                = static_cast<int>(std::lround(clamped));
            }
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
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
        // 与 Int 同理：SliderFloat 只支持 ±FLT_MAX/2；越界时降级为 InputFloat + 裁剪。
        const bool in_slider_range = has_range && std::isfinite(*param.min_value) &&
                                     std::isfinite(*param.max_value) &&
                                     (*param.min_value >= -kSliderLimitFloat) &&
                                     (*param.max_value <= kSliderLimitFloat);
        const bool touched =
            in_slider_range ? ImGui::SliderFloat("##value", &value, static_cast<float>(*param.min_value),
                                                static_cast<float>(*param.max_value), "%.2f")
                            : ImGui::InputFloat("##value", &value, 0.1f, 1.0f, "%.2f");
        if (touched) {
            if (has_range) {
                value = static_cast<float>(
                    std::clamp(static_cast<double>(value), *param.min_value, *param.max_value));
            }
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    case ParamType::Bool: {
        bool value = param.flag();
        if (ImGui::Checkbox("##value", &value)) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    case ParamType::Enum: {
        const std::string current = param.text();
        // ---- v16（仅界面）：提供商下拉「合并显示」（`-web` 项合并 / 去后缀；见上方说明）----
        if (param.id == "provider") {
            const std::vector<ProviderChoice> choices    = buildProviderChoices();
            const engine::Param*              mode_param = node.findParam("mode");
            const std::string mode = mode_param != nullptr ? mode_param->text() : std::string();
            std::vector<std::string> labels;
            std::vector<std::string> values; // 与 labels 对齐：选中后写入 `provider` 的真实 id
            labels.reserve(choices.size() + 1);
            values.reserve(choices.size() + 1);
            int index = -1;
            for (const ProviderChoice& choice : choices) {
                labels.push_back(choice.label);
                values.push_back((mode == "web" && !choice.web_id.empty()) ? choice.web_id
                                                                          : choice.id);
                if (choice.id == current ||
                    (!choice.web_id.empty() && choice.web_id == current)) {
                    index = static_cast<int>(labels.size()) - 1;
                }
            }
            if (index < 0) {
                // 表外值（手改 JSON / 旧工作流里的其它 id）：**如实显示**，不静默替换
                labels.insert(labels.begin(), current);
                values.insert(values.begin(), current);
                index = 0;
            }
            std::vector<const char*> items;
            items.reserve(labels.size());
            for (const std::string& choice_label : labels) {
                items.push_back(choice_label.c_str());
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (!items.empty() &&
                ImGui::Combo("##value", &index, items.data(), static_cast<int>(items.size()))) {
                const int safe_index = std::clamp(index, 0, static_cast<int>(values.size()) - 1);
                const std::string wanted = values[static_cast<std::size_t>(safe_index)];
                if (wanted != current) {
                    write_param(param, wanted); // 写入的始终是表内真实 id（运行期照常解析）
                }
                edited      = true;
                changed_now = true;
            }
            note_activation();
            break;
        }
        // 允许调用方按上下文覆盖可选项（扩展位）。
        //  * ⚠️ M_patchB L2（PB2-20 / 决策 D-21）：**不得**用它裁剪 ProviderConfig 的「模式」
        //    （恒 {official, web}，不变量 I13）；目前无调用方传入
        //  * 若当前值不在可选项里，**如实显示当前值**（调用方给出提示）
        std::vector<std::string> options =
            enum_override != nullptr ? *enum_override : param.enum_options;
        if (!current.empty() &&
            std::find(options.begin(), options.end(), current) == options.end()) {
            options.insert(options.begin(), current);
        }
        int index = 0;
        for (std::size_t i = 0; i < options.size(); ++i) {
            if (options[i] == current) {
                index = static_cast<int>(i);
                break;
            }
        }
        std::vector<const char*> items;
        items.reserve(options.size());
        for (const std::string& option : options) {
            items.push_back(option.c_str());
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        // 注意：Combo 在"点击下拉项"的当帧就返回 true，而该帧的 IsItemDeactivatedAfterEdit()
        // 并不成立（被点击的是弹出层里的选项，属于另一个 item），
        // 因此这里必须"值变化即写回"，否则会出现「选了 Web 但模式没变」的现象。
        if (!items.empty() &&
            ImGui::Combo("##value", &index, items.data(), static_cast<int>(items.size()))) {
            const int safe_index = std::clamp(index, 0, static_cast<int>(options.size()) - 1);
            param.value          = options[static_cast<std::size_t>(safe_index)];
            edited               = true;
            changed_now          = true;
        }
        // ---- v16（仅界面）：切换「模式」→ 让 `provider` 配对到该条目的网页版 / 官方版 ----
        //  * 使「不带 `-web` 的条目」（如 `deepseek` / `gemini`）在 web 模式下直接可用
        //  * 写入的仍是表内真实 id → 运行期与自检无需任何改动；可撤销
        if (param.id == "mode" && changed_now) {
            syncProviderIdWithMode(node);
        }
        note_activation();
        break;
    }
    case ParamType::File: {
        // P7a-02：File 参数支持**多值**（每行一个路径；换行为分隔符，见 utils/paths.h）
        //  * 2 行输入框 + 右侧按钮组（浏览…=替换 / 多选…=追加去重 / 清空）
        //  * 多行控件的 SameLine 落点不可靠 → 用 SetCursorScreenPos 定位按钮组
        std::string value = param.text();

        const ImVec2 start    = ImGui::GetCursorScreenPos();
        const float  spacing  = ImGui::GetStyle().ItemSpacing.x;
        const float  button_w = 84.0f;
        const float  field_w  = ImGui::GetContentRegionAvail().x - button_w - spacing;
        const float  field_h  = ImGui::GetTextLineHeight() * 2.0f +
                               ImGui::GetStyle().FramePadding.y * 2.0f;

        const bool touched =
            ImGui::InputTextMultiline("##value", &value, ImVec2(field_w, field_h));
        if (touched) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();

        const ImVec2 after_field = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(start.x + field_w + spacing, start.y));
        ImGui::BeginGroup();
        if (ImGui::Button("浏览…", ImVec2(button_w, 0.0f))) {
            const std::string picked = utils::open_file(filters_for(node), param.text());
            if (!picked.empty()) {
                write_param(param, picked);
                edited      = true;
                changed_now = true;
            }
        }
        if (ImGui::Button("多选…", ImVec2(button_w, 0.0f))) {
            const std::vector<std::string> picked =
                utils::open_files(filters_for(node), param.text());
            if (!picked.empty()) {
                std::vector<std::string> merged = paths::splitPathList(param.text());
                for (const std::string& item : picked) {
                    if (std::find(merged.begin(), merged.end(), item) == merged.end()) {
                        merged.push_back(item);
                    }
                }
                write_param(param, paths::joinPathList(merged));
                edited      = true;
                changed_now = true;
            }
        }
        if (ImGui::Button("清空", ImVec2(button_w, 0.0f))) {
            write_param(param, std::string());
            edited      = true;
            changed_now = true;
        }
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(after_field);

        const std::vector<std::string> entries = paths::splitPathList(param.text());
        const int count = static_cast<int>(entries.size());
        if (count > 1) {
            ImGui::TextDisabled("共 %d 张图片（每行一个路径）", count);
        }

        // P7a-04/07：令牌 → 资源目录里的真实路径（可见完整路径；缺失时红字告警）
        bool has_legacy = false;
        for (const std::string& entry : entries) {
            if (asset::needsMigration(entry)) {
                has_legacy = true;
                continue;
            }
            bool              missing = false;
            std::string       resolve_error;
            const std::string local = asset::toLocalPath(entry, &missing, &resolve_error);
            if (missing || local.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s",
                                   resolve_error.empty() ? "图片资源缺失（可能已被删除）"
                                                         : resolve_error.c_str());
                ImGui::PopStyleColor();
            }
            else {
                ImGui::TextDisabled("→ %s", local.c_str());
            }
        }

        // P7a-06：外部路径 → 一键迁进资源目录（失败项**保持原值**，不丢用户数据）
        if (has_legacy) {
            ImGui::TextDisabled("其中含**外部路径**：迁移进资源目录后，换目录 / 换机仍能取到图片");
            if (ImGui::Button("迁移到资源目录", ImVec2(150.0f, 0.0f))) {
                std::vector<std::string> migrated;
                std::vector<std::string> failures;
                for (const std::string& entry : entries) {
                    if (!asset::needsMigration(entry)) {
                        migrated.push_back(entry);
                        continue;
                    }
                    std::string       import_error;
                    const std::string token = asset::importFile(entry, &import_error);
                    if (token.empty()) {
                        if (!import_error.empty()) {
                            failures.push_back(import_error);
                        }
                        migrated.push_back(entry);
                        continue;
                    }
                    migrated.push_back(token);
                }
                write_param(param, paths::joinPathList(migrated));
                edited      = true;
                changed_now = true;
                log::info("[资源目录] 迁移完成：" + std::to_string(entries.size()) + " 项，失败 " +
                          std::to_string(failures.size()) + " 项");
                if (!failures.empty()) {
                    log::error("[资源目录] 迁移失败：" + failures.front());
                }
            }
        }
        break;
    }
    case ParamType::Directory: {
        std::string value = param.text();
        ImGui::SetNextItemWidth(-FLT_MIN - 90.0f);
        const bool touched = ImGui::InputTextWithHint("##value", "(未选择)", &value);
        if (touched) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        ImGui::SameLine();
        if (ImGui::Button("选择…", ImVec2(80.0f, 0.0f))) {
            const std::string picked = utils::pickFolder(param.text());
            if (!picked.empty()) {
                write_param(param, picked);
                edited      = true;
                changed_now = true;
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
        if (ImGui::ColorEdit3("##value", rgb)) {
            char buffer[16] = {};
            std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X",
                          static_cast<int>(rgb[0] * 255.0f), static_cast<int>(rgb[1] * 255.0f),
                          static_cast<int>(rgb[2] * 255.0f));
            changed_now = write_param(param, std::string(buffer));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    case ParamType::StringList: {
        // M_8nodes_extension L1-a（§5.1）：`stop` 序列 —— 逗号分隔编辑 ↔ JSON 字符串数组
        std::string text = string_list_to_text(param);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool touched = ImGui::InputText("##value", &text);
        if (touched) {
            changed_now = write_param(param, string_list_from_text(text));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    }

    ImGui::PopID();
    return edited;
}
} // namespace aiwrite::ui

