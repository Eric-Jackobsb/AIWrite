#include "error_bar.h"

#include "connection_test.h" // M_patchAB（PB-07）：错误条「重新登录」动作
#include "log.h"

#include <imgui.h>

#include <string>

namespace aiwrite::ui {

void drawErrorBar(EditorState& state)
{
    if (!state.last_error.active) {
        return;
    }

    // DEBT-L8（M6-04 步骤 5）：失败汇总 —— 多失败显示汇总文案，单失败保持「id：消息」
    const std::string full = state.last_error.failed_count > 1
                                 ? state.last_error.summary
                                 : state.last_error.node_id + "：" + state.last_error.message;
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
        // 多失败时 node_id = 首个失败节点（定位首个；全部明细在悬停提示）
        if (state.model().graph.findNode(state.last_error.node_id) != nullptr) {
            state.selected_node  = state.last_error.node_id;
            state.selected_nodes = {state.last_error.node_id};
            // P7a-17：**居中并放大**到该节点（NavigateToSelection），而不是仅跟随全部内容
            state.request_focus_selection = true;
            state.setStatus(state.last_error.failed_count > 1
                                ? "已定位首个失败节点 " + state.last_error.node_id
                                : "已定位失败节点 " + state.last_error.node_id);
            log::info("[错误条] 定位失败节点 " + state.last_error.node_id);
        }
        else {
            state.setStatus("失败节点已不存在：" + state.last_error.node_id);
        }
    }
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered()) {
        // 多失败 → tooltip 列全部失败节点（每节点一行）；单失败 → 复用单行全文
        const std::string tip =
            state.last_error.details.empty()
                ? full + "\n（" + state.last_error.at + "）\n点击定位失败节点；点右侧 × 关闭本次提示"
                : state.last_error.details + "\n（" + state.last_error.at +
                      "）\n点击定位首个失败节点；点右侧 × 关闭本次提示";
        ImGui::SetTooltip("%s", tip.c_str());
    }

    // ---- M_polish 阶段二 `POL-UX-01`（M6-04 步骤 4 · 重试按钮）：一键重跑首个失败节点及下游 ----
    //  * 复用 PD-06① `startRerunAsync(Downstream)`：失败节点阻塞下游 ⇒ 重跑「节点及下游」走通后续链路；
    //    核心重跑语义已被 PD-06① 离线断言覆盖（本按钮是纯 UI 薄接线）
    //  * 守卫：节点仍在图中 + 当前无运行会话；成功启动后清掉错误条（重跑结束由 refresh_last_error 重新判定）
    const engine::Node* failed_node = state.model().graph.findNode(state.last_error.node_id);
    const bool          retry_ok    = failed_node != nullptr && !state.session_active();
    ImGui::SameLine();
    ImGui::BeginDisabled(!retry_ok);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.42f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.54f, 0.32f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.48f, 0.28f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 1.0f, 0.92f, 1.0f));
    if (ImGui::Button("重试##error_bar_retry")) {
        if (state.startRerunAsync(state.last_error.node_id, engine::RerunScope::Downstream)) {
            state.clearLastError(); // 重跑已接管：错误条消失，状态栏显示「重跑中…」
        }
        // 重跑被拒绝（运行前校验不过）→ 保留错误条，状态栏已有拒绝原因
    }
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          retry_ok
                              ? (state.last_error.failed_count > 1
                                     ? "重跑首个失败节点及其下游（复用上游结果）；多失败时下游会连带重跑"
                                     : "重跑该失败节点及其下游（复用上游结果）")
                              : (failed_node == nullptr
                                     ? "失败节点已不存在（可能已被删除），无法重试"
                                     : "正在运行中：请先等待本次运行结束"));
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("×##error_bar_close")) {
        state.clearLastError();
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
            openWebLoginWindow(state.last_error.relogin_provider_id);
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
