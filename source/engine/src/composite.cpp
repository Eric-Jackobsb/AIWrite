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


// ---------------------------------------------------- 编辑器试运行沙箱 ----
// 上级节点编辑器「执行连线」：内部图脱离父工作流也能直接跑
//  * 内部图本身没有数据源（暴露输入是“外部喂进来”的引用）⇒ 为每个暴露输入补一个
//    「测试源节点」并接向它指向的内部输入端口，让运行前校验通过、数据从源端起产出
//  * 源节点类型按端口类型映射：Text/Any→TextInput、Image→ImageInput、Provider→ProviderConfig
//  * 源节点 id 用 "__ce_src_<k>"（不进入画布；内部 n1.. 不与它冲突）
//  * 幂等无关：每次「执行连线」都新建沙箱，不改动规格内部图
const char* test_source_type_for(engine::PortType type)
{
    switch (type) {
    case engine::PortType::Image:    return "ImageInput";
    case engine::PortType::Provider: return "ProviderConfig";
    case engine::PortType::Text:     return "TextInput";
    case engine::PortType::Any:      return "TextInput";
    case engine::PortType::Number:   return "TextInput"; // 兜底（当前节点集没有 Number 输入端口）
    }
    return "TextInput";
}

bool buildCompositeTestGraph(const CompositeSpec& spec, Graph* out, std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    if (out == nullptr) {
        return fail("输出图指针为空");
    }
    if (spec.id.empty()) {
        return fail("上级节点规格 id 为空：无法构建试运行图");
    }

    *out = spec.internal_graph; // 深拷贝内部图（节点/边/参数原样保留）
    out->refreshSequences();    // 防御：确保后续 addNode/addEdge 的 id 不与内部 n1.. 冲突

    for (std::size_t k = 0; k < spec.inputs.size(); ++k) {
        const CompositePortRef& ref = spec.inputs[k];
        const Node*             target = out->findNode(ref.internal_node);
        if (target == nullptr) {
            continue; // 规格校验已保证存在；防御性跳过
        }
        const Port* port = target->findPort(ref.internal_port, PortDirection::Input);
        if (port == nullptr) {
            continue;
        }
        // 目标端口已有内部入边 ⇒ 不是真正的外部输入（规格校验已禁止；防御性跳过）
        if (out->findEdgeIntoInput(ref.internal_node, ref.internal_port) != nullptr) {
            continue;
        }

        const char* src_type = test_source_type_for(port->type);

        std::string       add_error;
        const std::string raw_id =
            out->addNode(src_type, target->x - 240.0f, target->y, &add_error);
        if (raw_id.empty()) {
            return fail("添加测试源节点失败: " + add_error);
        }
        Node* src = out->findNode(raw_id);
        if (src == nullptr) {
            return fail("测试源节点查找失败: " + raw_id);
        }
        src->id    = "__ce_src_" + std::to_string(k + 1);
        src->title = "测试输入 " + std::to_string(k + 1) + "（暴露输入 " +
                     (ref.display_name.empty() ? ref.internal_port : ref.display_name) + "）";
        // Number 端口兜底：把源输出端口类型改为目标类型，保证 canConnect 通过
        if (!src->outputs.empty() && src->outputs[0].type != port->type &&
            port->type != PortType::Any) {
            src->outputs[0].type = port->type;
        }
        const std::string out_port = src->outputs.empty() ? std::string() : src->outputs[0].id;
        if (out_port.empty()) {
            return fail("测试源节点没有输出端口: " + std::string(src_type));
        }
        const std::string edge_id =
            out->addEdge(src->id, out_port, ref.internal_node, ref.internal_port, &add_error);
        if (edge_id.empty()) {
            return fail("连接测试源失败: " + add_error);
        }
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

// ------------------------------------------------------------------ 打包 ----
// ARCH-E6：打包下沉 —— 主画布「打包为上级节点」的规格构建从 UI 层（ce_pack.cpp）迁入
// engine 单一函数；与 validate_composite_spec 同源（§4 验收判据 4）。
//  * 自动修正：暴露输入端口有内部入边（variadic 端口内外并存）时，该内部边不纳入
//    内部图 —— 外部输入在展开时接管该端口 ⇒ 生成产物必然通过严格校验（问题 2）。
bool buildCompositeSpecFromSelection(const Graph& source, const std::vector<std::string>& selected_nodes,
                                     const std::string& name, CompositeSpec* out, std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };
    if (out == nullptr) {
        return fail("输出规格指针为空");
    }
    if (selected_nodes.size() < 2) {
        return fail("打包需要至少选中 2 个节点");
    }
    const auto is_selected = [&selected_nodes](const std::string& node_id) {
        return std::find(selected_nodes.begin(), selected_nodes.end(), node_id) != selected_nodes.end();
    };

    CompositeSpec spec;
    spec.name = name.empty() ? "组合节点" : name;

    // ---- 1) internal_graph：选中节点（深拷贝，保持 id 与坐标）+ 选区内部连线 ----
    for (const std::string& nid : selected_nodes) {
        const Node* n = source.findNode(nid);
        if (n == nullptr) {
            continue;
        }
        if (!n->composite_id.empty()) {
            return fail("选区内不能包含上级节点（不支持嵌套）");
        }
        Node copy = *n;
        copy.state         = NodeState::Idle;
        copy.error_message.clear();
        spec.internal_graph.nodes.push_back(std::move(copy));
    }

    // ---- 2) 暴露输入：选区节点的输入端口，有来自选区外的入边 ----
    //  （variadic 端口可与内部入边并存 ⇒ 生成阶段落实「暴露输入不得有内部入边」：
    //    指向暴露输入端口的内边不纳入内部图 = 打包-校验同源）
    std::vector<CompositePortRef> inputs;
    std::vector<std::pair<std::string, std::string>> exposed_in_ports; // (internal_node, internal_port)
    for (const Node& n : spec.internal_graph.nodes) {
        for (const Port& p : n.inputs) {
            bool exposed = false;
            for (const Edge& e : source.edges) {
                if (e.to_node == n.id && e.to_port == p.id && !is_selected(e.from_node)) {
                    exposed = true;
                    break;
                }
            }
            if (!exposed) {
                continue;
            }
            CompositePortRef ref;
            ref.internal_node = n.id;
            ref.internal_port = p.id;
            ref.display_name  = p.display_name;
            ref.type          = p.type;
            inputs.push_back(std::move(ref));
            exposed_in_ports.emplace_back(n.id, p.id);
        }
    }

    for (const Edge& edge : source.edges) {
        if (!is_selected(edge.from_node) || !is_selected(edge.to_node)) {
            continue; // 只有两端都在选区内的边才进内部图
        }
        // 暴露输入端口的内部入边不纳入（外部输入在展开时接管该端口；问题 2 自动修正）
        const bool targets_exposed_input = std::any_of(
            exposed_in_ports.begin(), exposed_in_ports.end(),
            [&edge](const std::pair<std::string, std::string>& kv) {
                return kv.first == edge.to_node && kv.second == edge.to_port;
            });
        if (targets_exposed_input) {
            continue;
        }
        spec.internal_graph.edges.push_back(edge);
    }
    spec.internal_graph.refreshSequences();
    spec.inputs = std::move(inputs);

    // ---- 3) 暴露输出：边界输出端口（唯一；编辑器可再调整）----
    {
        std::vector<CompositePortRef> boundary;
        for (const Node& n : spec.internal_graph.nodes) {
            for (const Port& p : n.outputs) {
                for (const Edge& e : source.edges) {
                    if (e.from_node == n.id && e.from_port == p.id && !is_selected(e.to_node)) {
                        CompositePortRef ref;
                        ref.internal_node = n.id;
                        ref.internal_port = p.id;
                        ref.display_name  = p.display_name;
                        ref.type          = p.type;
                        boundary.push_back(std::move(ref));
                        break;
                    }
                }
            }
        }
        if (boundary.empty()) {
            // 无边界输出：退而取选区第一个输出端口（若选区整体是端到端内部流程）
            for (const Node& n : spec.internal_graph.nodes) {
                if (n.outputs.empty()) {
                    continue;
                }
                const Port& p = n.outputs.front();
                CompositePortRef ref;
                ref.internal_node = n.id;
                ref.internal_port = p.id;
                ref.display_name  = p.display_name;
                ref.type          = p.type;
                boundary.push_back(std::move(ref));
                break;
            }
        }
        if (boundary.empty()) {
            return fail("选区内没有任何可对外输出的端口（至少需要一个输出端口）");
        }
        spec.outputs.push_back(std::move(boundary.front()));
    }

    // ---- 4) 暴露参数：选中节点全部非密钥参数（外部参数 id = 内部参数 id）----
    for (const Node& n : spec.internal_graph.nodes) {
        for (const Param& p : n.params) {
            if (p.is_secret) {
                continue; // 密钥（如 API Key）不暴露
            }
            const bool dup = std::any_of(spec.exposed_params.begin(), spec.exposed_params.end(),
                                         [&p](const CompositeParamRef& r) {
                                             return r.param_id == p.id;
                                         });
            if (dup) {
                continue; // 外部参数 id 唯一：重复的只暴露首个
            }
            CompositeParamRef ref;
            ref.internal_node = n.id;
            ref.param_id      = p.id;
            ref.display_name  = p.display_name;
            spec.exposed_params.push_back(std::move(ref));
        }
    }

    *out = std::move(spec);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}



// ------------------------------------------------------------ 展开执行 ----
// CN-实施决策：在执行器工作副本上**就地展开**（见设计文档 §7.2「展开执行」）
//  * 展开发生在 start/startFrom 之内 ⇒ 同步/异步两条路径天然共用
//  * 展开后图中不再有 Composite 节点 ⇒ 无需注册 Composite 执行器
//  * 运行前校验在展开后的图上执行 ⇒ 内部必填端口未接会被正确拦截
bool expand_composites(Graph& graph,
                       std::unordered_map<std::string, std::vector<std::string>>* replaced_out,
                       std::string* error)
{
    CompositeRegistry& registry = CompositeRegistry::instance();
    // 节点快照副本：展开过程会增删 graph.nodes，不能边遍历边改
    const std::vector<Node> original_nodes = graph.nodes;

    for (const Node& composite : original_nodes) {
        if (composite.composite_id.empty()) {
            continue;
        }
        const CompositeSpec* spec = registry.find(composite.composite_id);
        if (spec == nullptr) {
            if (error != nullptr) {
                *error = "[上级节点] 规格不存在: " + composite.composite_id + "（文件 " +
                         registry.root().string() + "\\" + composite.composite_id +
                         ".json 缺失或被移除）";
            }
            return false;
        }

        // ---- 内部节点平移：深拷贝 + 前缀 id（<composite节点id>__<内部节点id>）----
        std::vector<std::string> internal_ids;
        internal_ids.reserve(spec->internal_graph.nodes.size());
        for (const Node& internal : spec->internal_graph.nodes) {
            Node copy = internal;
            copy.id  = composite.id + "__" + internal.id;
            copy.x += 16.0f; // 轻微偏移：仅调试视觉，不影响执行
            copy.y += 16.0f;
            copy.state = NodeState::Idle;
            copy.error_message.clear();
            graph.nodes.push_back(std::move(copy));
            internal_ids.push_back(composite.id + "__" + internal.id);
        }

        // ---- 暴露参数覆盖内部参数值（外部实例值优先）----
        for (const Param& external_param : composite.params) {
            for (const CompositeParamRef& ref : spec->exposed_params) {
                if (ref.param_id != external_param.id) {
                    continue;
                }
                Node*  target = graph.findNode(composite.id + "__" + ref.internal_node);
                Param* target_param = (target != nullptr) ? target->findParam(external_param.id)
                                                          : nullptr;
                if (target_param != nullptr) {
                    target_param->value = external_param.value;
                }
                break;
            }
        }

        // ---- 内部边平移 ----
        for (const Edge& edge : spec->internal_graph.edges) {
            Edge copy       = edge;
            copy.id         = composite.id + "__" + edge.id;
            copy.from_node  = composite.id + "__" + edge.from_node;
            copy.to_node    = composite.id + "__" + edge.to_node;
            copy.is_valid   = true;
            graph.edges.push_back(std::move(copy));
        }

        // ---- 外部输入边 → 内部标记输入端口 ----
        // 外部输入端口 id = in1…inN，按 spec.inputs 顺序一一对应
        for (std::size_t k = 0; k < spec->inputs.size(); ++k) {
            const std::string       ext_port = "in" + std::to_string(k + 1);
            const CompositePortRef& ref      = spec->inputs[k];
            const std::string       int_node = composite.id + "__" + ref.internal_node;
            for (Edge& edge : graph.edges) {
                if (edge.to_node == composite.id && edge.to_port == ext_port) {
                    edge.to_node = int_node;
                    edge.to_port = ref.internal_port;
                }
            }
        }

        // ---- 外部输出边 ← 内部标记输出端口 ----
        for (std::size_t k = 0; k < spec->outputs.size(); ++k) {
            const std::string       ext_port = "out" + std::to_string(k + 1);
            const CompositePortRef& ref      = spec->outputs[k];
            const std::string       int_node = composite.id + "__" + ref.internal_node;
            for (Edge& edge : graph.edges) {
                if (edge.from_node == composite.id && edge.from_port == ext_port) {
                    edge.from_node = int_node;
                    edge.from_port = ref.internal_port;
                }
            }
        }

        // ---- 移除 Composite 占位节点（残余连线已全部重接/内部化）----
        graph.removeNode(composite.id);

        if (replaced_out != nullptr) {
            (*replaced_out)[composite.id] = std::move(internal_ids);
        }
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}


// -------------------------------------------------------------- 注册表 ----
CompositeRegistry& CompositeRegistry::instance()
{
    static CompositeRegistry registry;
    return registry;
}

void CompositeRegistry::setRoot(std::filesystem::path root)
{
    builtin_root_     = root;
    user_root_        = std::move(root);
    roots_overridden_ = true; // 测试隔离：跳过旧目录迁移（不触碰用户真实数据）
    specs_.clear();
    source_.clear();
    reload();
}

void CompositeRegistry::setRoots(std::filesystem::path builtinRoot,
                                  std::filesystem::path userRoot)
{
    builtin_root_     = std::move(builtinRoot);
    user_root_        = std::move(userRoot);
    roots_overridden_ = true;
    specs_.clear();
    source_.clear();
    reload();
}

const std::filesystem::path& CompositeRegistry::builtinRoot() const
{
    return builtin_root_.empty() ? paths::builtinCompositesDir() : builtin_root_;
}

const std::filesystem::path& CompositeRegistry::userRoot() const
{
    return user_root_.empty() ? paths::userCompositesDir() : user_root_;
}

std::filesystem::path CompositeRegistry::root() const
{
    return userRoot(); // 兼容旧调用：用户根 = 写入口
}

void CompositeRegistry::reload()
{
    specs_.clear();
    source_.clear();
    loadFromDir(builtinRoot(), CompositeSource::Builtin);
    loadFromDir(userRoot(), CompositeSource::User); // 用户源后扫 ⇒ 覆盖内置源
    if (!roots_overridden_) {
        migrateLegacy(); // 旧 ~/.brain-ai/composites 首次并入用户源
    }
    loaded_ = true;
}

const CompositeSpec* CompositeRegistry::find(const std::string& id) const
{
    ensureLoaded();
    for (const CompositeSpec& spec : specs_) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

std::vector<const CompositeSpec*> CompositeRegistry::list() const
{
    ensureLoaded();
    std::vector<const CompositeSpec*> result;
    result.reserve(specs_.size());
    for (const CompositeSpec& spec : specs_) {
        result.push_back(&spec);
    }
    std::sort(result.begin(), result.end(),
              [](const CompositeSpec* a, const CompositeSpec* b) { return a->name < b->name; });
    return result;
}

void CompositeRegistry::ensureLoaded() const
{
    if (!loaded_) {
        // 场景：workflow 加载早于 UI 首帧 → 注册表在第一次访问时扫描目录
        const_cast<CompositeRegistry*>(this)->reload();
    }
}

void CompositeRegistry::loadFromDir(const std::filesystem::path& dir, CompositeSource source)
{
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return; // 目录不存在 = 空注册表
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream file(entry.path());
        if (!file.is_open()) {
            log::warn("[上级节点] 无法读取文件: " + entry.path().string());
            continue;
        }
        nlohmann::json document;
        try {
            file >> document;
        }
        catch (const nlohmann::json::parse_error&) {
            log::warn("[上级节点] 跳过损坏文件: " + entry.path().string());
            continue;
        }
        CompositeSpec spec;
        std::string   parse_error;
        if (!compositeFromJson(document, spec, &parse_error)) {
            log::warn("[上级节点] 跳过无效规格 " + entry.path().string() + "：" + parse_error);
            continue;
        }
        // PS-D9：后扫覆盖前扫 —— 用户源覆盖内置源（同 id 只保留最后加载的一份）
        specs_.erase(std::remove_if(specs_.begin(), specs_.end(),
                                    [&spec](const CompositeSpec& s) { return s.id == spec.id; }),
                     specs_.end());
        specs_.push_back(std::move(spec));
        source_[specs_.back().id] = source;
    }
    std::sort(specs_.begin(), specs_.end(),
              [](const CompositeSpec& a, const CompositeSpec& b) { return a.id < b.id; });
}

bool CompositeRegistry::save(const CompositeSpec& spec, std::string* error,
                             bool allow_incomplete_draft)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    std::string validate_error;
    if (!validate_composite_spec(spec, &validate_error, allow_incomplete_draft)) {
        return fail(validate_error);
    }

    // PS-D9：内置源只读 —— 直接覆盖内置 id 被拒（须先「复制到我的节点库」）
    if (sourceOf(spec.id) == CompositeSource::Builtin) {
        return fail("内置官方预设为只读：请先「复制到我的节点库」再编辑");
    }

    const std::filesystem::path dir = userRoot();
    std::error_code             ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return fail("无法创建上级节点目录: " + dir.string());
    }

    const std::filesystem::path file = dir / (spec.id + ".json");
    std::ofstream               out(file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return fail("无法写入文件: " + file.string());
    }
    out << compositeToJson(spec).dump(4);
    out.close();
    if (!out.good()) {
        return fail("写入文件失败: " + file.string());
    }

    // 写盘成功 → 更新内存副本（同 id 覆盖；来源 = 用户）
    auto it = std::find_if(specs_.begin(), specs_.end(),
                           [&spec](const CompositeSpec& s) { return s.id == spec.id; });
    if (it != specs_.end()) {
        *it = spec;
    }
    else {
        specs_.push_back(spec);
        std::sort(specs_.begin(), specs_.end(),
                  [](const CompositeSpec& a, const CompositeSpec& b) { return a.id < b.id; });
    }
    source_[spec.id] = CompositeSource::User;

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool CompositeRegistry::remove(const std::string& id, std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    const CompositeSource source = sourceOf(id);
    if (source == CompositeSource::None) {
        if (error != nullptr) {
            error->clear();
        }
        return true; // 幂等：不存在视为删除成功
    }
    if (source == CompositeSource::Builtin) {
        return fail("内置官方预设为只读：不能删除");
    }

    const std::filesystem::path file = userRoot() / (id + ".json");
    std::error_code             ec;
    if (std::filesystem::exists(file, ec)) {
        if (!std::filesystem::remove(file, ec) || ec) {
            return fail("删除文件失败: " + file.string());
        }
    }
    // 重新扫描双源：用户副本删除后，若内置仍有同 id ⇒ 来源回落 Builtin（PS-D9 §2.2）
    reload();

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

CompositeSource CompositeRegistry::sourceOf(const std::string& id) const
{
    ensureLoaded();
    const auto it = source_.find(id);
    return (it != source_.end()) ? it->second : CompositeSource::None;
}

bool CompositeRegistry::copyToUser(const std::string& id, std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    const CompositeSpec* spec = find(id);
    if (spec == nullptr) {
        return fail("找不到上级节点规格: " + id);
    }
    if (sourceOf(id) == CompositeSource::User) {
        if (error != nullptr) {
            error->clear();
        }
        return true; // 已是用户源，无需复制
    }

    // 写入用户源（同 id 覆盖官方资产是有意为之：用户接管后来源切 User、可编辑）
    const std::filesystem::path dir = userRoot();
    std::error_code             ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return fail("无法创建上级节点目录: " + dir.string());
    }
    const std::filesystem::path file = dir / (id + ".json");
    std::ofstream               out(file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return fail("无法写入文件: " + file.string());
    }
    out << compositeToJson(*spec).dump(4);
    out.close();
    if (!out.good()) {
        return fail("写入文件失败: " + file.string());
    }

    // 内存：同 id 覆盖为当前规格内容，来源切 User
    auto it = std::find_if(specs_.begin(), specs_.end(),
                           [&id](const CompositeSpec& s) { return s.id == id; });
    if (it != specs_.end()) {
        *it = *spec;
    }
    else {
        specs_.push_back(*spec);
        std::sort(specs_.begin(), specs_.end(),
                  [](const CompositeSpec& a, const CompositeSpec& b) { return a.id < b.id; });
    }
    source_[id] = CompositeSource::User;

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

// PS-D9：旧目录迁移 —— 首次 reload 把 ~/.brain-ai/composites/ 遗留文件并入用户源
//（复制后删除原文件，用户节点不丢失）；进程内只尝试一次，成功后旧目录清空 ⇒ 幂等。
//  * 仅在默认用户根（未 set_root/set_roots 覆盖）时执行，避免测试误触碰用户真实数据。
void CompositeRegistry::migrateLegacy()
{
    static bool attempted = false;
    if (attempted) {
        return;
    }
    attempted = true;

    const std::filesystem::path legacy = paths::compositesDir();
    std::error_code             ec;
    if (!std::filesystem::is_directory(legacy, ec)) {
        return;
    }
    int migrated = 0;
    for (const auto& entry : std::filesystem::directory_iterator(legacy, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
            continue;
        }
        nlohmann::json document;
        try {
            std::ifstream in(entry.path());
            if (!in.is_open()) {
                continue;
            }
            in >> document;
        }
        catch (const nlohmann::json::parse_error&) {
            log::warn("[上级节点] 旧目录迁移：跳过无法解析的文件 " + entry.path().string());
            continue;
        }
        CompositeSpec spec;
        std::string   parse_error;
        if (!compositeFromJson(document, spec, &parse_error)) {
            log::warn("[上级节点] 旧目录迁移：跳过无效规格 " + entry.path().string() + "：" +
                      parse_error);
            continue;
        }
        // 复制到用户源（同 id 覆盖语义）
        const std::filesystem::path dst = userRoot() / (spec.id + ".json");
        std::filesystem::create_directories(dst.parent_path(), ec);
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            log::error("[上级节点] 旧目录迁移：无法写入 " + dst.string());
            continue;
        }
        out << compositeToJson(spec).dump(4);
        out.close();
        if (!out.good()) {
            log::error("[上级节点] 旧目录迁移：写入失败 " + dst.string());
            continue;
        }
        // 写入成功 → 删除原文件；内存并入（来源 = User，覆盖内置）
        const std::string id = spec.id;
        std::error_code   rm_ec;
        std::filesystem::remove(entry.path(), rm_ec);
        specs_.erase(std::remove_if(specs_.begin(), specs_.end(),
                                    [&id](const CompositeSpec& s) { return s.id == id; }),
                     specs_.end());
        specs_.push_back(std::move(spec));
        source_[id] = CompositeSource::User;
        ++migrated;
    }
    std::sort(specs_.begin(), specs_.end(),
              [](const CompositeSpec& a, const CompositeSpec& b) { return a.id < b.id; });
    if (migrated > 0) {
        log::info("[上级节点] 旧目录迁移完成：已并入用户源 " + std::to_string(migrated) +
                  " 个规格（" + legacy.string() + "）");
    }
}

std::string CompositeRegistry::makeId() const
{
    ensureLoaded();
    std::set<int> used;
    for (const CompositeSpec& spec : specs_) {
        if (spec.id.rfind("c", 0) == 0) {
            try {
                used.insert(std::stoi(spec.id.substr(1)));
            }
            catch (...) {
            }
        }
    }
    int candidate = 1;
    while (used.count(candidate) > 0) {
        ++candidate;
    }
    return "c" + std::to_string(candidate);
}

} // namespace aiwrite::engine

