#include "composite.h"

#include "log.h"
#include "node_registry.h"
#include "paths.h"
#include "validate.h"
#include "workflow_io.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <utility>

namespace aiwrite::engine {
namespace {

constexpr int kCompositeSchemaVersion = 1;

// 内部图里查找节点端口
const Port* find_internal_port(const CompositeSpec& spec, const std::string& node_id,
                               const std::string& port_id, PortDirection dir)
{
    const Node* node = spec.internal_graph.findNode(node_id);
    return (node != nullptr) ? node->findPort(port_id, dir) : nullptr;
}

// 内部图里查找节点参数
const Param* find_internal_param(const CompositeSpec& spec, const std::string& node_id,
                                 const std::string& param_id)
{
    const Node* node = spec.internal_graph.findNode(node_id);
    return (node != nullptr) ? node->findParam(param_id) : nullptr;
}

// 热重载连边有效性：两端节点存在、非自环、端口仍存在、类型兼容
bool edge_still_valid(const Graph& graph, const Edge& edge)
{
    const Node* from = graph.findNode(edge.from_node);
    const Node* to   = graph.findNode(edge.to_node);
    if (from == nullptr || to == nullptr) {
        return false;
    }
    if (from->id == to->id) {
        return false; // 自环（防御性移除）
    }
    const Port* output = from->findPort(edge.from_port, PortDirection::Output);
    const Port* input  = to->findPort(edge.to_port, PortDirection::Input);
    if (output == nullptr || input == nullptr) {
        return false; // 端口已不存在（暴露输入/输出数量或 id 变化）
    }
    return Graph::isPortTypeCompatible(output->type, input->type);
}

} // namespace

// ------------------------------------------------------------------ 校验 ----
bool validate_composite_spec(const CompositeSpec& spec, std::string* error,
                             bool allow_incomplete_draft)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    if (spec.id.empty()) {
        return fail("上级节点 id 不能为空");
    }
    if (spec.name.empty()) {
        return fail("上级节点名称不能为空");
    }

    // ---- 内部图：合法（含无环）、非空、无嵌套 ----
    // 草稿（新建空白规格）允许空内部图：用户先在编辑器里添加内部节点，保存时才严格校验。
    if (spec.internal_graph.nodes.empty() && !allow_incomplete_draft) {
        return fail("上级节点内部图为空：请先添加内部节点");
    }
    for (const Node& node : spec.internal_graph.nodes) {
        if (!node.composite_id.empty()) {
            return fail("上级节点不支持嵌套：内部图不能包含上级节点（" + node.id + "）");
        }
    }
    // 仅非空图才跑 validateWorkflow：空图没有节点/边/环可校验（且 validateWorkflow 会
    // 对空图报「工作流为空」，草稿保存需放行）。
    if (!spec.internal_graph.nodes.empty()) {
        ValidationMessages graph_errors;
        validateWorkflow(spec.internal_graph, &graph_errors);
        if (!graph_errors.empty()) {
            return fail("内部图校验失败：" + graph_errors.front());
        }
    }

    // ---- 输出：强制单输出（设计 CN-D7）----
    // 草稿允许未标记输出（0 个）；编辑器「保存」仍强制 1 个输出。
    if (spec.outputs.size() != 1 && !allow_incomplete_draft) {
        return fail("上级节点必须标记 1 个输出端口（当前 " +
                    std::to_string(spec.outputs.size()) + " 个）");
    }

    // ---- 暴露端口引用：端口存在 + 方向正确 ----
    for (std::size_t i = 0; i < spec.inputs.size(); ++i) {
        const CompositePortRef& ref = spec.inputs[i];
        if (find_internal_port(spec, ref.internal_node, ref.internal_port, PortDirection::Input) ==
            nullptr) {
            return fail("暴露输入 " + std::to_string(i + 1) + " 引用的内部输入端口不存在：" +
                        ref.internal_node + "." + ref.internal_port);
        }
        // 标记为对外输入的端口不应再有内部入边（展开后会重复占用）
        if (spec.internal_graph.findEdgeIntoInput(ref.internal_node, ref.internal_port) != nullptr) {
            return fail("暴露输入「" + ref.display_name + "」对应的内部端口已有内部连线，" +
                        "请先删除该内部连线或更换标记端口");
        }
    }
    for (std::size_t i = 0; i < spec.outputs.size(); ++i) {
        const CompositePortRef& ref = spec.outputs[i];
        if (find_internal_port(spec, ref.internal_node, ref.internal_port, PortDirection::Output) ==
            nullptr) {
            return fail("暴露输出 " + std::to_string(i + 1) + " 引用的内部输出端口不存在：" +
                        ref.internal_node + "." + ref.internal_port);
        }
    }

    // ---- 暴露参数：参数存在 + 外部参数 id 唯一 ----
    std::set<std::string> seen_param_ids;
    for (std::size_t i = 0; i < spec.exposed_params.size(); ++i) {
        const CompositeParamRef& ref = spec.exposed_params[i];
        if (find_internal_param(spec, ref.internal_node, ref.param_id) == nullptr) {
            return fail("暴露参数「" + ref.param_id + "」对应的内部参数不存在：" + ref.internal_node +
                        "." + ref.param_id);
        }
        if (!seen_param_ids.insert(ref.param_id).second) {
            return fail("暴露参数 id 重复：" + ref.param_id);
        }
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

// ------------------------------------------------------------ 序列化 ----
nlohmann::json compositeToJson(const CompositeSpec& spec)
{
    nlohmann::json document;
    document["schema_version"] = kCompositeSchemaVersion;
    document["id"]             = spec.id;
    document["name"]           = spec.name;
    document["description"]    = spec.description;

    nlohmann::json inputs = nlohmann::json::array();
    for (const CompositePortRef& ref : spec.inputs) {
        inputs.push_back({{"internal_node", ref.internal_node},
                          {"internal_port", ref.internal_port},
                          {"display_name", ref.display_name},
                          {"type", portTypeName(ref.type)},
                          {"is_optional", ref.is_optional}});
    }
    document["inputs"] = std::move(inputs);

    nlohmann::json outputs = nlohmann::json::array();
    for (const CompositePortRef& ref : spec.outputs) {
        outputs.push_back({{"internal_node", ref.internal_node},
                           {"internal_port", ref.internal_port},
                           {"display_name", ref.display_name},
                           {"type", portTypeName(ref.type)}});
    }
    document["outputs"] = std::move(outputs);

    nlohmann::json params = nlohmann::json::array();
    for (const CompositeParamRef& ref : spec.exposed_params) {
        params.push_back({{"internal_node", ref.internal_node},
                          {"param_id", ref.param_id},
                          {"display_name", ref.display_name}});
    }
    document["exposed_params"] = std::move(params);

    document["internal_graph"] = graphToJson(spec.internal_graph);
    return document;
}

bool compositeFromJson(const nlohmann::json& json, CompositeSpec& spec, std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    if (!json.is_object()) {
        return fail("上级节点 JSON 不是对象");
    }
    CompositeSpec parsed;
    parsed.id          = json.value("id", std::string());
    parsed.name        = json.value("name", std::string());
    parsed.description = json.value("description", std::string());

    const auto read_type = [](const std::string& name) {
        if (name == "text") {
            return PortType::Text;
        }
        if (name == "image") {
            return PortType::Image;
        }
        if (name == "provider") {
            return PortType::Provider;
        }
        if (name == "number") {
            return PortType::Number;
        }
        return PortType::Any;
    };

    const auto inputs = json.find("inputs");
    if (inputs != json.end() && inputs->is_array()) {
        for (const auto& item : *inputs) {
            if (!item.is_object()) {
                continue;
            }
            CompositePortRef ref;
            ref.internal_node = item.value("internal_node", std::string());
            ref.internal_port = item.value("internal_port", std::string());
            ref.display_name  = item.value("display_name", std::string());
            ref.type          = read_type(item.value("type", std::string()));
            ref.is_optional   = item.value("is_optional", true);
            parsed.inputs.push_back(std::move(ref));
        }
    }
    const auto outputs = json.find("outputs");
    if (outputs != json.end() && outputs->is_array()) {
        for (const auto& item : *outputs) {
            if (!item.is_object()) {
                continue;
            }
            CompositePortRef ref;
            ref.internal_node = item.value("internal_node", std::string());
            ref.internal_port = item.value("internal_port", std::string());
            ref.display_name  = item.value("display_name", std::string());
            ref.type          = read_type(item.value("type", std::string()));
            parsed.outputs.push_back(std::move(ref));
        }
    }
    const auto params = json.find("exposed_params");
    if (params != json.end() && params->is_array()) {
        for (const auto& item : *params) {
            if (!item.is_object()) {
                continue;
            }
            CompositeParamRef ref;
            ref.internal_node = item.value("internal_node", std::string());
            ref.param_id      = item.value("param_id", std::string());
            ref.display_name  = item.value("display_name", std::string());
            parsed.exposed_params.push_back(std::move(ref));
        }
    }

    const auto internal = json.find("internal_graph");
    if (internal == json.end() || !internal->is_object()) {
        return fail("上级节点缺少 internal_graph");
    }
    std::string graph_error;
    if (!graphFromJson(*internal, parsed.internal_graph, &graph_error)) {
        return fail("internal_graph 加载失败：" + graph_error);
    }

    spec = std::move(parsed);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

// ---------------------------------------------------- 节点实例构建 ----
bool buildCompositeNode(Node* node, const CompositeSpec& spec, std::string* error)
{
    if (node == nullptr) {
        return false;
    }
    // 实例化只做**结构**守卫（id/name 非空），不做运行态严格校验：
    //  * 端口/参数按规格尽力构建：内部引用缺失时端口回退 ref.type、参数直接跳过
    //  * 内部图「可运行」与否由运行前校验在**展开后**的图上拦截（executor start →
    //    expand_composites → validateBeforeRun）；添加实例时提前拒绝会让「草稿 / 待完善」
    //    规格永远无法拖入画布 —— 与下方「暴露输入恒可选：是否必接由内部端口决定
    //   （展开后校验）」同一口径（2026-10-18 修复）。
    if (spec.id.empty() || spec.name.empty()) {
        if (error != nullptr) {
            *error = "上级节点规格缺少 id 或 name：无法实例化";
        }
        return false;
    }

    node->type         = "Composite";
    node->composite_id = spec.id;
    node->title        = spec.name;

    node->inputs.clear();
    for (std::size_t i = 0; i < spec.inputs.size(); ++i) {
        const CompositePortRef& ref = spec.inputs[i];
        const Port* internal_port =
            find_internal_port(spec, ref.internal_node, ref.internal_port, PortDirection::Input);
        Port port;
        port.id           = "in" + std::to_string(i + 1);
        port.name         = ref.display_name.empty() ? port.id : ref.display_name;
        port.display_name = port.name;
        port.type         = (internal_port != nullptr) ? internal_port->type : ref.type;
        port.direction    = PortDirection::Input;
        port.is_optional  = true; // 暴露输入恒可选：是否必接由内部端口决定（展开后校验）
        node->inputs.push_back(std::move(port));
    }

    node->outputs.clear();
    for (std::size_t i = 0; i < spec.outputs.size(); ++i) {
        const CompositePortRef& ref = spec.outputs[i];
        const Port* internal_port =
            find_internal_port(spec, ref.internal_node, ref.internal_port, PortDirection::Output);
        Port port;
        port.id           = "out" + std::to_string(i + 1);
        port.name         = ref.display_name.empty() ? port.id : ref.display_name;
        port.display_name = port.name;
        port.type         = (internal_port != nullptr) ? internal_port->type : ref.type;
        port.direction    = PortDirection::Output;
        node->outputs.push_back(std::move(port));
    }

    node->params.clear();
    for (const CompositeParamRef& ref : spec.exposed_params) {
        const Param* internal_param = find_internal_param(spec, ref.internal_node, ref.param_id);
        if (internal_param == nullptr) {
            continue;
        }
        Param copy = *internal_param;
        copy.value = copy.default_value; // 新实例取默认值
        if (!ref.display_name.empty()) {
            copy.display_name = ref.display_name;
        }
        node->params.push_back(std::move(copy));
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}


// ------------------------------------------------------------ 热重载 ----
// 上级节点编辑器「保存」后：把主画布中已放置的同 id 实例按新规格**就地重建**
//  * 保留实例 id / 位置 / 自定义标题（== old_name 视为未自定义 → 跟随新规格名）/ 已填参数值
//  * 端口 id 稳定（in1…inN / out1…）：仍在的边保留，失效的边被移除
int refresh_composite_instances(Graph& graph, const std::string& composite_id,
                                const std::string& old_name, std::string* error)
{
    const CompositeSpec* spec = CompositeRegistry::instance().find(composite_id);
    if (spec == nullptr) {
        if (error != nullptr) {
            *error = "上级节点规格不存在: " + composite_id;
        }
        return 0;
    }

    std::vector<std::string> targets;
    for (const Node& node : graph.nodes) {
        if (node.composite_id == composite_id) {
            targets.push_back(node.id);
        }
    }
    if (targets.empty()) {
        return 0; // 画布没有该规格的实例：不改动图
    }

    for (const std::string& id : targets) {
        Node* node = graph.findNode(id);
        if (node == nullptr) {
            continue;
        }
        // 保留实例已填参数值（按 id；新增参数取默认值）
        std::map<std::string, nlohmann::json> old_values;
        for (const Param& p : node->params) {
            old_values[p.id] = p.value;
        }
        const std::string old_title = node->title;
        const float       ox        = node->x;
        const float       oy        = node->y;

        Node fresh;
        fresh.id = node->id; // 保留实例 id：画布选中 / 连边 / 撤销引用不失效
        if (!buildCompositeNode(&fresh, *spec, nullptr)) {
            continue; // 规格已通过严格校验，正常不会走到这里
        }
        fresh.x = ox;
        fresh.y = oy;
        // 标题：用户自定义过则保留；否则跟随规格新名称
        if (!old_title.empty() && old_title != old_name) {
            fresh.title = old_title;
        }
        for (Param& p : fresh.params) {
            const auto it = old_values.find(p.id);
            if (it != old_values.end()) {
                p.value = it->second;
            }
        }
        fresh.state        = NodeState::Idle;
        fresh.error_message.clear();
        *node = std::move(fresh);
    }

    // 清理连边：端点端口已不存在 / 类型不兼容 / 自环 / 重复（同 4 元组只留一条）
    std::set<std::string> seen;
    graph.edges.erase(
        std::remove_if(graph.edges.begin(), graph.edges.end(),
                       [&](const Edge& edge) {
                           if (!edge_still_valid(graph, edge)) {
                               return true;
                           }
                           const std::string key = edge.from_node + "|" + edge.from_port + "|" +
                                                   edge.to_node + "|" + edge.to_port;
                           return !seen.insert(key).second;
                       }),
        graph.edges.end());

    if (error != nullptr) {
        error->clear();
    }
    return static_cast<int>(targets.size());
}


} // namespace aiwrite::engine
