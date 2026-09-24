#include "ui/output_panel.h"

#include "engine/executor.h"
#include "engine/graph.h"
#include "ui/editor_state.h"
#include "utils/file_dialog.h"
#include "utils/log.h"

#include <cstddef>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace aiwrite::ui {
namespace {

// 面板内显示上限（只截断**显示**；复制与导出始终为完整内容）
constexpr std::size_t kDisplayLimit = 40000;

const ImVec4 kColorDone(0.31f, 0.75f, 0.42f, 1.0f);
const ImVec4 kColorError(1.00f, 0.35f, 0.35f, 1.0f);
const ImVec4 kColorSkip(0.85f, 0.70f, 0.30f, 1.0f);

std::string value_text(const nlohmann::json& value)
{
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_null()) {
        return {};
    }
    return value.dump();
}

ImVec4 state_color(engine::NodeState state)
{
    switch (state) {
    case engine::NodeState::Done:    return kColorDone;
    case engine::NodeState::Error:   return kColorError;
    case engine::NodeState::Skipped: return kColorSkip;
    default:                         return ImGui::GetStyle().Colors[ImGuiCol_Text];
    }
}

std::string node_header(const engine::NodeRunInfo& info)
{
    return "[" + info.node_id + "] " + info.type + " · " + engine::nodeStateName(info.state) +
           " · " + std::to_string(static_cast<long long>(info.duration_ms + 0.5)) + " ms";
}

// 只读多行文本（可选中复制）。缓冲按「节点 id + 内容」缓存，仅在内容变化时重建。
void show_readonly_text(const std::string& node_id, const std::string& text)
{
    struct Cache {
        std::string       text;
        std::vector<char> buffer;
    };
    static std::unordered_map<std::string, Cache> cache;

    const bool        truncated = text.size() > kDisplayLimit;
    const std::string shown     = truncated ? text.substr(0, kDisplayLimit) : text;

    Cache& entry = cache[node_id];
    if (entry.text != shown) {
        entry.text = shown;
        entry.buffer.assign(shown.begin(), shown.end());
        entry.buffer.push_back('\0');
    }

    const float height = ImGui::GetTextLineHeight() * 10.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::InputTextMultiline("##result", entry.buffer.data(), entry.buffer.size(),
                              ImVec2(-FLT_MIN, height), ImGuiInputTextFlags_ReadOnly);
    if (truncated) {
        ImGui::TextDisabled("（显示已截断：%zu / %zu 字符；复制与导出为完整内容）", shown.size(),
                            text.size());
    }
}

} // namespace

std::string node_output_text(const engine::Graph& graph, const engine::Executor& executor,
                             const std::string& node_id)
{
    const engine::Node* node = graph.findNode(node_id);
    if (node == nullptr) {
        return {};
    }

    std::string text;
    for (const engine::Port& port : node->outputs) {
        const nlohmann::json* value = executor.outputs().find(node_id, port.id);
        if (value == nullptr) {
            continue;
        }
        const std::string item = value_text(*value);
        if (item.empty()) {
            continue;
        }
        if (!text.empty()) {
            text += "\n";
        }
        if (node->outputs.size() > 1) {
            text += port.display_name + "：";
        }
        text += item;
    }
    return text;
}

std::string run_output_text(const engine::Graph& graph, const engine::Executor& executor)
{
    std::string text;
    for (const engine::NodeRunInfo& info : executor.runInfos()) {
        const std::string body = node_output_text(graph, executor, info.node_id);
        if (body.empty() && info.error.empty()) {
            continue; // 无输出也无错误（如被跳过的中间节点）→ 不占版面
        }
        if (!text.empty()) {
            text += "\n\n";
        }
        text += node_header(info);
        if (!info.error.empty()) {
            text += "\n错误：" + info.error;
        }
        if (!body.empty()) {
            text += "\n" + body;
        }
    }
    return text;
}

void draw_output_panel(const char* title, bool* open, const EditorState& state)
{
    if (!ImGui::Begin(title, open)) {
        ImGui::End();
        return;
    }

    const engine::Executor& executor = state.executor;
    const auto&             infos    = executor.runInfos();

    // ---- 顶部工具条 ----
    const std::string all_text = run_output_text(state.graph, executor);
    const bool        has_text = !all_text.empty();

    ImGui::BeginDisabled(!has_text);
    if (ImGui::Button("复制全文")) {
        ImGui::SetClipboardText(all_text.c_str());
        log::info("[输出面板] 已复制全文（" + std::to_string(all_text.size()) + " 字符）");
    }
    ImGui::SameLine();
    if (ImGui::Button("导出到文件…")) {
        const std::string path =
            utils::save_file({{"文本", "txt"}, {"全部文件", "*"}}, {}, "aiwrite-output.txt");
        if (!path.empty()) {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << all_text;
            if (stream.good()) {
                log::info("[输出面板] 已导出运行结果：" + path + "（" +
                          std::to_string(all_text.size()) + " 字符）");
            }
            else {
                log::error("[输出面板] 导出失败：" + path);
            }
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("｜ 节点 %zu ｜ 总耗时 %.2f s", infos.size(), executor.elapsedSeconds());
    ImGui::Separator();

    if (infos.empty()) {
        ImGui::TextDisabled("尚无运行结果：点工具栏「▶ 运行」");
        ImGui::End();
        return;
    }

    // ---- 逐节点分段 ----
    for (const engine::NodeRunInfo& info : infos) {
        const std::string body   = node_output_text(state.graph, executor, info.node_id);
        const std::string header = node_header(info);
        const bool        leaf   = body.empty() && info.error.empty();

        ImGui::PushID(info.node_id.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, state_color(info.state));
        const bool open_section = ImGui::CollapsingHeader(
            header.c_str(), leaf ? ImGuiTreeNodeFlags_Leaf : ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor();

        if (open_section) {
            if (!info.error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kColorError);
                ImGui::TextWrapped("错误：%s", info.error.c_str());
                ImGui::PopStyleColor();
            }
            if (!body.empty()) {
                show_readonly_text(info.node_id, body);
                if (ImGui::Button("复制该节点")) {
                    ImGui::SetClipboardText(body.c_str());
                    log::info("[输出面板] 已复制 " + info.node_id + " 的结果（" +
                              std::to_string(body.size()) + " 字符）");
                }
            }
            else if (info.error.empty()) {
                ImGui::TextDisabled("（该节点无输出）");
            }
        }
        ImGui::PopID();
    }

    ImGui::End();
}

} // namespace aiwrite::ui
