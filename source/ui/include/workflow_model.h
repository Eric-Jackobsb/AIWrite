#pragma once

// ============================================================================
//  文档级工作流模型（ARCH-E3：WorkflowModel 拆分 · 阶段 2 编辑侧事件化）
//
//  架构落点：ARCHITECTURE.md §1.8.3 `WorkflowModel` / §1.8.11 依赖图 / 决策 ARCH-D26 / D29
//   * 持有当前工作流的全部节点、连线（唯一真相 `graph`）与独立撤销栈（`undo`）
//   * 提供增删改节点、连线、参数的操作，修改后发出编辑事件（EditEvent / before 通道）
//   * 不执行节点、不校验、不保存文件、不渲染界面、不持有选中状态（§1.8.3 限制）
//   * 依赖 EventBus（§1.8.11 第 5 位初始化）；Undo 订阅「修改前」通道压快照
//
//  E3 范围（本批）：Graph + Undo + EventBus 从 `EditorState` 拆出为文档级对象；
//  编辑操作（snapshot / undo / redo / clear / load / createNode / deleteNode）归本对象，
//  UI 编排（状态栏 / 选中清空 / autosave baseline / 剪贴板）仍留在 `EditorState`。
//  过渡登记：`EditorState` 持有本对象（`editor().model()`）；`editor()` 退场（ARCH-A3）
//  后 UI 模块直接持有 `WorkflowModel`；SelectionState 拆分留 ARCH-E4。
// ============================================================================

#include <cstddef>
#include <functional>
#include <string>

#include "event_bus.h"
#include "graph.h"
#include "undo_stack.h"

namespace aiwrite::ui {

// 文档级工作流模型（§1.8.3）：持有 Graph + UndoStack + EventBus
class WorkflowModel {
public:
    WorkflowModel();
    ~WorkflowModel();

    engine::Graph      graph;   // 当前工作流数据（唯一真相；§1.8.3）
    engine::UndoStack  undo;    // 撤销栈（ARCH-D26：由 WorkflowModel 持有）

    // 事件总线（§1.8.11：WorkflowModel 依赖 EventBus；订阅方经此注册）
    EventBus&       event_bus() { return event_bus_; }
    const EventBus& event_bus() const { return event_bus_; }

    // 修改前钩子（过渡）：EditorState 注入「运行中修改先终止运行」（M2-04 守卫）。
    // 在压快照**之前**执行（与 E2 回调顺序一致：先 abortRunIfAny 再 undo.push）；
    // RunSession 自己感知编辑事件（ARCH-D22）后移除本钩子。
    void setBeforeEditHook(std::function<void(const std::string&)> hook);

    // 修改前通知：发布 before-edit 事件（压快照由 Undo 订阅回调完成；本方法只发通知）
    void snapshot(const std::string& label);

    // 撤销 / 重做：整图替换 + 逐节点 diff 事件；无可撤销/重做时返回 false（label 不写）
    bool undoOnce(std::string* label = nullptr);
    bool redoOnce(std::string* label = nullptr);

    // 整图重置（ARCH-D29）：替换数据 + 清空撤销栈 + 逐节点 diff 事件
    //  * clear = 清空为空白工作流；load = 载入 source（替换；调用方用 std::move 保留拷贝）
    //  * 不碰选中 / 状态栏 / autosave baseline（编排归 EditorState）
    void clear();
    void load(engine::Graph source);

    // 单点编辑：修改后发布对应 EditEvent（NodeAdded / NodeRemoved）
    //  * createNode 失败返回空串并把原因写入 error（可空）
    std::string createNode(const std::string& type, float x, float y,
                           std::string* error = nullptr);
    bool        deleteNode(const std::string& node_id);

    // 事件发布转发（画布 / 参数面板等直接改 graph 后发通知；与 event_bus() 等价）
    void publish(const EditEvent& event) { event_bus_.publish(event); }

private:
    // ARCH-E2：整图重置（undo/redo/clear/load）后逐节点 diff 事件
    //  （NodeRemoved 隐含连带连线，端点已删的连线不重复发 EdgeRemoved）
    void publishGraphDiff(const engine::Graph& before, const engine::Graph& after);

    EventBus event_bus_;                   // 通信层对象（§1.8.11 依赖第 5 位）
    std::size_t undo_sub_handle_ = 0;      // Undo 订阅「修改前」通道的句柄
    std::function<void(const std::string&)> before_edit_hook_; // 修改前钩子（过渡）
};

} // namespace aiwrite::ui
