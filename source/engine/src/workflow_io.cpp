#include "workflow_io.h"

#include "composite.h"
#include "migration.h"     // M_patchAB §4（PC-04）：版本迁移（未来版本明确拒绝）
#include "node_registry.h"
#include "log.h"

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

// 去重专项：文件里记录的 id / 标题 / 参数 → 节点（普通节点与上级节点**同口径**）
//  * id 冲突（文件 id 已被占用）⇒ 保留自动生成的 id，避免引用错位
//  * 密钥参数不从文件恢复（只驻留内存）
void apply_recorded_overrides(Node& node, const nlohmann::json& node_json,
                              const std::string& generated_id, Graph& graph)
{
    const std::string wanted_id = node_json.value("id", std::string());
    if (!wanted_id.empty() && wanted_id != generated_id &&
        graph.findNode(wanted_id) == nullptr) {
        node.id = wanted_id;
    }
    const std::string wanted_title = node_json.value("title", std::string());
    if (!wanted_title.empty()) {
        node.title = wanted_title;
    }
    const auto params = node_json.find("params");
    if (params != node_json.end() && params->is_object()) {
        for (auto it = params->begin(); it != params->end(); ++it) {
            if (Param* param = node.findParam(it.key())) {
                if (param->is_secret) {
                    continue; // 密钥不从文件恢复（只驻留内存）
                }
                param->value = it.value();
            }
        }
    }
}

} // namespace

// ------------------------------------------------------------------ 序列化 ----
nlohmann::json graphToJson(const Graph& graph)
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
        // CN（Composition_node）：上级节点实例 —— 端口/参数来自规格，序列化只需规格引用
        if (!node.composite_id.empty()) {
            node_json["composite_id"] = node.composite_id;
        }

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
bool graphFromJson(const nlohmann::json& json, Graph& graph, std::string* error)
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

    // ---- M_patchAB §4（PC-04 / DBG-M6-02）：**版本迁移** ----
    //  * 旧版本 ⇒ 按迁移表处理（生产表当前为空 ⇒ 记「无需迁移」说明）
    //  * **未来版本 ⇒ 明确拒绝**（旧实现只 warn 后硬读，可能把新格式读坏）
    nlohmann::json document = json; // 迁移就地改的是**副本**，不动调用方的 JSON
    const WorkflowMigrationOutcome outcome = migrateWorkflow(&document);
    for (const std::string& note : outcome.notes) {
        log::info("[工作流迁移] " + note);
    }
    if (!outcome.ok) {
        return fail(outcome.error);
    }
    if (outcome.migrated) {
        log::info("[工作流迁移] 已从 " + outcome.from + " 迁移到 " + outcome.to + "（内存中）");
    }

    registerAllNodes();
    const NodeRegistry& registry = NodeRegistry::instance();

    Graph parsed;
    parsed.name        = document.value("name", std::string("未命名工作流"));
    parsed.description = document.value("description", std::string());

    // ---- 节点：端口与参数定义一律以注册表/规格为准，参数值取自文件 ----
    for (const auto& node_json : document["nodes"]) {
        if (!node_json.is_object()) {
            continue;
        }
        const std::string type = node_json.value("type", std::string());
        if (type.empty()) {
            continue;
        }

        const auto  position = node_json.value("position", nlohmann::json::object());
        const float x        = static_cast<float>(sane_position(position.value("x", 0.0)));
        const float y        = static_cast<float>(sane_position(position.value("y", 0.0)));

        // CN（Composition_node）：上级节点 —— 端口/参数以规格为准（注册表里没有 Composite 定义）
        const std::string composite_id = node_json.value("composite_id", std::string());
        if (!composite_id.empty()) {
            if (CompositeRegistry::instance().find(composite_id) == nullptr) {
                log::warn("[工作流] 跳过上级节点（规格缺失，请确认用户源 " +
                          CompositeRegistry::instance().userRoot().string() + "\\" + composite_id +
                          ".json 或内置源 " +
                          CompositeRegistry::instance().builtinRoot().string() + "\\" +
                          composite_id + ".json 存在）: " +
                          node_json.value("id", std::string()));
                continue;
            }
            std::string       composite_error;
            const std::string generated_id =
                parsed.addCompositeNode(composite_id, x, y, &composite_error);
            if (generated_id.empty()) {
                log::warn("[工作流] 跳过上级节点: " + composite_error);
                continue;
            }
            Node* node = parsed.findNode(generated_id);
            if (node == nullptr) {
                continue;
            }

            apply_recorded_overrides(*node, node_json, generated_id, parsed);
            continue;
        }

        if (registry.find(type) == nullptr) {
            log::warn("[工作流] 跳过未注册的节点类型: " + type);
            continue;
        }

        const std::string generated_id = parsed.addNode(type, x, y, nullptr);
        if (generated_id.empty()) {
            continue;
        }
        Node* node = parsed.findNode(generated_id);
        if (node == nullptr) {
            continue;
        }

        // 尽量保留文件中的 id（边的引用要对得上）；若与已有节点冲突则保留自动生成的 id
        apply_recorded_overrides(*node, node_json, generated_id, parsed);
    }

    // ---- 连线：统一走 addEdge（类型/端口/重复/自连校验），非法则跳过并告警 ----
    const auto edges = document.find("edges");
    if (edges != document.end() && edges->is_array()) {
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
    const auto viewport = document.find("viewport");
    if (viewport != document.end() && viewport->is_object()) {
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
bool saveWorkflow(const Graph& graph, const std::filesystem::path& file, std::string* error)
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
    stream << graphToJson(graph).dump(2) << "\n";
    stream.close();
    if (!stream) {
        return fail("写入文件失败 " + file.string());
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool loadWorkflow(const std::filesystem::path& file, Graph& graph, std::string* error)
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
        return graphFromJson(nlohmann::json::parse(raw), graph, error);
    }
    catch (const std::exception& ex) {
        return fail(std::string("JSON 解析失败: ") + ex.what());
    }
}

} // namespace aiwrite::engine
