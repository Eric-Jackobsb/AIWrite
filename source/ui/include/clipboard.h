#pragma once

// ============================================================================
//  剪贴板（ARCH-A1：应用级服务收口 → AppContext · 六个服务之一）
//
//  架构落点：ARCHITECTURE.md §1.8 `AppContext` / 决策 ARCH-A1
//   * 从 `EditorState` 拆出的**纯数据 + 注入**服务：不持有 EventBus / 选中 / 图引用
//   * 复制时调用方传入选区节点 id 与源图（深拷贝到本对象）；
//     粘贴时把新建结果注入目标图，并由调用方发布事件 / 更新选中
//   * 语义与拆出前逐字一致（CHANGELOG「复制/粘贴暂停」说明仍适用：UI 入口已下线，
//     函数保留待 M2/M3 重做画布交互时接回；位置守卫在 node_canvas.cpp）
// ============================================================================

#include <string>
#include <vector>

#include "graph.h" // engine::Node / engine::Edge

namespace aiwrite::ui {

class Clipboard {
public:
    // 清空剪贴板（新建 / 复制前重置）
    void clear() { nodes_.clear(); edges_.clear(); }

    // 复制选区节点 + 选区内部连线（深拷贝）；返回复制的节点数
    //  * 连线仅复制 from/to 两端都在选区内的（与拆出前语义一致）
    std::size_t copy(const std::vector<std::string>& node_ids, const engine::Graph& graph);

    bool     has_content() const { return !nodes_.empty(); }
    std::size_t node_count() const { return nodes_.size(); }

    // 粘贴结果（调用方据此发布 NodeAdded / EdgeAdded 事件并更新选中集合）
    struct PasteResult {
        std::vector<std::string> node_ids;    // 新建节点 id（按复制顺序）
        std::vector<engine::Edge> added_edges; // 新建连线（含新 id；EdgeAdded 负载）
    };

    // 粘贴到目标图：每个节点偏移 +40,+40 深拷贝；选区内部连线重新接线到新节点
    PasteResult paste(engine::Graph& graph);

private:
    std::vector<engine::Node> nodes_; // 复制的节点
    std::vector<engine::Edge> edges_; // 选区内部的连线
};

} // namespace aiwrite::ui
