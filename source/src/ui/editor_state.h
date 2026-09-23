#pragma once

// ============================================================================
//  编辑器状态（UI 层共享）
//
//  * 持有唯一的工作流图 Graph 与撤销栈 UndoStack
//  * 提供"修改前自动压快照"的编辑操作，供画布 / 节点库 / 工具栏 / 菜单调用
//  * 所有可验证的逻辑都在 engine::Graph 内，本层只做编排与日志
// ============================================================================

#include <string>
#include <utility>
#include <vector>

#include "engine/graph.h"
#include "engine/undo_stack.h"

namespace aiwrite::ui {

struct EditorState {
    engine::Graph      graph;
    engine::UndoStack  undo;

    // 画布每帧同步的选择信息
    std::vector<std::string> selected_nodes;
    std::vector<std::string> selected_links;       // 选中的连线（左键点连线 / 框选）
    std::string              selected_node;        // 单选时用于参数面板

    std::string status;                       // 状态栏提示

    // 一次性请求（由画布消费）
    bool request_focus_title          = false; // 双击节点标题 → 参数面板聚焦标题输入框
    bool request_navigate_to_content  = false; // 新建/粘贴后视图跟随内容

    // ------------------------------------------------------------- 编辑 -----
    void        snapshot(const std::string& label);   // 修改前压快照
    bool        undo_once();
    bool        redo_once();
    void        clear_workflow();
    void        create_sample_workflow();
    std::string create_node(const std::string& type, float x, float y); // 返回新节点 id

    bool        delete_selected();
    bool        delete_node(const std::string& node_id);

    // ---- 复制/粘贴 ----
    // 暂停接线（UI 入口已下线，见 CHANGELOG「复制/粘贴暂停」）：逻辑与剪贴板代码保留，
    // M2/M3 重做画布交互时接回；接回前必须先修好画布位置同步（node_canvas.cpp sync_positions）。
    void        copy_selection();
    bool        paste_clipboard();
    bool        has_clipboard() const { return !clipboard_nodes_.empty(); }
    bool        has_selection() const { return !selected_nodes.empty() || !selected_links.empty(); }
    void        set_status(const std::string& text);

    void        log_summary(const std::string& prefix) const;

private:
    std::vector<engine::Node> clipboard_nodes_;                        // 复制的节点
    std::vector<engine::Edge> clipboard_edges_;                        // 选区内部的连线
};

// 全局单例（UI 生命周期内唯一）
EditorState& editor();

} // namespace aiwrite::ui
