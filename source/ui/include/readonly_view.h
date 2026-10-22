#pragma once

// ============================================================================
//  渲染只读视图（ARCH-R3 · ARCHITECTURE.md §1.4 / §1.8.5 `ReadOnlyView`）
//
//  * **每帧构建一次**：主循环统一构建（ARCH-D30），传给所有渲染模块
//  * **只存引用、零拷贝**（ARCH-D34）：内部只持有 `const Graph&` 一个数据引用，
//    工作流数据经访问器返回 graph 内部数组的 const 引用；不拷贝、不修改、不跨帧存活
//  * **字段 = 三个核心 + 配置**（ARCH-D31）：工作流数据 / 选中 / 运行快照 / 配置
//  * **快照为空 = 空对象**（ARCH-D32）：`RunSnapshot::running = false` 即未运行
//  * **渲染签名** `render(const ReadOnlyView&)`（ARCH-D33）
//  * **使用者**：所有渲染模块（画布、参数面板、输出面板、状态栏）（ARCH-D35）
//
//  ARCH-E4：`SelectionReadView` 过渡结构已由独立对象 `SelectionState` 替换（§1.8.4 /
//  ARCH-D28）—— 本视图 `selection` 字段 = `const SelectionState&`；渲染调用点不变
//  （`view.selection.nodes()` / `.links()` / `.single()`，访问器名与过渡字段一致）。
//  `graph()` 为**过渡访问器**：画布绘制层（canvas_view）仍需整图只读查询
//  （findNode / canConnect 等），E5 收敛交互后逐步收窄为 nodes/edges + 查询方法。
// ============================================================================

#include <string>
#include <vector>

#include "config.h"
#include "executor.h"
#include "graph.h"
#include "selection_state.h" // ARCH-E4：选中状态独立对象（§1.8.4）

namespace aiwrite::ui {

struct ReadOnlyView {
    // ---- 工作流数据（零拷贝：返回 graph 内部数组 const 引用）----
    const std::vector<engine::Node>& nodes_for_render() const { return graph_.nodes; }
    const std::vector<engine::Edge>& edges_for_render() const { return graph_.edges; }
    const engine::Node* find_node(const std::string& node_id) const {
        return graph_.findNode(node_id);
    }
    const engine::Edge* find_edge(const std::string& edge_id) const {
        return graph_.findEdge(edge_id);
    }
    // 过渡访问器：绘制层整图只读查询（见文件头过渡说明）
    const engine::Graph& graph() const { return graph_; }

    // ---- 三个核心 + 配置 ----
    const SelectionState&    selection;  // 选中状态（ARCH-E4：独立对象，§1.8.4）
    const engine::RunSnapshot& snapshot;  // 运行快照（空对象 = 未运行）
    const Config&              config;    // 应用配置

    ReadOnlyView(const engine::Graph& graph, const SelectionState& sel,
                 const engine::RunSnapshot& snap, const Config& cfg)
        : selection(sel), snapshot(snap), config(cfg), graph_(graph) {}

private:
    const engine::Graph& graph_;  // 唯一持有的数据引用
};

} // namespace aiwrite::ui

