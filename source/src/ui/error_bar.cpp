#include "ui/error_bar.h"

#include "utils/log.h"

#include <imgui.h>

#include <string>

namespace aiwrite::ui {

void draw_error_bar(EditorState& state)
{
    if (!state.last_error.active) {
        return;
    }

    const std::string full = state.last_error.node_id + "：" + state.last_error.message;
    std::string       line = full;
    if (line.size() > 96) {
        line.resize(96);
        line += "…"; // 单行省略（全文在悬停提示里）
    }

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.60f, 0.20f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.52f, 0.17f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.85f, 1.0f));
    if (ImGui::Button(("执行失败  " + line + "##error_bar").c_str())) {
        // 点击 → 选中失败节点（复用视图跟随；连续错误只覆盖显示、不堆积）
        if (state.graph.findNode(state.last_error.node_id) != nullptr) {
            state.selected_node  = state.last_error.node_id;
            state.selected_nodes = {state.last_error.node_id};
            state.request_navigate_to_content = true;
            state.set_status("已定位失败节点 " + state.last_error.node_id);
            log::info("[错误条] 定位失败节点 " + state.last_error.node_id);
        }
        else {
            state.set_status("失败节点已不存在：" + state.last_error.node_id);
        }
    }
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s\n（%s）\n点击定位失败节点；点右侧 × 关闭本次提示", full.c_str(),
                          state.last_error.at.c_str());
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("×##error_bar_close")) {
        state.clear_last_error();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("关闭错误条（下次运行失败时会再次出现）");
    }
}

} // namespace aiwrite::ui
