#pragma once

// ============================================================================
//  工作流图数据模型（设计文档 §5.1 ~ §5.8）
//
//  * Node / Port / Param / Edge / Graph / Viewport
//  * 增删查、类型兼容校验、参数校验、稳定 id（"n1" / "e1"）
//  * 本层**不依赖 ImGui**，因此可被 api_probe --graph-selftest 直接自检，
//    也保证界面点击与自检走的是同一套逻辑代码。
// ============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace aiwrite::engine {

// ------------------------------------------------------------------- 枚举 ----
enum class PortType { Text, Image, Provider, Number, Any };       // 设计 §5.2
enum class PortDirection { Input, Output };
enum class ParamType { String, Text, Int, Float, Bool, Enum, File, Directory, Color }; // §5.3
enum class NodeState { Idle, Waiting, Running, Done, Error, Skipped };                 // §4.6

const char* portTypeName(PortType type);     // "text" / "image" / ...
const char* paramTypeName(ParamType type);   // "string" / "text" / ...
const char* nodeStateName(NodeState state);  // "idle" / "running" / ...

// ------------------------------------------------------------------- Port ----
struct Port {
    std::string   id;
    std::string   name;
    std::string   display_name;
    PortType      type      = PortType::Any;
    PortDirection direction = PortDirection::Input;
    bool          is_optional = false;
    bool          is_variadic = false;
    std::string   description;
};

// ------------------------------------------------------------------ Param ----
struct Param {
    std::string              id;
    std::string              name;
    std::string              display_name;
    ParamType                type = ParamType::String;
    nlohmann::json           value;
    nlohmann::json           default_value;
    std::optional<double>    min_value;
    std::optional<double>    max_value;
    std::optional<double>    step;
    std::vector<std::string> enum_options;
    std::string              description;
    bool                     is_required = false;
    bool                     is_secret   = false; // 如 api_key：界面用密码框

    void        reset_to_default() { value = default_value; }
    std::string text() const;                       // 取字符串值（非字符串返回 dump）
    double      number(double fallback = 0.0) const;
    bool        flag(bool fallback = false) const;
    bool        is_empty() const;
};

// ------------------------------------------------------------------- Node ----
struct Node {
    std::string        id;
    std::string        type;
    std::string        title;
    float              x = 0.0f;
    float              y = 0.0f;
    std::vector<Port>  inputs;
    std::vector<Port>  outputs;
    std::vector<Param> params;
    NodeState          state = NodeState::Idle;
    std::string        error_message;

    const Port*  findPort(const std::string& port_id, PortDirection dir) const;
    Port*        findPort(const std::string& port_id, PortDirection dir);
    const Param* findParam(const std::string& param_id) const;
    Param*       findParam(const std::string& param_id);
    int          portIndex(const std::string& port_id, PortDirection dir) const;
};

// ------------------------------------------------------------------- Edge ----
struct Edge {
    std::string id;
    std::string from_node;
    std::string from_port;
    std::string to_node;
    std::string to_port;
    bool        is_valid = true;
    std::string error_message;
};

// ------------------------------------------------------------------ Graph ----
class Graph {
public:
    std::string       id          = "g1";
    std::string       name        = "未命名工作流";
    std::string       description;                    // 工作流说明（写入 JSON 的 description）
    std::vector<Node> nodes;
    std::vector<Edge> edges;

    // 视口（设计 §5.6 Viewport）
    float viewport_x    = 0.0f;
    float viewport_y    = 0.0f;
    float viewport_zoom = 1.0f;

    // id 序列（随快照一起复制，保证撤销/重做后 id 不回退）
    int next_node_seq = 1;
    int next_edge_seq = 1;

    // ----------------------------------------------------------- 查找 -------
    Node*       findNode(const std::string& node_id);
    const Node* findNode(const std::string& node_id) const;
    Edge*       findEdge(const std::string& edge_id);
    const Edge* findEdge(const std::string& edge_id) const;
    int         nodeIndex(const std::string& node_id) const;

    const Edge* findEdgeIntoInput(const std::string& node_id, const std::string& port_id) const;
    int         inputConnectionCount(const std::string& node_id, const std::string& port_id) const;

    // ----------------------------------------------------------- 增删 -------
    // 说明：add/remove 会使既有 Node*/Edge* 失效（vector 重分配），
    //       因此创建接口统一返回 **稳定 id**，访问时用 findNode/findEdge 重新获取。
    // 依据注册表创建节点；成功返回新节点 id，失败返回空串并写 error
    std::string addNode(const std::string& type, float x, float y, std::string* error);
    bool        removeNode(const std::string& node_id);          // 连带删除相关边
    bool        removeEdgesOfNode(const std::string& node_id);
    // 复制节点（图内克隆，位置偏移 dx/dy；不含连线）；成功返回新节点 id
    std::string cloneNode(const std::string& node_id, float dx, float dy, std::string* error);

    // 建立连线；成功返回新连线 id
    std::string addEdge(const std::string& from_node, const std::string& from_port,
                        const std::string& to_node, const std::string& to_port,
                        std::string* error);
    bool        removeEdge(const std::string& edge_id);

    // ----------------------------------------------------------- 校验 -------
    // 设计 §4.5：同类型可连；any 双向兼容；其他不允许
    static bool isPortTypeCompatible(PortType from, PortType to);

    // would_replace 为 true 表示目标输入端口已有连线（调用方可先删除旧连线）
    bool canConnect(const std::string& from_node, const std::string& from_port,
                    const std::string& to_node, const std::string& to_port,
                    std::string* reason, bool* would_replace = nullptr) const;

    // 设计 §6.3：必填为空 / 超出范围 / 枚举非法 / 文件(夹)不存在
    static bool validateParam(const Param& param, std::string* error);
    bool        validateParams(const Node& node, std::vector<std::string>* errors) const;

    // ----------------------------------------------------------- id ---------
    std::string makeNodeId();   // "n1"、"n2" ...
    std::string makeEdgeId();   // "e1"、"e2" ...
    void        refreshSequences();

    void clear();               // 清空内容并重置 id 序列
};

// "n12" → 12、"e3" → 3；非法或空返回 0
int numericIdOf(const std::string& id);

} // namespace aiwrite::engine
