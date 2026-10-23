#include "node_registry.h"

#include "provider_spec.h" // M_patchB L1：提供商列表 / 默认值来自配置表
#include "config.h"        // M_8nodes_extension L1-a：retry_enabled 默认值跟随全局 error.retry_enabled

#include <algorithm>
#include <utility>

namespace aiwrite::engine {
// ---------------------------------------------------------------- 分类名称 ----
const char* categoryName(NodeCategory category)
{
    switch (category) {
    case NodeCategory::Input:     return "输入";
    case NodeCategory::Process:   return "处理";
    case NodeCategory::Config:    return "配置";
    case NodeCategory::Inference: return "推理";
    case NodeCategory::Output:    return "输出";
    case NodeCategory::Auxiliary: return "辅助功能"; // M_node_expansion L2
    }
    return "输入";
}

const char* categoryKey(NodeCategory category)
{
    switch (category) {
    case NodeCategory::Input:     return "input";
    case NodeCategory::Process:   return "process";
    case NodeCategory::Config:    return "config";
    case NodeCategory::Inference: return "inference";
    case NodeCategory::Output:    return "output";
    case NodeCategory::Auxiliary: return "auxiliary"; // M_node_expansion L2
    }
    return "input";
}

// ------------------------------------------------------------- NodeRegistry ----
NodeRegistry& NodeRegistry::instance()
{
    static NodeRegistry registry;
    return registry;
}

void NodeRegistry::registerNode(Definition definition)
{
    for (Definition& existing : definitions_) {
        if (existing.type == definition.type) {
            existing = std::move(definition); // 覆盖同名类型
            return;
        }
    }
    definitions_.push_back(std::move(definition));
}

const Definition* NodeRegistry::find(const std::string& type) const
{
    for (const Definition& definition : definitions_) {
        if (definition.type == type) {
            return &definition;
        }
    }
    return nullptr;
}

Definition* NodeRegistry::findMutable(const std::string& type)
{
    for (Definition& definition : definitions_) {
        if (definition.type == type) {
            return &definition;
        }
    }
    return nullptr;
}

std::vector<const Definition*> NodeRegistry::listTypes() const
{
    std::vector<const Definition*> result;
    result.reserve(definitions_.size());
    for (const Definition& definition : definitions_) {
        result.push_back(&definition);
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const Definition* a, const Definition* b) {
                         if (a->category != b->category) {
                             return static_cast<int>(a->category) < static_cast<int>(b->category);
                         }
                         return a->display_name < b->display_name;
                     });
    return result;
}

std::vector<const Definition*> NodeRegistry::listByCategory(NodeCategory category) const
{
    std::vector<const Definition*> result;
    for (const Definition& definition : definitions_) {
        if (definition.category == category) {
            result.push_back(&definition);
        }
    }
    return result;
}

std::size_t NodeRegistry::size() const
{
    return definitions_.size();
}

void NodeRegistry::clear()
{
    definitions_.clear();
}

// ============================================================================
//  PB2-24 要点②：`provider` 动态枚举（实现）
// ============================================================================

std::vector<std::string> providerOptionIds()
{
    const ai::ProviderSpecs& specs        = ai::providerSpecs();
    std::vector<std::string>  official_ids = specs.ids("official");
    std::vector<std::string>  web_ids      = specs.ids("web");
    std::vector<std::string>  ids;
    ids.reserve(official_ids.size() + web_ids.size());
    ids.insert(ids.end(), official_ids.begin(), official_ids.end());
    ids.insert(ids.end(), web_ids.begin(), web_ids.end());
    return ids;
}

namespace {

// 就地替换 Definition 里 `provider` 的枚举（默认值不在新枚举里 ⇒ 落到首项）
void apply_provider_options(Definition& definition, const std::vector<std::string>& ids)
{
    for (Param& param : definition.params) {
        if (param.id != "provider" || param.type != ParamType::Enum) {
            continue;
        }
        param.enum_options = ids;
        const std::string current =
            param.default_value.is_string() ? param.default_value.get<std::string>() : std::string();
        if (!ids.empty() && std::find(ids.begin(), ids.end(), current) == ids.end()) {
            param.default_value = ids.front();
        }
        return;
    }
}

// 就地替换节点实例里 `provider` 的枚举（**保留当前值**：可能是刚新建的站点 id）
void apply_provider_options(Node& node, const std::vector<std::string>& ids)
{
    for (Param& param : node.params) {
        if (param.id != "provider" || param.type != ParamType::Enum) {
            continue;
        }
        param.enum_options = ids;
        if (param.isEmpty() && !ids.empty()) {
            param.value = ids.front();
        }
        return;
    }
}

} // namespace

void refreshProviderOptions()
{
    const std::vector<std::string> ids = providerOptionIds();
    if (ids.empty()) {
        return; // 表不可用（极小概率）：保留原枚举，绝不把下拉清空
    }
    if (Definition* definition = NodeRegistry::instance().findMutable("ProviderConfig")) {
        apply_provider_options(*definition, ids);
    }
}

void refreshProviderOptions(Graph& graph)
{
    const std::vector<std::string> ids = providerOptionIds();
    if (ids.empty()) {
        return;
    }
    if (Definition* definition = NodeRegistry::instance().findMutable("ProviderConfig")) {
        apply_provider_options(*definition, ids);
    }
    for (Node& node : graph.nodes) {
        apply_provider_options(node, ids);
    }
}

void reloadProviderSpecsAndRefresh(Graph* graph)
{
    ai::reloadProviderSpecs();
    if (graph != nullptr) {
        refreshProviderOptions(*graph);
    }
    else {
        refreshProviderOptions();
    }
}

std::vector<std::string> providerParamOptions()
{
    const Definition* definition = NodeRegistry::instance().find("ProviderConfig");
    if (definition == nullptr) {
        return {};
    }
    for (const Param& param : definition->params) {
        if (param.id == "provider" && param.type == ParamType::Enum) {
            return param.enum_options;
        }
    }
    return {};
}

} // namespace aiwrite::engine
