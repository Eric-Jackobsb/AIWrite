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
    // 复制/粘贴已暂停（UI 入口下线，EditorState 逻辑与剪贴板代码保留）：M2/M3 重做画布交互时接回
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
    ImGui::TextDisabled("| 节点 %d  连线 %d  |  选中 节点 %d / 连线 %d  |  撤销栈 %d",
                        static_cast<int>(state.graph.nodes.size()),
                        static_cast<int>(state.graph.edges.size()),
                        static_cast<int>(state.selected_nodes.size()),
                        static_cast<int>(state.selected_links.size()),
                        static_cast<int>(state.undo.undoDepth()));

    // ---- 运行（右对齐 = 顶栏右上角；设计 §6.5 / §7.1）----
    // 【占位】当前只渲染按钮，不接任何逻辑：执行引擎（M2-03 拓扑排序 / M2-04 执行引擎）
    // 落地后把这里改为调用 Executor，并按设计 §6.5 增加「停止」按钮与状态栏进度。
    const char* run_label    = "▶ 运行";
    const float run_width    = ImGui::CalcTextSize(run_label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float right_edge   = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    const float next_left    = ImGui::GetCursorPosX() + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(next_left > right_edge - run_width ? next_left : right_edge - run_width);
    ImGui::BeginDisabled(true); // 占位：无执行引擎
    ImGui::Button(run_label);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("运行工作流（UI 占位，暂未接逻辑）\n"
                          "执行引擎将在 M2-03（拓扑排序）/ M2-04（单线程分帧执行）落地后接线；\n"
                          "届时会连同设计 §6.5 的「停止」按钮与状态栏进度一起启用。");
    }
}

} // namespace aiwrite::ui
