// provider_spec_query.cpp -- Provider 配置表：纯查询（模型 / 协议 / 默认值 / resolve_*）
//
// ARCH-Z1（2026-10-23）：从 provider_spec.cpp 拆出（原 507〜643 行 + 1316〜1371 行）。
//   ProviderSpec::findModel · ProviderSpecs::find/ids/countKind · SpecLoadReport 摘要 ·
//   协议/adapter 已实现清单与显示名 · defaultChatPath/AuthStyle/AuthHeader ·
//   resolveApiBase/resolveKeyRef/resolveEnvName/resolve_model/spec_model_supports_vision。
//   全部纯函数（不碰文件系统、不碰缓存），节点 / 提示 / 自检共用。

#include "provider_spec.h"

#include "string_util.h"   // CLN-05：trim 收敛

#include <string>
#include <vector>

namespace aiwrite::ai {

// ============================================================================
//  表内查询（纯函数）
// ============================================================================
const ProviderModelSpec* ProviderSpec::findModel(const std::string& model_id) const
{
    for (const ProviderModelSpec& model : models) {
        if (model.id == model_id) {
            return &model;
        }
    }
    return nullptr;
}

std::string defaultChatPath(const std::string& protocol)
{
    if (protocol == "anthropic") {
        return "/v1/messages";
    }
    if (protocol == "gemini") {
        return "/v1beta/models/{model}:generateContent";
    }
    return "/chat/completions";
}

std::string defaultAuthStyle(const std::string& protocol)
{
    if (protocol == "anthropic") {
        return "x-api-key";
    }
    if (protocol == "gemini") {
        return "query";
    }
    return "bearer";
}

std::string defaultAuthHeader(const std::string& auth_style)
{
    if (auth_style == "api-key") {
        return "api-key";
    }
    if (auth_style == "x-api-key") {
        return "x-api-key";
    }
    if (auth_style == "query") {
        return "key";
    }
    if (auth_style == "none") {
        return {};
    }
    return "Authorization";
}

std::vector<std::string> implementedProtocols()
{
    // L1 已接线：openai 兼容（含智谱 / 硅基流动 / Ollama / 任意用户条目）
    //            + 内置网页版适配器 deepseek-web（现有 web_chat/PoW）
    // L3（PB2-13）：通用 DOM 适配器（选择器驱动）→ protocol=dom 也已实现
    // L2（B2-b · PB2-09/10/11）：工厂新增 anthropic（Messages API）与 gemini（generateContent）
    //  * 与 `provider_factory.cpp` 的 `factory_protocols()` **必须一致** → 自检 `V-14` 断言
    //    （单体真值 = 工厂；本清单供「提示 / 校验 / --provider-selftest」只读展示）
    return {"openai", "anthropic", "gemini", "deepseek-web", "dom"};
}

std::vector<std::string> implementedWebAdapters()
{
    // L3（PB2-13）：通用 DOM 适配器（选择器驱动）已实现（站点=纯数据；缺选择器 → 登录型条目）
    return {"builtin:deepseek", "dom"};
}

std::string protocolDisplay(const std::string& protocol)
{
    if (protocol == "openai") {
        return "OpenAI 兼容";
    }
    if (protocol == "anthropic") {
        return "Anthropic Messages";
    }
    if (protocol == "gemini") {
        return "Gemini generateContent";
    }
    if (protocol == "dom") {
        return "通用 DOM 适配器";
    }
    if (protocol == "deepseek-web") {
        return "DeepSeek 网页版（内置）";
    }
    return protocol;
}

std::string SpecLoadReport::summary() const
{
    std::string text = "配置表：" + std::to_string(files.size()) + " 层";
    if (fallback_used) {
        text += "（最小兜底）";
    }
    if (!errors.empty()) {
        text += "，错误 " + std::to_string(errors.size());
    }
    if (!warnings.empty()) {
        text += "，警告 " + std::to_string(warnings.size());
    }
    return text;
}

std::string SpecLoadReport::errorOf(const std::string& id) const
{
    for (const std::string& error : errors) {
        if (error.find(id) != std::string::npos) {
            return error;
        }
    }
    return {};
}

const ProviderSpec* ProviderSpecs::find(const std::string& id) const
{
    for (const ProviderSpec& spec : items) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

std::vector<std::string> ProviderSpecs::ids(const std::string& kind) const
{
    std::vector<std::string> result;
    result.reserve(items.size());
    for (const ProviderSpec& spec : items) {
        if (kind.empty() || spec.kind == kind) {
            result.push_back(spec.id);
        }
    }
    return result;
}

std::size_t ProviderSpecs::countKind(const std::string& kind) const
{
    return ids(kind).size();
}

// ============================================================================
//  解析辅助：节点参数 -> 生效值（纯函数；节点 / 提示 / 自检共用）
// ============================================================================
std::string resolveApiBase(const ProviderSpec& spec, const std::string& param_value)
{
    const std::string value = utils::trim(param_value);
    return value.empty() ? spec.api_base : value;
}

std::string resolveKeyRef(const ProviderSpec& spec, const std::string& param_value)
{
    const std::string value = utils::trim(param_value);
    return value.empty() ? spec.key_ref_default : value;
}

std::string resolveEnvName(const ProviderSpec& spec)
{
    return spec.env_names.empty() ? std::string() : spec.env_names.front();
}

std::string resolve_model(const ProviderSpec& spec, const std::string& param_model,
                          const std::string& model_custom, bool for_vision)
{
    const std::string custom = utils::trim(model_custom);
    if (!custom.empty()) {
        return custom; // M5-02：自定义模型名优先
    }
    const std::string node_model = utils::trim(param_model);
    if (!node_model.empty() && spec.findModel(node_model) != nullptr) {
        return node_model; // 节点模型属于该条目 → 用它
    }
    if (for_vision) {
        if (!spec.vision_model_default.empty()) {
            return spec.vision_model_default;
        }
        for (const ProviderModelSpec& model : spec.models) {
            if (model.vision) {
                return model.id;
            }
        }
    }
    if (!spec.models.empty()) {
        return spec.models.front().id; // 表内首选
    }
    return node_model; // 表未声明模型 → 自由填写
}

bool spec_model_supports_vision(const ProviderSpec& spec, const std::string& model,
                                bool* declared)
{
    const ProviderModelSpec* found = spec.findModel(model);
    if (declared != nullptr) {
        *declared = found != nullptr;
    }
    if (found == nullptr) {
        return true; // 表外模型：不拦截，交给后端报错（PB2-12 规则）
    }
    return found->vision;
}

} // namespace aiwrite::ai
