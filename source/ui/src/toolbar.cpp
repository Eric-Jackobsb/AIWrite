#include "ui/toolbar.h"

#include "engine/provider_resolve.h"
#include "ui/editor_state.h"
#include "utils/log.h"

#include <imgui.h>

#include <string>
#include <vector>

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

    // ---- 运行前预检（P1-b）：未接线分支（official / M5-02）本次运行必定失败，先弹窗确认 ----
    struct BlockedNode {
        std::string node_id;
        std::string node_title;
        std::string reason;
        std::string source_node;  // 空 = 节点自身设置
        std::string source_title; // 来源节点头衔（如「提供商配置」）
    };
    static std::vector<BlockedNode> blocked_nodes; // 弹窗打开期间的待确认清单
    constexpr const char* kUnwiredModal = "运行前提示##run_unwired_confirm";

    // 收集「本次运行必定失败」的节点（官方 API 未接线 / 多模态未接线）
    const auto collect_blocked = [&state]() {
        std::vector<BlockedNode> found;
        for (const engine::Node& node : state.graph.nodes) {
            const std::string reason = engine::unwired_reason(state.graph, node);
            if (reason.empty()) {
                continue;
            }
            const engine::EffectiveProvider effective =
                engine::resolve_effective_provider(state.graph, node);
            BlockedNode item;
            item.node_id    = node.id;
            item.node_title = node.title.empty() ? node.type : node.title;
            item.reason     = reason;
            if (effective.from_edge) {
                item.source_node  = effective.source_node;
                item.source_title = "提供商配置";
            }
            found.push_back(std::move(item));
        }
        return found;
    };

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
        blocked_nodes = collect_blocked();
        if (blocked_nodes.empty()) {
            state.start_run_async(); // PB-01：后台线程推进；之后每帧由主循环 tick_run 泵事件
        }
        else {
            // P1-b：存在「未接线分支」→ 先弹窗确认（不直接运行）
            log::warn("[运行前提示] 有 " + std::to_string(blocked_nodes.size()) +
                      " 个节点的分支尚未接线，等待用户确认");
            ImGui::OpenPopup(kUnwiredModal);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("运行工作流（设计 §6.5）\n"
                          "· 先做运行前校验（无环 / 必填输入已连 / 参数合法），失败会被拒绝并写入 Console\n"
                          "· 运行前预检：若存在未接线分支（官方 API / 多模态）会先弹窗确认\n"
                          "· 每帧推进一个节点；节点状态实时显示在画布上"
                          "（蓝=运行中 / 绿=完成 / 红=失败 / 暗=跳过）");
    }

    // ---- 运行前预检弹窗（P1-b）----
    // 为什么不把 official 升级为运行前校验的 error：--run-selftest 与 api_probe 断言都要求
    // 运行前校验「不阻断」（见 api_probe「未配置 Key：运行前校验仍通过」）；拦截只发生在工具栏。
    if (ImGui::BeginPopupModal(kUnwiredModal, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f),
                           "运行前提示：%d 个节点的分支尚未接线，本次运行必定失败",
                           static_cast<int>(blocked_nodes.size()));
        ImGui::Separator();
        for (const BlockedNode& item : blocked_nodes) {
            if (item.source_node.empty()) {
                ImGui::BulletText("%s %s ← 节点自身设置（%s）", item.node_id.c_str(),
                                  item.node_title.c_str(), item.reason.c_str());
            }
            else {
                ImGui::BulletText("%s %s ← %s %s（%s）", item.node_id.c_str(),
                                  item.node_title.c_str(), item.source_title.c_str(),
                                  item.source_node.c_str(), item.reason.c_str());
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("官方 API（API Key）已接线：填好「提供商配置 → API Key」即可；网页版需先登录一次。\n"
                            "「切换为网页版并运行」会把这些来源节点的「模式」改为 web（一次快照，可撤销）。\n"
                            "多模态分支（M5-02）无法通过切换模式修复，仍会失败。");
        ImGui::Separator();

        if (ImGui::Button("切换为网页版并运行", ImVec2(180.0f, 0.0f))) {
            const auto switchable = [](const EditorState& state,
                                       const std::vector<BlockedNode>& items) {
                for (const BlockedNode& item : items) {
                    const std::string target_id =
                        item.source_node.empty() ? item.node_id : item.source_node;
                    if (const engine::Node* target = state.graph.findNode(target_id)) {
                        if (target->findParam("mode") != nullptr) {
                            return true;
                        }
                    }
                }
                return false;
            };
            if (switchable(state, blocked_nodes)) {
                state.snapshot("切换提供商为网页版并运行"); // 先压快照：整批切换可一次撤销
            }
            int switched = 0;
            for (const BlockedNode& item : blocked_nodes) {
                const std::string target_id =
                    item.source_node.empty() ? item.node_id : item.source_node;
                if (engine::Node* target = state.graph.findNode(target_id)) {
                    if (engine::Param* mode = target->findParam("mode")) {
                        mode->value = std::string("web");
                        ++switched;
                    }
                }
            }
            log::info("[运行前提示] 已把 " + std::to_string(switched) + " 个节点的模式切换为 web，开始运行");
            blocked_nodes.clear();
            ImGui::CloseCurrentPopup();
            state.start_run_async(); // PB-01：异步
        }
        ImGui::SameLine();
        if (ImGui::Button("仍要运行", ImVec2(100.0f, 0.0f))) {
            log::warn("[运行前提示] 用户选择仍要运行（预期失败）");
            blocked_nodes.clear();
            ImGui::CloseCurrentPopup();
            state.start_run_async(); // PB-01：异步
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(80.0f, 0.0f))) {
            blocked_nodes.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
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
