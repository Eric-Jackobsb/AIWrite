#include "engine/workflow_io.h"

#include "engine/node_registry.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace aiwrite::engine {
namespace {

// 数值合法性（与画布侧 kPositionLimit / 缩放范围保持一致）：
// 损坏或超范围的坐标与缩放一律回落到安全值，避免写坏文件、也避免读坏文件影响画布。
constexpr double kPositionLimit = 100000.0;
constexpr double kZoomMin       = 0.05;
constexpr double kZoomMax       = 10.0;

double sane_position(double value)
{
    if (!std::isfinite(value) || std::fabs(value) > kPositionLimit) {
        return 0.0;
    }
    return value;
}

double sane_zoom(double value)
{
    if (!std::isfinite(value) || value < kZoomMin || value > kZoomMax) {
        return 1.0;
    }
    return value;
}

} // namespace

// ------------------------------------------------------------------ 序列化 ----
nlohmann::json graph_to_json(const Graph& graph)
{
    nlohmann::json document;
    document["version"]     = kWorkflowVersion;
    document["name"]        = graph.name;
    document["description"] = graph.description;

    nlohmann::json nodes = nlohmann::json::array();
    for (const Node& node : graph.nodes) {
        nlohmann::json node_json;
        node_json["id"]       = node.id;
        node_json["type"]     = node.type;
        node_json["title"]    = node.title;
        node_json["position"] = {{"x", sane_position(node.x)}, {"y", sane_position(node.y)}};

        nlohmann::json params = nlohmann::json::object();
        for (const Param& param : node.params) {
            // 密钥类参数（is_secret，例如 API Key）**不写盘**：
            // 设计文档要求密钥只驻留内存，M4-07 起改用凭据管理器引用名（api_key_ref）
            if (param.is_secret) {
                continue;
            }
            params[param.id] = param.value;
        }
        node_json["params"] = std::move(params);

        nodes.push_back(std::move(node_json));
    }
    document["nodes"] = std::move(nodes);

    nlohmann::json edges = nlohmann::json::array();
    for (const Edge& edge : graph.edges) {
        nlohmann::json edge_json;
        edge_json["id"]   = edge.id;
        edge_json["from"] = {{"node", edge.from_node}, {"port", edge.from_port}};
        edge_json["to"]   = {{"node", edge.to_node}, {"port", edge.to_port}};
        edges.push_back(std::move(edge_json));
    }
    document["edges"] = std::move(edges);

    document["viewport"] = {{"x", sane_position(graph.viewport_x)},
                            {"y", sane_position(graph.viewport_y)},
                            {"zoom", sane_zoom(graph.viewport_zoom)}};
    return document;
}

// ------------------------------------------------------------------ 反序列化 --
bool graph_from_json(const nlohmann::json& json, Graph& graph, std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    if (!json.is_object()) {
        return fail("工作流 JSON 不是对象");
    }
    if (!json.contains("nodes") || !json["nodes"].is_array()) {
        return fail("缺少 nodes 数组");
    }

    if (json.contains("version") && json["version"].is_string()) {
        const std::string version = json["version"].get<std::string>();
        if (!version.empty() && version.rfind("1.", 0) != 0) {
            log::warn("[工作流] 文件版本 " + version + " 与当前 " + kWorkflowVersion +
                      " 不同，尝试按 1.x 规则读取");
        }
    }

    registerAllNodes();
    const NodeRegistry& registry = NodeRegistry::instance();

    Graph parsed;
    parsed.name        = json.value("name", std::string("未命名工作流"));
    parsed.description = json.value("description", std::string());

    // ---- 节点：端口与参数定义一律以注册表为准，参数值取自文件 ----
    for (const auto& node_json : json["nodes"]) {
        if (!node_json.is_object()) {
            continue;
        }
        const std::string type = node_json.value("type", std::string());
        if (type.empty()) {
            continue;
        }
        if (registry.find(type) == nullptr) {
            log::warn("[工作流] 跳过未注册的节点类型: " + type);
            continue;
        }

        const auto  position = node_json.value("position", nlohmann::json::object());
        const float x        = static_cast<float>(sane_position(position.value("x", 0.0)));
        const float y        = static_cast<float>(sane_position(position.value("y", 0.0)));

        const std::string generated_id = parsed.addNode(type, x, y, nullptr);
        if (generated_id.empty()) {
            continue;
        }
        Node* node = parsed.findNode(generated_id);
        if (node == nullptr) {
            continue;
        }

        // 尽量保留文件中的 id（边的引用要对得上）；若与已有节点冲突则保留自动生成的 id
        const std::string wanted_id = node_json.value("id", std::string());
        if (!wanted_id.empty() && wanted_id != generated_id &&
            parsed.findNode(wanted_id) == nullptr) {
            node->id = wanted_id;
        }

        const std::string wanted_title = node_json.value("title", std::string());
        if (!wanted_title.empty()) {
            node->title = wanted_title;
        }

        const auto params = node_json.find("params");
        if (params != node_json.end() && params->is_object()) {
            for (auto it = params->begin(); it != params->end(); ++it) {
                if (Param* param = node->findParam(it.key())) {
                    if (param->is_secret) {
                        continue; // 密钥不从文件恢复（只驻留内存）
                    }
                    param->value = it.value();
                }
            }
        }
    }

    // ---- 连线：统一走 addEdge（类型/端口/重复/自连校验），非法则跳过并告警 ----
    const auto edges = json.find("edges");
    if (edges != json.end() && edges->is_array()) {
        for (const auto& edge_json : *edges) {
            if (!edge_json.is_object()) {
                continue;
            }
            const auto from = edge_json.value("from", nlohmann::json::object());
            const auto to   = edge_json.value("to", nlohmann::json::object());

            const std::string from_node = from.value("node", std::string());
            const std::string from_port = from.value("port", std::string());
            const std::string to_node   = to.value("node", std::string());
            const std::string to_port   = to.value("port", std::string());
            if (from_node.empty() || to_node.empty()) {
                continue;
            }

            std::string       edge_error;
            const std::string new_edge_id =
                parsed.addEdge(from_node, from_port, to_node, to_port, &edge_error);
            if (new_edge_id.empty()) {
                log::warn("[工作流] 跳过无效连线 " + from_node + "." + from_port + " → " + to_node +
                          "." + to_port + "（" + edge_error + "）");
                continue;
            }

            const std::string wanted_id = edge_json.value("id", std::string());
            if (!wanted_id.empty() && wanted_id != new_edge_id &&
                parsed.findEdge(wanted_id) == nullptr) {
                if (Edge* edge = parsed.findEdge(new_edge_id)) {
                    edge->id = wanted_id;
                }
            }
        }
    }

    // ---- 视口 ----
    const auto viewport = json.find("viewport");
    if (viewport != json.end() && viewport->is_object()) {
        parsed.viewport_x    = static_cast<float>(sane_position(viewport->value("x", 0.0)));
        parsed.viewport_y    = static_cast<float>(sane_position(viewport->value("y", 0.0)));
        parsed.viewport_zoom = static_cast<float>(sane_zoom(viewport->value("zoom", 1.0)));
    }

    parsed.refreshSequences();
    graph = std::move(parsed);

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

// ------------------------------------------------------------------ 文件 IO ----
bool save_workflow(const Graph& graph, const std::filesystem::path& file, std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    std::error_code code;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), code);
        if (code) {
            return fail("无法创建目录 " + file.parent_path().string() + "（" + code.message() + "）");
        }
    }

    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return fail("无法写入文件 " + file.string());
    }
    stream << graph_to_json(graph).dump(2) << "\n";
    stream.close();
    if (!stream) {
        return fail("写入文件失败 " + file.string());
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool load_workflow(const std::filesystem::path& file, Graph& graph, std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return fail("无法读取文件 " + file.string());
    }
    const std::string raw((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();

    try {
        return graph_from_json(nlohmann::json::parse(raw), graph, error);
    }
    catch (const std::exception& ex) {
        return fail(std::string("JSON 解析失败: ") + ex.what());
    }
}

} // namespace aiwrite::engine
