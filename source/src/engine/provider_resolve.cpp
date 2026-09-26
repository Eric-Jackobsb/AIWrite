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

// ---- 3) M_patchB L1（PB2-05）：查配置表，用表默认值补齐 / 决定 kind 与能力 ----
//  优先级：工作流节点参数（非空）→ 配置表默认值 → 老行为（表里没有该 id）
//  * D-21：**不**按条目 `kind` 改写 `mode`（用户选什么就是什么；不一致由界面/校验给提示）
void apply_spec_table(EffectiveProvider& result, const Node& config_node, bool for_vision)
{
    const std::string api_base_param = param_text(config_node, "api_base", std::string());
    const std::string key_ref_param  = param_text(config_node, "api_key_ref", std::string());

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
        result.model = ai::resolve_model(spec, result.model, result.model_custom, for_vision);
    }
    else {
        // 表里没有该 id → 保持改造前的行为（节点参数为准；老工作流迁移落点 custom-official）
        result.api_base = api_base_param;
        result.key_ref  = key_ref_param;
    }
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
    apply_spec_table(result, *config_node, node.type == "VLMGenerate");
    return result;
}

// M_patchB L2 修订（PB2-20）：按**节点自身参数**解析（「提供商配置」节点就是它自己的配置来源）
//  * 与 resolve_effective_provider() 的「节点自身参数」分支**同一套规则**（同一 helper），
//    区别只在：不查 provider 连线、from_edge 恒 false
EffectiveProvider resolve_self_provider(const Node& node)
{
    EffectiveProvider result;
    result.resolved     = true;
    result.from_edge    = false;
    result.source_node  = node.id;
    result.provider     = param_text(node, "provider", result.provider);
    result.mode         = param_text(node, "mode", result.mode);
    result.model        = param_text(node, "model", result.model);
    result.model_custom = param_text(node, "model_custom", std::string());
    apply_spec_table(result, node, node.type == "VLMGenerate");
    return result;
}

EffectiveProvider resolve_display_provider(const Graph& graph, const Node& node)
{
    if (uses_provider(node.type)) {
        return resolve_effective_provider(graph, node); // 推理节点：provider 连线优先
    }
    return resolve_self_provider(node); // 「提供商配置」等：按节点自身参数（PB2-20 / I13）
}

bool official_not_wired(const EffectiveProvider& provider)
{
    return provider.resolved && provider.mode == "official";
}

std::vector<std::string> provider_mode_options(const Node& node)
{
    (void)node; // D-21：候选**不按条目 kind 裁剪**（入参保留 = 单点开关，将来若需收窄只改这里）
    return {"official", "web"};
}

std::string provider_mode_suggestion(const Node& node)
{
    const std::string                        provider = param_text(node, "provider", std::string("deepseek"));
    std::shared_ptr<const ai::ProviderSpecs> specs    = ai::provider_specs_snapshot();
    const ai::ProviderSpec*                  spec     = specs ? specs->find(provider) : nullptr;
    if (spec == nullptr) {
        return {}; // 表里没有该 id：不给建议（避免猜）
    }
    if (spec->kind == "web") {
        return "web";
    }
    if (spec->kind == "official") {
        return "official";
    }
    return {};
}

std::string mode_kind_hint(const EffectiveProvider& provider)
{
    if (!provider.resolved || provider.spec == nullptr) {
        return {}; // 表里没有该条目：由 unwired_reason 给「配置表里没有提供商」的可操作提示
    }
    const std::string display = provider.display.empty() ? provider.provider : provider.display;
    if (provider.kind == "official" && provider.mode == "web") {
        return "「" + display +
               "」不是网页版条目：**没有网页版站点可用**（不会再回落到内置默认站点，决策 D-22②）；"
               "可选：① 把「提供商」改为网页版条目（如 deepseek-web）；"
               "② 新建网页版站点条目（~/.brain-ai/providers.d/，见使用说明 §9）；"
               "③ 把「模式」改回 official";
    }
    if (provider.kind == "web" && provider.mode == "official") {
        return "「" + display +
               "」是网页版条目，但模式为 official：该条目没有官方 API 通道"
               "（按表内 API 地址解析，缺失时会明确报错）；建议把「模式」改为 web";
    }
    return {};
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

    // ---- 决策 D-22② / 不变量 I14：网页版模式必须有**该条目自己的**站点（**不**回落内置默认站点）----
    if (effective.mode == "web") {
        const std::string site_error = ai::web_site_error(effective.spec);
        if (!site_error.empty()) {
            return site_error;
        }
    }

    // ---- 网页版条目 + mode=official（D-21）：该条目**没有官方 API 通道** → 明确报错，
    //      而不是「静默回落 + 只说缺 Key」（避免用户填了 Key 才发现地址为空）----
    if (effective.kind == "web" && effective.mode != "web") {
        return mode_kind_hint(effective);
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

