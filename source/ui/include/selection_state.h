#pragma once

// ============================================================================
//  选中状态（ARCH-E4：SelectionState 拆分 · 阶段 2 编辑侧事件化）
//
//  架构落点：ARCHITECTURE.md §1.8.4 `SelectionState` / §1.8.11 依赖图 / 决策 ARCH-D28
//   * 持有当前选中的节点 id 集合（+ 连线 id + 单选节点 id），提供增删改查接口
//   * 变化时发「选中变化」编辑事件（`EditEvent::SelectionChanged`）
//   * 不持有工作流数据、不渲染、不执行（§1.8.4 限制）
//   * 依赖图 §1.8.11 第 6 位：**无依赖**（不 include EventBus）—— 变化通知经
//     `set_changed_callback` 注入（EditorState 构造时注入 → 发 SelectionChanged 到
//     `model().event_bus()`；SelectionChanged 为通知形态，无内容消费者，autosave 不订阅）
//   * 变化检测内置：每个修改方法先比较再赋值，仅真正变化才触发回调（避免每帧高频通知，
//     与 E1 `sync_selection` 语义一致）
//   * 生命周期：文档级（加载工作流时清空，§1.8.6）
//
//  E4 范围（本批）：选中集合从 `EditorState` 拆出为独立对象；`ReadOnlyView` 的
//  `SelectionReadView` 过渡结构替换为本对象 const 引用（渲染调用点不变）；
//  UI 编排（状态栏 / 视图跟随 / autosave baseline）仍留在 `EditorState`。
//  过渡登记：`EditorState` 持有本对象（`editor().selection()`）；`editor()` 退场
//  （ARCH-A3）后 UI 模块直接持有 `SelectionState`；剪贴板独立留 ARCH-A1。
// ============================================================================

#include <functional>
#include <string>
#include <vector>

namespace aiwrite::ui {

// 文档级选中状态（§1.8.4）：节点 / 连线 / 单选节点 id 集合 + 变化通知（回调注入）
class SelectionState {
public:
    // 只读访问（ReadOnlyView 引用本对象；访问器名与过渡 `SelectionReadView` 字段一致，
    // 渲染调用点不变：`view.selection.nodes()` / `.links()` / `.single()`）
    const std::vector<std::string>& nodes() const { return nodes_; }
    const std::vector<std::string>& links() const { return links_; }
    const std::string& single() const { return single_; }

    // 查询
    bool empty() const { return nodes_.empty() && links_.empty(); }
    bool has_nodes() const { return !nodes_.empty(); }
    bool has_links() const { return !links_.empty(); }
    bool is_selected(const std::string& node_id) const;

    // 修改（内置变化检测：仅真正变化才触发 changed 回调；回调内按注入顺序分发）
    // 画布同步（sync_selection）：一次设置三组集合 + 一次变化检测（每帧调用安全）
    void set_selection(const std::vector<std::string>& nodes,
                       const std::vector<std::string>& links, const std::string& single);
    void select_node(const std::string& node_id); // nodes={id} + single=id（画布创建/右键/双击）
    void set_nodes(const std::vector<std::string>& ids); // 只改 nodes_（single_ 不动）
    void set_single(const std::string& id);             // 只改 single_
    void remove_node(const std::string& node_id); // nodes_ 移除 + 若 single==id 清 single
    void clear_nodes();  // 清 nodes_ + single_（**不清** links_ —— 与现有 undo/redo/clear 行为一致）
    void clear_links();  // 清 links_
    void prune_links(std::function<bool(const std::string&)> keep); // 保留满足 keep 的连线（ce 热重载清理失效引用）
    void clear();        // 全清（打开/恢复工作流）

    // 变化回调（无依赖注入）：EditorState 构造时注入 → 发 `EditEvent::SelectionChanged`
    void set_changed_callback(std::function<void()> callback);

private:
    std::vector<std::string> nodes_;   // 选中的节点 id（画布同步 / 状态栏计数）
    std::vector<std::string> links_;   // 选中的连线 id
    std::string              single_;  // 单选节点 id（参数面板用）
    std::function<void()>    changed_callback_;
};

} // namespace aiwrite::ui
