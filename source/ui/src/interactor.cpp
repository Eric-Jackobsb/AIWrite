#include "interactor.h"

#include <algorithm>
#include <cmath>

namespace aiwrite::ui {

namespace ed = ax::NodeEditor;

namespace {

// 连线创建颜色（与两画布原实现一致：连接受理蓝 / 拒绝红）
const ImVec4 kCreateAcceptColor(0.29f, 0.56f, 0.89f, 1.0f);
const ImVec4 kCreateRejectColor(1.0f, 0.30f, 0.30f, 1.0f);
const float  kCreateThickness = 2.0f;

// 夹紧到 ±limit（非有限值 → 0，避免溢出进入绘图与视图计算）
float clampValue(float value, float limit)
{
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, -limit, limit);
}

} // namespace

// ---------------------------------------------------- 选中读取（唯一封装）----
// GetSelectedObjectCount / GetSelectedNodes / GetSelectedLinks 只允许出现在本文件
// （ARCHITECTURE §4 验收判据 3：关键谓词只出现在 interactor.cpp）。
SelectionRead interactorReadSelection()
{
    SelectionRead sel;
    const int count = ed::GetSelectedObjectCount();
    const std::size_t capacity = static_cast<std::size_t>(std::max(count, 0));
    if (count <= 0) {
        return sel;
    }

    std::vector<ed::NodeId> node_handles(capacity);
    const int node_count = ed::GetSelectedNodes(node_handles.data(), count);
    for (int i = 0; i < node_count; ++i) {
        sel.node_ids.push_back("n" +
                               std::to_string(node_handles[static_cast<std::size_t>(i)].Get()));
    }

    std::vector<ed::LinkId> link_handles(capacity);
    const int link_count = ed::GetSelectedLinks(link_handles.data(), count);
    for (int i = 0; i < link_count; ++i) {
        sel.link_ids.push_back("e" +
                               std::to_string(link_handles[static_cast<std::size_t>(i)].Get()));
    }
    return sel;
}

void interactorClearSelection()
{
    ed::ClearSelection();
}

// ---------------------------------------------------- 连线创建（契约内置）----
// imgui-node-editor v0.9.3：ed::BeginCreate() 内部会调用
// CreateItemAction::Begin()（其中 IM_ASSERT(false == m_InActive); m_InActive = true;），
// 只有 ed::EndCreate() 会把它复位为 false。因此 Begin/End **必须成对调用**，
// 不能用返回值提前 return（否则下一帧 Begin 时断言 IM_ASSERT(false == m_InActive) 失败）。
// 官方示例（widgets-example / blueprints-example）同样是无条件调用 EndCreate()。
void interactorCreateLink(const CanvasInteract& interact)
{
    const bool creating = ed::BeginCreate(kCreateAcceptColor, kCreateThickness);

    ed::PinId start_pin;
    ed::PinId end_pin;
    if (creating && ed::QueryNewLink(&start_pin, &end_pin) && start_pin && end_pin) {
        PinRef from;
        PinRef to;
        const bool resolved =
            interact.resolve_pin ? interact.resolve_pin(start_pin, end_pin, from, to) : false;

        if (!resolved) {
            ed::RejectNewItem(kCreateRejectColor, kCreateThickness);
        }
        else {
            // 统一成「输出 → 输入」
            if (from.direction == engine::PortDirection::Input &&
                to.direction == engine::PortDirection::Output) {
                std::swap(from, to);
            }

            if (!from.valid || !to.valid) {
                ed::RejectNewItem(kCreateRejectColor, kCreateThickness);
            }
            else {
                std::string reason;
                bool        would_replace = false;
                const bool  ok = interact.can_connect
                                     ? interact.can_connect(from, to, reason, would_replace)
                                     : false;
                if (!ok) {
                    ed::RejectNewItem(kCreateRejectColor, kCreateThickness);
                    if (ImGui::IsMouseReleased(0) && interact.on_link_rejected) {
                        interact.on_link_rejected(reason);
                    }
                }
                else if (ed::AcceptNewItem(kCreateAcceptColor, kCreateThickness)) {
                    if (interact.on_link_accepted) {
                        interact.on_link_accepted(from, to, would_replace);
                    }
                }
            }
        }
    }

    // 拖拽提示（主画布特有；QueryNewLink 已调用过，end_pin 可能为 0 = 还没拖到目标端口）
    if (creating && start_pin.Get() != 0 && interact.on_create_tip) {
        interact.on_create_tip(start_pin, end_pin);
    }

    ed::EndCreate(); // 无条件配对（契约）
}

// ---------------------------------------------------- 拖出删除（契约内置）----
// BeginDelete/EndDelete 必须无条件配对（契约同 BeginCreate）。
void interactorDeleteOuter(const CanvasInteract& interact, const std::string& link_label,
                           const std::string& node_label)
{
    const bool deleting = ed::BeginDelete();
    if (deleting) {
        ed::LinkId deleted_link;
        while (ed::QueryDeletedLink(&deleted_link)) {
            if (ed::AcceptDeletedItem()) {
                if (interact.on_delete_batch) {
                    interact.on_delete_batch(link_label);
                }
                if (interact.delete_link) {
                    interact.delete_link("e" + std::to_string(deleted_link.Get()));
                }
            }
        }

        ed::NodeId deleted_node;
        while (ed::QueryDeletedNode(&deleted_node)) {
            if (ed::AcceptDeletedItem()) {
                if (interact.on_delete_batch) {
                    interact.on_delete_batch(node_label);
                }
                if (interact.delete_node) {
                    interact.delete_node("n" + std::to_string(deleted_node.Get()));
                }
            }
        }
    }
    ed::EndDelete(); // 与 BeginDelete 无条件配对（契约同 BeginCreate）
}

// ---------------------------------------------------- 选中删除 ------------
// Delete 键 / 工具栏 / 菜单「删除选中」共用（一次批次只压一次撤销快照；
// 无选中时调用方已前置静默 —— 与 Delete 键原行为一致）。
void interactorDeleteSelection(const CanvasInteract& interact, const SelectionRead& selection,
                               const std::function<void()>& batch_snapshot)
{
    if (selection.node_ids.empty() && selection.link_ids.empty()) {
        return;
    }
    if (batch_snapshot) {
        batch_snapshot(); // 一次批次只压一次快照
    }
    for (const std::string& node_id : selection.node_ids) {
        if (interact.delete_node) {
            interact.delete_node(node_id);
        }
    }
    for (const std::string& link_id : selection.link_ids) {
        if (interact.delete_link) {
            interact.delete_link(link_id);
        }
    }
    interactorClearSelection(); // 清掉内部选中 → 参数编辑区回落「未选中」（下一帧再同步）
}

// ---------------------------------------------------- 选中同步 ------------
void interactorSyncSelection(const CanvasInteract& interact)
{
    if (!interact.on_selection) {
        return;
    }
    const SelectionRead sel = interactorReadSelection();
    interact.on_selection(sel.node_ids, sel.link_ids);
}

// ---------------------------------------------------- 位置同步 ------------
void interactorPushNodePosition(const std::string& node_id, float x, float y)
{
    ed::SetNodePosition(nodeHandle(node_id), ImVec2(x, y));
}

PositionRead interactorReadNodePosition(const std::string& node_id, float model_x, float model_y,
                                        float clamp_limit, float epsilon)
{
    const ImVec2 position = ed::GetNodePosition(nodeHandle(node_id));

    PositionRead result;
    result.x = position.x;
    result.y = position.y;
    // 编辑器已认识该节点 → 有限性 + 上限（ed::GetNodePosition 对未知手柄返回 FLT_MAX，
    // 见 imgui_node_editor.cpp:1676）
    result.editor_knows = std::isfinite(position.x) && std::isfinite(position.y) &&
                          std::fabs(position.x) < kCanvasPositionLimit &&
                          std::fabs(position.y) < kCanvasPositionLimit;

    if (!result.editor_knows) {
        // 编辑器不认识的节点：模型 → 编辑器推位置，**绝不把脏值回写模型**
        ed::SetNodePosition(nodeHandle(node_id), ImVec2(model_x, model_y));
        result.changed = false;
        return result;
    }

    if (clamp_limit > 0.0f) {
        result.x = clampValue(position.x, clamp_limit);
        result.y = clampValue(position.y, clamp_limit);
    }
    result.changed = std::fabs(result.x - model_x) > epsilon ||
                     std::fabs(result.y - model_y) > epsilon;
    return result;
}

// ---------------------------------------------------- 快捷键窗口判定 ------
// 主画布：画布窗口聚焦（含子窗口）或悬停（历史 Delete 键窗口条件）
bool interactorWindowActiveMain()
{
    return ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) ||
           ImGui::IsWindowHovered();
}

// ce：根窗口（上级节点编辑器主窗口）聚焦/悬停（RootAndChildWindows：顶栏 /
// 规格面板 / 画布都算 —— DEBT-B1 修复语义）
bool interactorWindowActiveComposite()
{
    return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
           ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
}

} // namespace aiwrite::ui

