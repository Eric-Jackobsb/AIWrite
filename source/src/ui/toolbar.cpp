#include "ui/toolbar.h"

#include "ui/editor_state.h"

#include <imgui.h>

#include <string>

namespace aiwrite::ui {

void draw_toolbar_buttons()
{
    EditorState& state = editor();

    const bool can_undo = state.undo.canUndo();
    const bool can_redo = state.undo.canRedo();

    // ---- 撤销 / 重做（设计 §6.6 工具栏按钮；无快捷键）----
    ImGui::BeginDisabled(!can_undo);
    if (ImGui::Button("↶ 撤销")) {
        state.undo_once();
    }
    ImGui::EndDisabled();
    if (can_undo && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("撤销：%s（%d 步可用）", state.undo.lastUndoLabel().c_str(),
                          static_cast<int>(state.undo.undoDepth()));
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!can_redo);
    if (ImGui::Button("↷ 重做")) {
        state.redo_once();
    }
    ImGui::EndDisabled();
    if (can_redo && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("重做：%s（%d 步可用）", state.undo.lastRedoLabel().c_str(),
                          static_cast<int>(state.undo.redoDepth()));
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");

    // ---- 节点操作 ----
    // 复制/粘贴入口按设计 §6.1 放在「节点右键 → 复制」「画布右键 → 粘贴」与「编辑菜单」（M3-04 已接回）；
    // 工具栏按 M3-10 的按钮清单（新建/示例/撤销/重做/删除选中/打开/保存/运行/停止）不重复放置。
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.has_selection());
    if (ImGui::Button("删除选中")) {
        state.delete_selected();
    }
    ImGui::EndDisabled();
    if (state.has_selection() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("删除选中的节点与连线（Ctrl+单击多选、框选、左键点连线均可选中）");
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");

    // ---- 工作流 ----
    ImGui::SameLine();
    if (ImGui::Button("新建（清空）")) {
        state.clear_workflow();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("清空画布，并清空撤销历史（设计 §6.6：加载/新建时清空栈）");
    }

    ImGui::SameLine();
    if (ImGui::Button("示例工作流")) {
        state.create_sample_workflow();
    }

    ImGui::SameLine();
    if (ImGui::Button("打开工作流")) {
        open_workflow_dialog();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("从磁盘加载工作流 JSON：先做加载校验，通过后清空撤销栈并记入最近列表");
    }

    ImGui::SameLine();
    if (ImGui::Button("保存工作流")) {
        save_workflow_dialog();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("另存为工作流 JSON（API Key 等 is_secret 参数不落盘，设计 §8.4）");
    }

    ImGui::SameLine();
    ImGui::TextDisabled("| 节点 %d  连线 %d  |  选中 节点 %d / 连线 %d  |  撤销栈 %d",
                        static_cast<int>(state.graph.nodes.size()),
                        static_cast<int>(state.graph.edges.size()),
                        static_cast<int>(state.selected_nodes.size()),
                        static_cast<int>(state.selected_links.size()),
                        static_cast<int>(state.undo.undoDepth()));

    // ---- 运行 / 停止（右对齐 = 顶栏右上角；设计 §6.5 / §7.1）----
    const bool  running    = state.executor.running();
    const char* run_label  = "▶ 运行";
    const char* stop_label = "■ 停止";
    const float spacing    = ImGui::GetStyle().ItemSpacing.x;
    const float run_width  = ImGui::CalcTextSize(run_label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float stop_width = ImGui::CalcTextSize(stop_label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float right_edge = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    const float next_left  = ImGui::GetCursorPosX() + spacing;
    const float wanted     = right_edge - run_width - spacing - stop_width;

    ImGui::SameLine();
    ImGui::SetCursorPosX(next_left > wanted ? next_left : wanted);

    ImGui::BeginDisabled(running || state.graph.nodes.empty());
    if (ImGui::Button(run_label)) {
        state.start_run(); // 运行前校验 → 执行器 start；之后每帧由主循环 tick_run 推进
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("运行工作流（设计 §6.5）\n"
                          "· 先做运行前校验（无环 / 必填输入已连 / 参数合法），失败会被拒绝并写入 Console\n"
                          "· 每帧推进一个节点；节点状态实时显示在画布上"
                          "（蓝=运行中 / 绿=完成 / 红=失败 / 暗=跳过）");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button(stop_label)) {
        state.cancel_run();
    }
    ImGui::EndDisabled();
    if (running && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("停止运行：不再开始新节点（正在执行的节点会跑完当前步骤）");
    }
}

} // namespace aiwrite::ui
