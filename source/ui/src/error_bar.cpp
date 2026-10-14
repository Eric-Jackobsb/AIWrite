#include "error_bar.h"

#include "connection_test.h" // M_patchAB（PB-07）：错误条「重新登录」动作
#include "log.h"

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
            // P7a-17：**居中并放大**到该节点（NavigateToSelection），而不是仅跟随全部内容
            state.request_focus_selection = true;
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

    // ---- M_patchAB §2.2（`PB-07`）：会话失效 → 一键「重新登录」 ----
    //  * 判据来自 `EditorState::refresh_last_error()` 的错误串反查（纯函数；只有命中才显示本按钮）
    //  * 按钮打开**该失败节点生效条目**的登录窗口（严格解析、不回落内置默认站点）
    if (state.last_error.relogin_available) {
        ImGui::SameLine();
        const std::string label =
            state.last_error.relogin_needed ? std::string("会话已失效，点此重新登录")
                                            : std::string("站点拒绝请求，点此处理");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.42f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.74f, 0.52f, 0.14f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.68f, 0.47f, 0.12f, 1.0f));
        if (ImGui::Button((label + "##error_bar_relogin").c_str())) {
            log::info("[错误条] 重新登录（条目 " +
                      (state.last_error.relogin_provider_id.empty()
                           ? std::string("(未指定)")
                           : state.last_error.relogin_provider_id) +
                      "）：" + state.last_error.relogin_text);
            open_web_login_window(state.last_error.relogin_provider_id);
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              state.last_error.relogin_text.empty()
                                  ? "打开该站点的登录窗口（重新登录）"
                                  : state.last_error.relogin_text.c_str());
        }
    }
}

} // namespace aiwrite::ui
