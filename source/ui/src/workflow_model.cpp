// ============================================================================
//  文档级工作流模型实现（ARCH-E3）
//
//  * Undo 订阅「修改前」通道：回调 = 修改前钩子（abortRunIfAny，过渡）→ undo.push
//    （顺序与 E2 一致：先终止运行，再压「无运行状态」的快照）
//  * 单点操作（createNode / deleteNode）：修改后 publish(EditEvent)
//  * 整图重置（undo / redo / clear / load）：替换后 publishGraphDiff 逐节点 diff
//  * 本对象不 setStatus / 不碰选中 / 不写盘（编排归 EditorController）
// ============================================================================

#include "workflow_model.h"

#include <algorithm>

namespace aiwrite::ui {

WorkflowModel::WorkflowModel()
{
    // ARCH-E2：Undo 订阅「修改前」通道 —— 编辑操作修改前经 publish_before_edit 通知，
    // 快照式撤销在此压栈（snapshot() 只发通知，不再直接操作 undo 栈）。
    undo_sub_handle_ = event_bus_.subscribe_before_edit(
        [this](const std::string& label) {
            // 修改前钩子（EditorController 注入的 abortRunIfAny：运行中修改工作流 → 计划与图
            // 不再一致，先终止本次运行，M2-04 守卫）—— 先于压快照执行（顺序与 E2 一致）
            if (before_edit_hook_) {
                before_edit_hook_(label);
            }
            undo.push(graph, label);
        });
}

WorkflowModel::~WorkflowModel()
{
    event_bus_.unsubscribe_before_edit(undo_sub_handle_);
}

void WorkflowModel::setBeforeEditHook(std::function<void(const std::string&)> hook)
{
    before_edit_hook_ = std::move(hook);
}

// ARCH-E2：修改前压快照由 Undo 订阅者完成；本方法只发布「修改前」通知
void WorkflowModel::snapshot(const std::string& label)
{
    event_bus_.publish_before_edit(label);
}

bool WorkflowModel::undoOnce(std::string* label)
{
    engine::Graph restored;
    std::string   lbl;
    if (!undo.undo(graph, restored, &lbl)) {
        return false;
    }
    const engine::Graph before = graph; // 撤销 = 整图替换 → 逐节点 diff 事件
    graph = std::move(restored);
    if (label != nullptr) {
        *label = lbl;
    }
    publishGraphDiff(before, graph);
    return true;
}

bool WorkflowModel::redoOnce(std::string* label)
{
    engine::Graph restored;
    std::string   lbl;
    if (!undo.redo(graph, restored, &lbl)) {
        return false;
    }
    const engine::Graph before = graph; // 重做 = 整图替换 → 逐节点 diff 事件
    graph = std::move(restored);
    if (label != nullptr) {
        *label = lbl;
    }
    publishGraphDiff(before, graph);
    return true;
}

void WorkflowModel::clear()
{
    const engine::Graph before = graph; // 清空 = 整图重置 → 逐节点 diff 事件
    graph.clear();
    undo.clear();
    publishGraphDiff(before, graph);
}

void WorkflowModel::load(engine::Graph source)
{
    const engine::Graph before = graph; // 加载 = 整图替换 → 逐节点 diff 事件
    graph = std::move(source);
    undo.clear(); // 设计 §6.6：加载工作流清空撤销栈
    publishGraphDiff(before, graph);
}

std::string WorkflowModel::createNode(const std::string& type, float x, float y,
                                      std::string* error)
{
    std::string   add_error;
    const std::string id = graph.addNode(type, x, y, &add_error);
    if (id.empty()) {
        if (error != nullptr) {
            *error = add_error;
        }
        return {};
    }
    // ARCH-E1：编辑事件通知（形态 1：通知；消费者各自订阅处理）
    event_bus_.publish(EditEvent::node(EditEventKind::NodeAdded, id));
    return id;
}

bool WorkflowModel::deleteNode(const std::string& node_id)
{
    if (graph.findNode(node_id) == nullptr) {
        return false;
    }
    if (!graph.removeNode(node_id)) {
        return false;
    }
    // ARCH-E1：编辑事件通知（连带其连线由 NodeRemoved 隐含）
    event_bus_.publish(EditEvent::node(EditEventKind::NodeRemoved, node_id));
    return true;
}

// ARCH-E2：整图重置（undo/redo/clear/load）后的逐节点 diff 事件。
//  * 与单点操作同构：NodeRemoved 隐含连带连线（端点已删的连线不重复发 EdgeRemoved）
//  * 通知形态：消费者（autosave 等）只读事件，具体数据直接读当前图
void WorkflowModel::publishGraphDiff(const engine::Graph& before, const engine::Graph& after)
{
    std::vector<std::string> removed_nodes;
    for (const engine::Node& node : before.nodes) {
        if (after.findNode(node.id) == nullptr) {
            removed_nodes.push_back(node.id);
            event_bus_.publish(EditEvent::node(EditEventKind::NodeRemoved, node.id));
        }
    }
    for (const engine::Node& node : after.nodes) {
        if (before.findNode(node.id) == nullptr) {
            event_bus_.publish(EditEvent::node(EditEventKind::NodeAdded, node.id));
        }
    }
    for (const engine::Edge& edge : before.edges) {
        const bool endpoint_gone =
            std::find(removed_nodes.begin(), removed_nodes.end(), edge.from_node) !=
                removed_nodes.end() ||
            std::find(removed_nodes.begin(), removed_nodes.end(), edge.to_node) !=
                removed_nodes.end();
        if (!endpoint_gone && after.findEdge(edge.id) == nullptr) {
            event_bus_.publish(EditEvent::edge(EditEventKind::EdgeRemoved, edge));
        }
    }
    for (const engine::Edge& edge : after.edges) {
        if (before.findEdge(edge.id) == nullptr) {
            event_bus_.publish(EditEvent::edge(EditEventKind::EdgeAdded, edge));
        }
    }
}

} // namespace aiwrite::ui
