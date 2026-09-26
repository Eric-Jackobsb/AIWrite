#include "engine/provider_resolve.h"

#include "utils/credential.h"

namespace aiwrite::engine {
namespace {

// 从节点参数里取值（不存在则保持原值）
std::string param_text(const Node& node, const std::string& param_id, const std::string& fallback)
{
    const Param* param = node.findParam(param_id);
    if (param == nullptr) {
        return fallback;
    }
    const std::string value = param->text();
    return value.empty() ? fallback : value;
}

} // namespace

bool uses_provider(const std::string& node_type)
{
    return node_type == "LLMGenerate" || node_type == "VLMGenerate";
}

EffectiveProvider resolve_effective_provider(const Graph& graph, const Node& node)
{
    EffectiveProvider result;
    if (!uses_provider(node.type)) {
        return result;
    }

    // ---- 2) 基础：节点自身参数（provider 输入未连接时即为生效值）----
    result.resolved     = true;
    result.source_node  = node.id;
    result.provider     = param_text(node, "provider", result.provider);
    result.mode         = param_text(node, "mode", result.mode);
    result.model        = param_text(node, "model", result.model);
    result.model_custom = param_text(node, "model_custom", std::string());

    // ---- 1) provider 输入连线优先（覆盖节点自身参数）----
    const Edge* edge = graph.findEdgeIntoInput(node.id, "provider");
    if (edge == nullptr) {
        return result;
    }
    const Node* source = graph.findNode(edge->from_node);
    if (source == nullptr) {
        return result;
    }
    result.from_edge    = true;
    result.source_node  = source->id;
    result.provider     = param_text(*source, "provider", result.provider);
    result.mode         = param_text(*source, "mode", result.mode);
    result.model        = param_text(*source, "model", result.model);
    result.model_custom = param_text(*source, "model_custom", std::string());
    // M5-02：自定义模型名非空 → 覆盖枚举值（界面与执行同一规则）
    if (!result.model_custom.empty()) {
        result.model = result.model_custom;
    }
    return result;
}

bool official_not_wired(const EffectiveProvider& provider)
{
    return provider.resolved && provider.mode == "official";
}

std::string unwired_reason(const Graph& graph, const Node& node)
{
    if (!uses_provider(node.type)) {
        return {};
    }
    const EffectiveProvider effective = resolve_effective_provider(graph, node);

    // M5-02：图片理解已接线（official / OpenAI 兼容多模态）；网页版没有图片入口 → 提示改 official
    if (node.type == "VLMGenerate" && effective.mode == "web") {
        return "图片理解暂不支持网页版：请把「提供商配置」的模式改为 official，"
               "并在「模型（自定义）」填写第三方/本地视觉模型名（如 glm-4v-flash）";
    }
    if (node.type != "LLMGenerate" && node.type != "VLMGenerate") {
        return {};
    }
    if (effective.mode != "official") {
        return {};
    }
    // PB-05 已接线官方 API：只有“缺 API Key”才会必定失败（参数 → 环境变量 DEEPSEEK_API_KEY）
    // Key 可能在 provider 输入的来源节点（「提供商配置」）上，其次本节点，最后环境变量
    const Param* key = nullptr;
    if (const Edge* provider_edge = graph.findEdgeIntoInput(node.id, "provider")) {
        if (const Node* source = graph.findNode(provider_edge->from_node)) {
            key = source->findParam("api_key");
        }
    }
    if (key == nullptr) {
        key = node.findParam("api_key");
    }
    const char*  env_key = std::getenv("DEEPSEEK_API_KEY");
    const bool   has_env = (env_key != nullptr && *env_key != '\0');
    if (key != nullptr && !key->is_empty()) {
        return {};
    }
    if (has_env) {
        return {};
    }
    // PB-06：凭据库里已有该 ref 也算“有 Key”（不打印内容）
    std::string ref;
    if (const Edge* provider_edge = graph.findEdgeIntoInput(node.id, "provider")) {
        if (const Node* source = graph.findNode(provider_edge->from_node)) {
            if (const Param* ref_param = source->findParam("api_key_ref")) {
                ref = ref_param->text();
            }
        }
    }
    if (!ref.empty()) {
        std::string load_error;
        if (!utils::load_credential(ref, &load_error).empty()) {
            return {};
        }
    }
    if (node.type == "VLMGenerate") {
        return "图片理解缺少 API Key（可在「提供商配置」填写并自动入库，或设置环境变量 "
               "DEEPSEEK_API_KEY）；模型名请在「提供商配置 → 模型（自定义）」填写视觉模型";
    }
    return "官方 API 缺少 API Key（可在「提供商配置」填写并自动入库，或设置环境变量 DEEPSEEK_API_KEY）";
}

} // namespace aiwrite::engine
