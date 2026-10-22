#include "failure_summary.h"

namespace aiwrite::engine {

std::vector<FailureSummary> collectFailures(const Graph& graph)
{
    std::vector<FailureSummary> out;
    for (const Node& node : graph.nodes) {
        if (node.state != NodeState::Error) {
            continue;
        }
        FailureSummary item;
        item.node_id = node.id;
        item.message = node.error_message.empty() ? std::string("执行失败") : node.error_message;
        out.push_back(std::move(item));
    }
    return out;
}

std::string summarizeFailures(const Graph& graph)
{
    const std::vector<FailureSummary> failures = collectFailures(graph);
    if (failures.empty()) {
        return {};
    }
    std::string out = std::to_string(failures.size()) + " 个节点失败：";
    for (std::size_t i = 0; i < failures.size(); ++i) {
        if (i > 0) {
            out += "；";
        }
        out += failures[i].node_id;
        if (!failures[i].message.empty()) {
            out += "（" + failures[i].message + "）";
        }
    }
    return out;
}

} // namespace aiwrite::engine
