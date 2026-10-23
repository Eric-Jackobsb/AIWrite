#include "clipboard.h"

#include <algorithm> // std::find（选区内部连线判定）

#include "log.h"

namespace aiwrite::ui {

std::size_t Clipboard::copy(const std::vector<std::string>& node_ids, const engine::Graph& graph)
{
    clear();
    if (node_ids.empty()) {
        return 0;
    }
    for (const std::string& id : node_ids) {
        if (const engine::Node* node = graph.findNode(id)) {
            nodes_.push_back(*node);
        }
    }
    // 只复制选区内部的连线
    for (const engine::Edge& edge : graph.edges) {
        const bool from_selected = std::find(node_ids.begin(), node_ids.end(), edge.from_node) !=
                                   node_ids.end();
        const bool to_selected = std::find(node_ids.begin(), node_ids.end(), edge.to_node) !=
                                 node_ids.end();
        if (from_selected && to_selected) {
            edges_.push_back(edge);
        }
    }
    return nodes_.size();
}

Clipboard::PasteResult Clipboard::paste(engine::Graph& graph)
{
    PasteResult result;

    constexpr float kOffset = 40.0f;
    std::vector<std::pair<std::string, std::string>> id_map; // 原 id → 新 id

    for (const engine::Node& source : nodes_) {
        std::string error;
        const std::string new_id =
            graph.addNode(source.type, source.x + kOffset, source.y + kOffset, &error);
        if (new_id.empty()) {
            log::error("粘贴节点失败: " + error);
            continue;
        }
        if (engine::Node* node = graph.findNode(new_id)) {
            node->title  = source.title;
            node->params = source.params; // 一并复制参数值
        }
        id_map.emplace_back(source.id, new_id);
        result.node_ids.push_back(new_id);
    }

    for (const engine::Edge& edge : edges_) {
        std::string from_id;
        std::string to_id;
        for (const auto& entry : id_map) {
            if (entry.first == edge.from_node) {
                from_id = entry.second;
            }
            if (entry.first == edge.to_node) {
                to_id = entry.second;
            }
        }
        if (from_id.empty() || to_id.empty()) {
            continue;
        }
        std::string error;
        const std::string edge_id =
            graph.addEdge(from_id, edge.from_port, to_id, edge.to_port, &error);
        if (!edge_id.empty()) {
            // 回取完整 Edge（含新 id）作为 EdgeAdded 事件负载
            if (const engine::Edge* added = graph.findEdge(edge_id)) {
                result.added_edges.push_back(*added);
            }
        }
    }

    return result;
}

} // namespace aiwrite::ui
