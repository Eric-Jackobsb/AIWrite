#include "engine/graph.h"

#include "engine/node_registry.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace aiwrite::engine {
namespace {

std::string trim(const std::string& text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

} // namespace

// --------------------------------------------------------------- 枚举名称 ----
const char* portTypeName(PortType type)
{
    switch (type) {
    case PortType::Text:     return "text";
    case PortType::Image:    return "image";
    case PortType::Provider: return "provider";
    case PortType::Number:   return "number";
    case PortType::Any:      return "any";
    }
    return "any";
}

const char* paramTypeName(ParamType type)
{
    switch (type) {
    case ParamType::String:    return "string";
    case ParamType::Text:      return "text";
    case ParamType::Int:       return "int";
    case ParamType::Float:     return "float";
    case ParamType::Bool:      return "bool";
    case ParamType::Enum:      return "enum";
    case ParamType::File:      return "file";
    case ParamType::Directory: return "directory";
    case ParamType::Color:     return "color";
    }
    return "string";
}

const char* nodeStateName(NodeState state)
{
    switch (state) {
    case NodeState::Idle:    return "idle";
    case NodeState::Waiting: return "waiting";
    case NodeState::Running: return "running";
    case NodeState::Done:    return "done";
    case NodeState::Error:   return "error";
    case NodeState::Skipped: return "skipped";
    }
    return "idle";
}

// ------------------------------------------------------------------- Param ---
std::string Param::text() const
{
    if (value.is_null()) {
        return {};
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return value.dump();
}

double Param::number(double fallback) const
{
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        const std::string raw = value.get<std::string>();
        if (raw.empty()) {
            return fallback;
        }
        char* end = nullptr;
        const double parsed = std::strtod(raw.c_str(), &end);
        if (end != nullptr && *end == '\0') {
            return parsed;
        }
    }
    return fallback;
}

bool Param::flag(bool fallback) const
{
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number()) {
        return value.get<double>() != 0.0;
    }
    return fallback;
}

bool Param::is_empty() const
{
    if (value.is_null()) {
        return true;
    }
    if (value.is_string()) {
        return trim(value.get<std::string>()).empty();
    }
    if (value.is_array() || value.is_object()) {
        return value.empty();
    }
    return false;
}

// -------------------------------------------------------------------- Node ---
const Port* Node::findPort(const std::string& port_id, PortDirection dir) const
{
    const std::vector<Port>& ports = (dir == PortDirection::Input) ? inputs : outputs;
    for (const Port& port : ports) {
        if (port.id == port_id) {
            return &port;
        }
    }
    return nullptr;
}

Port* Node::findPort(const std::string& port_id, PortDirection dir)
{
    return const_cast<Port*>(static_cast<const Node*>(this)->findPort(port_id, dir));
}

const Param* Node::findParam(const std::string& param_id) const
{
    for (const Param& param : params) {
        if (param.id == param_id) {
            return &param;
        }
    }
    return nullptr;
}

Param* Node::findParam(const std::string& param_id)
{
    return const_cast<Param*>(static_cast<const Node*>(this)->findParam(param_id));
}

int Node::portIndex(const std::string& port_id, PortDirection dir) const
{
    const std::vector<Port>& ports = (dir == PortDirection::Input) ? inputs : outputs;
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (ports[i].id == port_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ------------------------------------------------------------------- id ------
int numericIdOf(const std::string& id)
{
    if (id.empty()) {
        return 0;
    }
    std::size_t index = 0;
    if (std::isalpha(static_cast<unsigned char>(id[0]))) {
        index = 1;
    }
    if (index >= id.size()) {
        return 0;
    }
    int value = 0;
    for (; index < id.size(); ++index) {
        const char ch = id[index];
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return 0;
        }
        value = value * 10 + (ch - '0');
    }
    return value;
}

std::string Graph::makeNodeId()
{
    return "n" + std::to_string(next_node_seq++);
}

std::string Graph::makeEdgeId()
{
    return "e" + std::to_string(next_edge_seq++);
}

void Graph::refreshSequences()
{
    int max_node = 0;
    for (const Node& node : nodes) {
        max_node = std::max(max_node, numericIdOf(node.id));
    }
    int max_edge = 0;
    for (const Edge& edge : edges) {
        max_edge = std::max(max_edge, numericIdOf(edge.id));
    }
    next_node_seq = max_node + 1;
    next_edge_seq = max_edge + 1;
}

void Graph::clear()
{
    nodes.clear();
    edges.clear();
    name          = "未命名工作流";
    description.clear();
    next_node_seq = 1;
    next_edge_seq = 1;
}

// ---------------------------------------------------------------- 查找 -------
Node* Graph::findNode(const std::string& node_id)
{
    return const_cast<Node*>(static_cast<const Graph*>(this)->findNode(node_id));
}

const Node* Graph::findNode(const std::string& node_id) const
{
    for (const Node& node : nodes) {
        if (node.id == node_id) {
            return &node;
        }
    }
    return nullptr;
}

Edge* Graph::findEdge(const std::string& edge_id)
{
    return const_cast<Edge*>(static_cast<const Graph*>(this)->findEdge(edge_id));
}

const Edge* Graph::findEdge(const std::string& edge_id) const
{
    for (const Edge& edge : edges) {
        if (edge.id == edge_id) {
            return &edge;
        }
    }
    return nullptr;
}

int Graph::nodeIndex(const std::string& node_id) const
{
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id == node_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const Edge* Graph::findEdgeIntoInput(const std::string& node_id, const std::string& port_id) const
{
    for (const Edge& edge : edges) {
        if (edge.to_node == node_id && edge.to_port == port_id) {
            return &edge;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------- 增删 -------
std::string Graph::addNode(const std::string& type, float x, float y, std::string* error)
{
    registerAllNodes(); // 幂等：保证首次使用时 8 个节点已注册

    const Definition* definition = NodeRegistry::instance().find(type);
    if (definition == nullptr) {
        if (error != nullptr) {
            *error = "未注册的节点类型: " + type;
        }
        return {};
    }

    Node node;
    node.id      = makeNodeId();
    node.type    = definition->type;
    node.title   = definition->title.empty() ? definition->display_name : definition->title;
    node.x       = x;
    node.y       = y;
    node.inputs  = definition->inputs;
    node.outputs = definition->outputs;
    node.params  = definition->params;
    for (Param& param : node.params) {
        param.value = param.default_value; // 实例取默认值
    }

    const std::string new_id = node.id;
    nodes.push_back(std::move(node));
    if (error != nullptr) {
        error->clear();
    }
    return new_id;
}

bool Graph::removeEdgesOfNode(const std::string& node_id)
{
    const std::size_t before = edges.size();
    edges.erase(std::remove_if(edges.begin(), edges.end(),
                               [&node_id](const Edge& edge) {
                                   return edge.from_node == node_id || edge.to_node == node_id;
                               }),
                edges.end());
    return edges.size() != before;
}

bool Graph::removeNode(const std::string& node_id)
{
    const int index = nodeIndex(node_id);
    if (index < 0) {
        return false;
    }
    removeEdgesOfNode(node_id);
    nodes.erase(nodes.begin() + index);
    return true;
}

std::string Graph::cloneNode(const std::string& node_id, float dx, float dy, std::string* error)
{
    const Node* source = findNode(node_id);
    if (source == nullptr) {
        if (error != nullptr) {
            *error = "待复制的节点不存在: " + node_id;
        }
        return {};
    }

    Node copy       = *source; // 深拷贝（含端口与参数值）
    copy.id         = makeNodeId();
    copy.title      = source->title + " 副本";
    copy.x          = source->x + dx;
    copy.y          = source->y + dy;
    copy.state      = NodeState::Idle;
    copy.error_message.clear();

    const std::string new_id = copy.id;
    nodes.push_back(std::move(copy));
    if (error != nullptr) {
        error->clear();
    }
    return new_id;
}

int Graph::copyParamsToSameType(const std::string& from_node_id, std::string* error)
{
    const Node* source = findNode(from_node_id);
    if (source == nullptr) {
        if (error != nullptr) {
            *error = "待应用参数的节点不存在: " + from_node_id;
        }
        return 0;
    }

    int updated = 0;
    for (Node& node : nodes) {
        if (node.id == source->id || node.type != source->type) {
            continue;
        }
        for (Param& target : node.params) {
            if (target.is_secret) {
                continue; // 密钥类参数不参与批量复制（避免误扩散）
            }
            const Param* origin = source->findParam(target.id);
            if (origin == nullptr || origin->is_secret) {
                continue;
            }
            target.value = origin->value; // 按值复制
        }
        ++updated;
    }

    if (error != nullptr) {
        error->clear();
    }
    return updated;
}

std::string Graph::addEdge(const std::string& from_node, const std::string& from_port,
                           const std::string& to_node, const std::string& to_port,
                           std::string* error)
{
    bool would_replace = false;
    std::string reason;
    if (!canConnect(from_node, from_port, to_node, to_port, &reason, &would_replace)) {
        if (error != nullptr) {
            *error = reason;
        }
        return {};
    }

    // 输入端口已有连线且非可变长端口 → 替换（设计 §6.2）
    if (would_replace) {
        const Node* target = findNode(to_node);
        const Port* port   = (target != nullptr) ? target->findPort(to_port, PortDirection::Input) : nullptr;
        if (port == nullptr || !port->is_variadic) {
            if (const Edge* old_edge = findEdgeIntoInput(to_node, to_port)) {
                removeEdge(old_edge->id);
            }
        }
    }

    Edge edge;
    edge.id        = makeEdgeId();
    edge.from_node = from_node;
    edge.from_port = from_port;
    edge.to_node   = to_node;
    edge.to_port   = to_port;

    const std::string new_id = edge.id;
    edges.push_back(std::move(edge));

    if (error != nullptr) {
        error->clear();
    }
    return new_id;
}

bool Graph::removeEdge(const std::string& edge_id)
{
    const auto it = std::find_if(edges.begin(), edges.end(),
                                 [&edge_id](const Edge& edge) { return edge.id == edge_id; });
    if (it == edges.end()) {
        return false;
    }
    edges.erase(it);
    return true;
}

bool Graph::isPortTypeCompatible(PortType from, PortType to)
{
    if (from == PortType::Any || to == PortType::Any) {
        return true; // any 双向兼容
    }
    return from == to;
}

bool Graph::canConnect(const std::string& from_node, const std::string& from_port,
                       const std::string& to_node, const std::string& to_port,
                       std::string* reason, bool* would_replace) const
{
    const auto fail = [reason](const std::string& text) {
        if (reason != nullptr) {
            *reason = text;
        }
        return false;
    };

    const Node* source = findNode(from_node);
    if (source == nullptr) {
        return fail("源节点不存在: " + from_node);
    }
    const Node* target = findNode(to_node);
    if (target == nullptr) {
        return fail("目标节点不存在: " + to_node);
    }

    const Port* output = source->findPort(from_port, PortDirection::Output);
    if (output == nullptr) {
        return fail("源端口不存在: " + from_port);
    }
    const Port* input = target->findPort(to_port, PortDirection::Input);
    if (input == nullptr) {
        return fail("目标端口不存在: " + to_port);
    }

    if (from_node == to_node) {
        return fail("不能连接到自身");
    }
    if (!isPortTypeCompatible(output->type, input->type)) {
        return fail(std::string("类型不兼容: ") + portTypeName(output->type) + " → " +
                    portTypeName(input->type));
    }

    for (const Edge& edge : edges) {
        if (edge.from_node == from_node && edge.from_port == from_port &&
            edge.to_node == to_node && edge.to_port == to_port) {
            return fail("已存在相同连线");
        }
    }

    const bool occupied = findEdgeIntoInput(to_node, to_port) != nullptr;
    if (would_replace != nullptr) {
        *would_replace = occupied && !input->is_variadic;
    }
    if (occupied && !input->is_variadic && reason != nullptr) {
        *reason = "输入端口已有连接，将被替换";
    }
    return true;
}

int Graph::inputConnectionCount(const std::string& node_id, const std::string& port_id) const
{
    int count = 0;
    for (const Edge& edge : edges) {
        if (edge.to_node == node_id && edge.to_port == port_id) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------- 校验 -------
bool Graph::validateParam(const Param& param, std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };
    const auto number_text = [](double value) {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    };

    if (param.is_required && param.is_empty()) {
        return fail("必填项不能为空");
    }
    if (param.is_empty()) {
        if (error != nullptr) {
            error->clear();
        }
        return true; // 非必填且为空 → 通过
    }

    switch (param.type) {
    case ParamType::Int:
    case ParamType::Float: {
        const double value = param.number();
        if (param.min_value.has_value() && value < *param.min_value) {
            return fail("不能小于 " + number_text(*param.min_value));
        }
        if (param.max_value.has_value() && value > *param.max_value) {
            return fail("不能大于 " + number_text(*param.max_value));
        }
        break;
    }
    case ParamType::Enum: {
        const std::string value = param.text();
        if (!param.enum_options.empty() &&
            std::find(param.enum_options.begin(), param.enum_options.end(), value) ==
                param.enum_options.end()) {
            return fail("取值不在枚举范围内: " + value);
        }
        break;
    }
    case ParamType::File: {
        std::error_code ec;
        const std::filesystem::path file_path = param.text();
        if (!std::filesystem::exists(file_path, ec) ||
            !std::filesystem::is_regular_file(file_path, ec)) {
            return fail("文件不存在: " + param.text());
        }
        break;
    }
    case ParamType::Directory: {
        std::error_code ec;
        const std::filesystem::path dir_path = param.text();
        if (!std::filesystem::exists(dir_path, ec) ||
            !std::filesystem::is_directory(dir_path, ec)) {
            return fail("目录不存在: " + param.text());
        }
        break;
    }
    default:
        break;
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool Graph::validateParams(const Node& node, std::vector<std::string>* errors) const
{
    bool ok = true;
    for (const Param& param : node.params) {
        if (!param_visible(node, param)) {
            continue; // 条件隐藏的参数不参与校验（如 web 模式下的 API Key / API 地址）
        }
        std::string error;
        if (!validateParam(param, &error)) {
            ok = false;
            if (errors != nullptr) {
                errors->push_back(param.display_name + ": " + error);
            }
        }
    }
    return ok;
}

bool Graph::topologicalOrder(std::vector<std::string>* order, std::string* error) const
{
    if (order != nullptr) {
        order->clear();
    }
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    // 入度表 + 下游邻接表：只统计"连接两个存在节点"的边
    std::unordered_map<std::string, int>                        in_degree;
    std::unordered_map<std::string, std::vector<std::string>>   downstream;
    in_degree.reserve(nodes.size());
    for (const Node& node : nodes) {
        in_degree.emplace(node.id, 0);
    }
    for (const Edge& edge : edges) {
        if (in_degree.find(edge.from_node) == in_degree.end() ||
            in_degree.find(edge.to_node) == in_degree.end()) {
            continue; // 悬空边：交给 validateWorkflow 报错
        }
        downstream[edge.from_node].push_back(edge.to_node);
        ++in_degree[edge.to_node];
    }

    // 零入度节点按插入序入队，保证结果稳定（可测、可复现）
    std::vector<std::string> queue;
    queue.reserve(nodes.size());
    for (const Node& node : nodes) {
        if (in_degree[node.id] == 0) {
            queue.push_back(node.id);
        }
    }

    std::vector<std::string> result;
    result.reserve(nodes.size());
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::string current = queue[head];
        result.push_back(current);
        for (const std::string& next : downstream[current]) {
            if (--in_degree[next] == 0) {
                queue.push_back(next);
            }
        }
    }

    if (result.size() != nodes.size()) {
        std::string cycle;
        for (const Node& node : nodes) {
            if (in_degree[node.id] > 0) {
                if (!cycle.empty()) {
                    cycle += "、";
                }
                cycle += node.id;
            }
        }
        return fail("工作流存在环，涉及节点: " + cycle + "（请删除环上的连线后重试）");
    }

    if (order != nullptr) {
        *order = std::move(result);
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool param_visible(const Node& node, const Param& param)
{
    if (param.visible_when_param.empty()) {
        return true; // 未设置条件 → 始终可见
    }
    const Param* guard = node.findParam(param.visible_when_param);
    if (guard == nullptr) {
        return true; // 条件参数不存在 → 不隐藏（宁可多显示也不要误隐藏）
    }
    return guard->text() == param.visible_when_value;
}

bool param_visible(const Node& node, const std::string& param_id)
{
    const Param* param = node.findParam(param_id);
    return param == nullptr ? true : param_visible(node, *param);
}

} // namespace aiwrite::engine
