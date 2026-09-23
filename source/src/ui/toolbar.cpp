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
}

} // namespace aiwrite::ui
