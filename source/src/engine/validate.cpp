#include "engine/validate.h"

#include "engine/node_registry.h"
#include "engine/provider_resolve.h"

#include <algorithm>
#include <cstdlib>

namespace aiwrite::engine {
namespace {

std::string edge_label(const Edge& edge)
{
    return "[边 " + (edge.id.empty() ? std::string("?") : edge.id) + "] ";
}

bool contains_id(const std::vector<std::string>& ids, const std::string& id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

bool validateWorkflow(const Graph& graph, ValidationMessages* errors)
{
    ValidationMessages found;

    if (graph.nodes.empty()) {
        found.push_back("工作流为空：请先添加节点");
    }

    registerAllNodes();

    // ---- 节点：类型已注册、id 唯一、参数合法 ----
    std::vector<std::string> seen_ids;
    seen_ids.reserve(graph.nodes.size());
    for (const Node& node : graph.nodes) {
        if (NodeRegistry::instance().find(node.type) == nullptr) {
            found.push_back("[" + node.id + "] 未知节点类型: " + node.type);
            continue;
        }
        if (contains_id(seen_ids, node.id)) {
            found.push_back("[" + node.id + "] 节点 id 重复");
            continue;
        }
        seen_ids.push_back(node.id);

        ValidationMessages param_errors;
        graph.validateParams(node, &param_errors);
        for (const std::string& text : param_errors) {
            found.push_back("[" + node.id + "] " + text);
        }
    }

    // ---- 边：两端节点/端口存在、类型兼容、非可变长输入不重复占用 ----
    for (const Edge& edge : graph.edges) {
        const Node* from = graph.findNode(edge.from_node);
        const Node* to   = graph.findNode(edge.to_node);
        if (from == nullptr) {
            found.push_back(edge_label(edge) + "起点节点不存在: " + edge.from_node);
            continue;
        }
        if (to == nullptr) {
            found.push_back(edge_label(edge) + "终点节点不存在: " + edge.to_node);
            continue;
        }

        const Port* from_port = from->findPort(edge.from_port, PortDirection::Output);
        const Port* to_port   = to->findPort(edge.to_port, PortDirection::Input);
        if (from_port == nullptr) {
            found.push_back(edge_label(edge) + "起点端口不存在: " + edge.from_node + "." +
                            edge.from_port);
            continue;
        }
        if (to_port == nullptr) {
            found.push_back(edge_label(edge) + "终点端口不存在: " + edge.to_node + "." +
                            edge.to_port);
            continue;
        }
        if (!Graph::isPortTypeCompatible(from_port->type, to_port->type)) {
            found.push_back(edge_label(edge) + "端口类型不兼容: " + portTypeName(from_port->type) +
                            " → " + portTypeName(to_port->type));
        }
        if (!to_port->is_variadic && graph.inputConnectionCount(edge.to_node, edge.to_port) > 1) {
            found.push_back(edge_label(edge) + "输入端口被重复占用: " + edge.to_node + "." +
                            edge.to_port);
        }
    }

    // ---- 环 ----
    std::string order_error;
    if (!graph.topologicalOrder(nullptr, &order_error)) {
        found.push_back(order_error);
    }

    if (errors != nullptr) {
        *errors = found;
    }
    return found.empty();
}

bool validateBeforeRun(const Graph& graph, ValidationMessages* errors,
                       ValidationMessages* warnings)
{
    ValidationMessages found;
    ValidationMessages notes;

    validateWorkflow(graph, &found); // 加载校验是运行前校验的子集

    bool has_provider = false;
    for (const Node& node : graph.nodes) {
        // 必填输入已连接（可变长输入至少 1 条边）
        for (const Port& port : node.inputs) {
            if (port.is_optional) {
                continue;
            }
            const int connections = graph.inputConnectionCount(node.id, port.id);
            if (port.is_variadic) {
                if (connections == 0) {
                    found.push_back("[" + node.id + "] 可变长输入「" + port.display_name +
                                    "」至少需要 1 条连线");
                }
            }
            else if (connections == 0) {
                found.push_back("[" + node.id + "] 必填输入「" + port.display_name + "」未连接");
            }
        }

        if (node.type != "ProviderConfig") {
            continue;
        }
        has_provider = true;

        const Param*      mode       = node.findParam("mode");
        const std::string mode_value = mode != nullptr ? mode->text() : "official";
        if (mode_value == "web") {
            notes.push_back("[" + node.id +
                            "] 网页版模式：请确认已在参数面板完成登录（会话只存内存；推理接线见 M4）");
            continue;
        }

        const Param* key       = node.findParam("api_key");
        const bool   in_params = key != nullptr && !key->is_empty();
        const char*  env_key   = std::getenv("DEEPSEEK_API_KEY");
        const bool   in_env    = env_key != nullptr && *env_key != '\0';
        if (!in_params && !in_env) {
            notes.push_back("[" + node.id +
                            "] 未配置 API Key（参数或环境变量 DEEPSEEK_API_KEY）：运行时将无法调用"
                            "官方 API");
        }
    }

    // ---- P1-b：推理节点「本次运行必定失败」提示（刻意保持 warning，不阻断运行）----
    //  * official（官方 API）分支尚未接线（归口 PB-04/PB-05）；多模态分支尚未接线（M5-02）
    //  * 为何不升级为 error：自检（--run-selftest）与 api_probe 断言要求运行前校验「不阻断」
    //    （见 api_probe「未配置 Key：运行前校验仍通过」）。真正的拦截在 UI 层：工具栏弹窗确认。
    for (const Node& node : graph.nodes) {
        const std::string reason = unwired_reason(graph, node);
        if (reason.empty()) {
            continue;
        }
        const EffectiveProvider effective = resolve_effective_provider(graph, node);
        std::string              text     = "[" + node.id + "] " + reason;
        if (node.type == "LLMGenerate") {
            text += effective.from_edge
                        ? "（生效模式 " + effective.mode + " 来自 " + effective.source_node + "）"
                        : "（生效模式 " + effective.mode + " 来自节点自身设置）";
        }
        text += "：本次运行该节点必定失败，下游会被跳过；网页版（provider.mode = web）已接线，"
                "需先在参数面板完成一次登录";
        notes.push_back(text);
    }

    if (!has_provider && !graph.nodes.empty()) {
        notes.push_back("工作流没有「提供商配置」节点：推理节点的 provider 输入将为空");
    }

    if (errors != nullptr) {
        *errors = found;
    }
    if (warnings != nullptr) {
        *warnings = notes;
    }
    return found.empty();
}

} // namespace aiwrite::engine
