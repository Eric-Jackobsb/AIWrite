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
//  过渡说明（R3 落地形态）：
//  * `SelectionState` 尚未拆分（ARCH-E4），本批先用 `SelectionReadView`（只读引用
//    聚合）承载选中字段；E4 落地后替换为 `const SelectionState&`，调用点不变。
//  * `graph()` 为**过渡访问器**：画布绘制层（canvas_view）仍需整图只读查询
//    （findNode / canConnect 等），E5 收敛交互后逐步收窄为 nodes/edges + 查询方法。
// ============================================================================

#include <string>
#include <vector>

#include "config.h"
#include "executor.h"
#include "graph.h"

namespace aiwrite::ui {

// 选中状态只读视图（ARCH-E4 拆出 `SelectionState` 后替换为该对象引用）
struct SelectionReadView {
    const std::vector<std::string>& nodes;   // 选中的节点 id（画布同步 / 状态栏计数）
    const std::vector<std::string>& links;   // 选中的连线 id
    const std::string&              single;  // 单选节点 id（参数面板用）
};

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
    const SelectionReadView&  selection;  // 选中状态（E4 后 = SelectionState）
    const engine::RunSnapshot& snapshot;  // 运行快照（空对象 = 未运行）
    const Config&              config;    // 应用配置

    ReadOnlyView(const engine::Graph& graph, const SelectionReadView& sel,
                 const engine::RunSnapshot& snap, const Config& cfg)
        : selection(sel), snapshot(snap), config(cfg), graph_(graph) {}

private:
    const engine::Graph& graph_;  // 唯一持有的数据引用
};

} // namespace aiwrite::ui
