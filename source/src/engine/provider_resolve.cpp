#include "engine/provider_resolve.h"

#include "utils/credential.h"

#include <cstdlib>

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

// 环境变量是否已设置（表内 env 名按序；空列表 = 仅 DEEPSEEK_API_KEY）
bool has_env_key(const std::vector<std::string>& env_names)
{
    if (env_names.empty()) {
        const char* value = std::getenv("DEEPSEEK_API_KEY");
        return value != nullptr && *value != '\0';
    }
    for (const std::string& name : env_names) {
        if (name.empty()) {
            continue;
        }
        const char* value = std::getenv(name.c_str());
        if (value != nullptr && *value != '\0') {
            return true;
        }
    }
    return false;
}

std::string join_names(const std::vector<std::string>& items)
{
    std::string text;
    for (const std::string& item : items) {
        if (!text.empty()) {
            text += "、";
        }
        text += item;
    }
    return text;
}

} // namespace

std::string EffectiveProvider::model_hint() const
{
    if (spec == nullptr || spec->models.empty()) {
        return {};
    }
    std::string text;
    for (const ai::ProviderModelSpec& item : spec->models) {
        if (!text.empty()) {
            text += "、";
        }
        text += item.id;
        if (item.vision) {
            text += "（视觉）";
        }
    }
    return text;
}

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

    const Node* config_node = &node;

    // ---- 1) provider 输入连线优先（覆盖节点自身参数）----
    if (const Edge* edge = graph.findEdgeIntoInput(node.id, "provider")) {
        if (const Node* source = graph.findNode(edge->from_node)) {
            result.from_edge    = true;
            result.source_node  = source->id;
            result.provider     = param_text(*source, "provider", result.provider);
            result.mode         = param_text(*source, "mode", result.mode);
            result.model        = param_text(*source, "model", result.model);
            result.model_custom = param_text(*source, "model_custom", std::string());
            config_node         = source;
        }
    }
    const std::string api_base_param = param_text(*config_node, "api_base", std::string());
    const std::string key_ref_param  = param_text(*config_node, "api_key_ref", std::string());

    // ---- 3) M_patchB L1（PB2-05）：查配置表，用表默认值补齐 / 决定 kind 与能力 ----
    //  优先级：工作流节点参数（非空）→ 配置表默认值 → 老行为（表里没有该 id）
    result.specs   = ai::provider_specs_snapshot();
    result.spec    = result.specs->find(result.provider);
    result.display = result.provider;
    if (result.spec != nullptr) {
        const ai::ProviderSpec& spec = *result.spec;
        result.kind         = spec.kind;
        result.display      = spec.display.empty() ? spec.id : spec.display;
        result.spec_origin  = spec.origin;
        result.api_base     = ai::resolve_api_base(spec, api_base_param);
        result.key_ref      = ai::resolve_key_ref(spec, key_ref_param);
        result.key_required = spec.auth_style != "none";
        const bool for_vision = node.type == "VLMGenerate";
        result.model = ai::resolve_model(spec, result.model, result.model_custom, for_vision);
        if (spec.kind == "web") {
            result.mode = "web"; // 站点条目只能是网页版（表优先于节点 mode 参数）
        }
    }
    else {
        // 表里没有该 id → 保持改造前的行为（节点参数为准；老工作流迁移落点 custom-official）
        result.api_base = api_base_param;
        result.key_ref  = key_ref_param;
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

    // ---- 表里找不到该条目：明确可操作（R3：老工作流迁移落点 / 拼写错误）----
    if (effective.resolved && effective.spec == nullptr) {
        return "配置表里没有提供商 \"" + effective.provider +
               "\"：请在「提供商配置 → 提供商」下拉里选择；自定义条目可放进 "
               "~/.brain-ai/providers.d/（用 --provider-dump 查看生效表）";
    }

    // ---- 图片理解：按**能力表**判断（不再写死「web 不支持」）----
    if (node.type == "VLMGenerate" && effective.spec != nullptr) {
        if (effective.kind == "web") {
            return effective.display +
                   "（网页版）不支持图片理解：请把「提供商配置」改为视觉 API 条目"
                   "（如 zhipu / siliconflow）";
        }
        bool declared = false;
        if (!ai::spec_model_supports_vision(*effective.spec, effective.model, &declared)) {
            const std::string suggested = !effective.spec->vision_model_default.empty()
                                              ? effective.spec->vision_model_default
                                              : std::string("见 --provider-dump");
            return effective.display + " 的模型 " + effective.model +
                   " 不支持图片理解（表内视觉模型：" + suggested + "）";
        }
    }

    if (effective.mode != "official") {
        return {};
    }
    if (effective.spec != nullptr && !effective.key_required) {
        return {}; // auth_style = none（本地 Ollama / 自建免鉴权服务）
    }

    // ---- 缺 Key：参数 → 环境变量（表内 env 名按序）→ 凭据库（ref）----
    const Param* key = nullptr;
    if (const Edge* provider_edge = graph.findEdgeIntoInput(node.id, "provider")) {
        if (const Node* source = graph.findNode(provider_edge->from_node)) {
            key = source->findParam("api_key");
        }
    }
    if (key == nullptr) {
        key = node.findParam("api_key");
    }
    if (key != nullptr && !key->is_empty()) {
        return {};
    }
    const std::vector<std::string> env_names =
        effective.spec != nullptr ? effective.spec->env_names : std::vector<std::string>();
    if (has_env_key(env_names)) {
        return {};
    }
    const std::string ref = effective.key_ref;
    if (!ref.empty() && !utils::load_credential(ref, nullptr).empty()) {
        return {};
    }

    const std::string env_text =
        env_names.empty() ? std::string("DEEPSEEK_API_KEY") : join_names(env_names);
    const std::string ref_text = ref.empty() ? std::string("（未设置引用名）") : ref;
    if (node.type == "VLMGenerate") {
        return "图片理解缺少 API Key（可在「提供商配置」填写并自动入库，或设置环境变量 " +
               env_text + "，或使用凭据库 " + ref_text +
               "）；模型名可在「提供商配置 → 模型（自定义）」填写视觉模型";
    }
    return "官方 API 缺少 API Key（可在「提供商配置」填写并自动入库，或设置环境变量 " + env_text +
           "，或使用凭据库 " + ref_text + "）";
}

} // namespace aiwrite::engine

