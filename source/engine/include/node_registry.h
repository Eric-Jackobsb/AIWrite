#pragma once

// ============================================================================
//  节点注册表（设计文档 §4.3 分类 / §4.4 九个节点 / §4.5 端口类型 / §9.2 实现）
//
//  * Definition 描述一个节点类型：分类、输入输出端口、参数（含默认值/范围/枚举）
//  * registerAllNodes() 注册 MVP 的 8 个节点；幂等，可重复调用
//  * Graph::addNode() 依据本表创建节点实例
// ============================================================================

#include <string>
#include <vector>

#include "engine/graph.h"

namespace aiwrite::engine {

// 设计 §4.3 节点分类
enum class NodeCategory { Input, Process, Config, Inference, Output };

const char* categoryName(NodeCategory category);        // "输入" / "处理" / ...
const char* categoryKey(NodeCategory category);         // "input" / "process" / ...（供序列化）

struct Definition {
    std::string       type;          // 如 "TextInput"（设计 §4.7 的 type 字段）
    std::string       display_name;  // 如 "文本输入"
    std::string       title;         // 新建节点的默认标题
    NodeCategory      category = NodeCategory::Input;
    std::vector<Port> inputs;
    std::vector<Port> outputs;
    std::vector<Param> params;
    std::string       description;
};

class NodeRegistry {
public:
    static NodeRegistry& instance();

    void registerNode(Definition definition);
    const Definition* find(const std::string& type) const;

    // 全部类型（按分类、再按注册顺序）
    std::vector<const Definition*> listTypes() const;
    std::vector<const Definition*> listByCategory(NodeCategory category) const;

    std::size_t size() const;
    void        clear();

private:
    std::vector<Definition> definitions_;
};

// 注册 MVP 的 8 个节点（幂等）
void registerAllNodes();

} // namespace aiwrite::engine
