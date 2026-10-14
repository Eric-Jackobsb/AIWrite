#include "inference_provider.h"

#include "deepseek_web_client.h" // `web_chat()`（内置网页版实现，`I2`：行为逐字不变）
#include "log.h"

#include <algorithm>
#include <cctype>

namespace aiwrite::ai {

// ---------------------------------------------------------------- 能力快照 --

ProviderCaps permissive_caps()
{
    // 与表内**缺省能力**不同：这里要的是「不拦任何能力」（`vision=true`）——
    //  * 用途：无表条目时（例：老工作流 / 临时自定义地址）不新增门控 ⇒ 行为与改造前一致（`I19`）
    ProviderCaps caps;
    caps.vision      = true;
    caps.seed        = true;
    caps.system_role = true;
    caps.stream      = false;
    return caps;
}

ProviderCaps caps_from_spec(const ProviderSpec* spec)
{
    return spec != nullptr ? spec->caps : permissive_caps();
}

ProviderCaps caps_for_call(const ProviderSpec* spec, bool web_mode)
{
    if (spec != nullptr) {
        return spec->caps;
    }
    if (web_mode) {
        // 无表条目的网页版调用 = 内置网页版协议（`deepseek-web`）的能力。
        //  * 表里查不到（用户把 providers.json 删了？）⇒ 用与内置条目一致的常量兜底
        const ProviderSpec* builtin = provider_specs().find("deepseek-web");
        if (builtin != nullptr) {
            return builtin->caps;
        }
        ProviderCaps caps;
        caps.vision      = false;
        caps.seed        = false;
        caps.system_role = false;
        caps.stream      = true;
        return caps;
    }
    return permissive_caps();
}

bool protocol_needs_key(const ProviderOptions& options)
{
    return options.auth_style != "none";
}

// ---------------------------------------------------------------- 共用请求件 --

std::vector<std::pair<std::string, std::string>>
common_json_headers(const ProviderOptions& options, const std::string& api_key)
{
    // 认证头（`PB2-04` 纯函数；无 Key ⇒ 不构造凭证头，`V-11`）
    std::vector<std::pair<std::string, std::string>> headers = build_auth_headers(options, api_key);
    for (const std::pair<std::string, std::string>& header : options.extra_headers) {
        headers.push_back(header);
    }
    headers.emplace_back("Accept", "application/json");
    return headers;
}

bool split_data_url(const std::string& data_url, std::string* mime, std::string* base64)
{
    static const std::string prefix = "data:";
    if (data_url.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    const std::size_t comma = data_url.find(',');
    if (comma == std::string::npos) {
        return false;
    }
    const std::string meta = data_url.substr(prefix.size(), comma - prefix.size());
    const std::size_t semi = meta.find(';');
    const std::string kind = (semi == std::string::npos) ? meta : meta.substr(0, semi);
    const bool        is_base64 = meta.find("base64") != std::string::npos;
    if (kind.empty() || !is_base64) {
        return false;
    }
    if (mime != nullptr) {
        *mime = kind;
    }
    if (base64 != nullptr) {
        *base64 = data_url.substr(comma + 1);
    }
    return true;
}

// ---------------------------------------------------------------- 能力门控 --

CapabilityGate capability_gate(const ProviderCaps& caps, const GenerateParams& params,
                               bool has_images)
{
    CapabilityGate gate;
    if (has_images && !caps.vision) {
        gate.blocking.push_back(
            "当前提供商不支持图片输入（配置表 capabilities.vision=false）→ 请改用支持视觉的服务"
            "（如智谱 glm-4v-flash / 硅基流动视觉模型 / 任意 OpenAI 兼容视觉模型，在"
            "「提供商配置 → 模型（自定义）」填视觉模型名），或去掉图片输入");
    }
    if (params.seed > 0 && !caps.seed) {
        gate.warnings.push_back("该提供商不支持 seed（capabilities.seed=false）⇒ **已忽略 seed**；"
                                "需要可复现请改用支持 seed 的服务（如官方 API）");
    }
    if (!params.system_prompt.empty() && !caps.system_role) {
        gate.system_prompt_inlined = true;
        gate.warnings.push_back("该通道没有 system 角色槽位（capabilities.system_role=false）⇒ "
                                "系统提示词已**前置拼接**进用户提示词");
    }
    return gate;
}

// ---------------------------------------------------------------- openai 兼容 --

OpenAiCompatProvider::OpenAiCompatProvider(ProviderRuntimeConfig config)
    : config_(std::move(config))
{
    if (!config_.provider_id.empty()) {
        caps_ = caps_from_spec(provider_specs().find(config_.provider_id));
    }
}

std::string OpenAiCompatProvider::name() const
{
    return "openai-compat";
}

std::string OpenAiCompatProvider::protocol() const
{
    return "openai";
}

ProviderCaps OpenAiCompatProvider::caps() const
{
    return caps_;
}

GenerateResult OpenAiCompatProvider::generate(const GenerateParams& params)
{
    GenerateResult out;
    out.protocol      = protocol();
    out.provider_name = name();

    OfficialChatRequest request;
    request.options          = config_.options;
    request.options.api_base = config_.api_base.empty() ? config_.options.api_base : config_.api_base;
    request.api_base         = request.options.api_base;
    request.api_key          = config_.api_key;
    request.model            = config_.model.empty() ? std::string("deepseek-chat") : config_.model;
    request.system_prompt    = params.system_prompt;
    request.prompt           = params.prompt;
    request.images           = params.images;
    request.image_max_bytes  = params.image_max_bytes > 0 ? params.image_max_bytes
                                                          : request.image_max_bytes;
    request.temperature      = params.temperature;
    request.max_tokens       = params.max_tokens;
    request.top_p            = params.top_p;
    request.seed             = caps_.seed ? params.seed : 0; // 能力门控：不支持则不发
    request.cancel           = params.cancel;
    if (params.connect_timeout_s > 0) {
        request.options.connect_timeout_s = params.connect_timeout_s;
    }
    if (params.read_timeout_s > 0) {
        request.options.read_timeout_s = params.read_timeout_s;
    }

    const OfficialChatResult result = official_chat(request);
    out.ok            = result.ok;
    out.http_status   = result.http_status;
    out.text          = result.text;
    out.error         = result.error;
    out.raw_head      = result.raw_head;
    out.elapsed_ms    = result.elapsed_ms;
    out.cancelled     = result.cancelled;
    out.encoded_bytes = result.encoded_bytes;
    out.encode_ms     = result.encode_ms;
    // 流式数据源暂停（`PB-03`）：整段到达后一次性回调（槽位保留，避免虚函数空转 `D-09`）
    if (out.ok && !out.text.empty() && params.on_delta) {
        params.on_delta(out.text);
    }
    return out;
}

// ---------------------------------------------------------------- 内置网页版 --

DeepSeekWebProvider::DeepSeekWebProvider(ProviderRuntimeConfig config)
    : config_(std::move(config))
{
    caps_ = caps_from_spec(config_.provider_id.empty()
                               ? nullptr
                               : provider_specs().find(config_.provider_id));
}

std::string DeepSeekWebProvider::name() const
{
    return "deepseek-web";
}

std::string DeepSeekWebProvider::protocol() const
{
    return "deepseek-web";
}

ProviderCaps DeepSeekWebProvider::caps() const
{
    return caps_;
}

GenerateResult DeepSeekWebProvider::generate(const GenerateParams& params)
{
    GenerateResult out;
    out.protocol      = protocol();
    out.provider_name = name();
    if (!config_.session_supplier) {
        out.error = "网页版通道缺少会话来源（内部调用未提供 session_supplier）";
        return out;
    }

    WebChatRequest request;
    request.prompt           = params.prompt;
    request.model_type       = (config_.model == "expert") ? "expert" : "default";
    request.thinking_enabled = (config_.model == "deepseek-reasoner");
    request.cancel           = params.cancel;
    request.on_delta         = params.on_delta;
    request.owner            = config_.owner;
    if (!config_.web.endpoints.host.empty()) {
        request.endpoints = config_.web.endpoints;
    }
    if (!params.system_prompt.empty() && !caps_.system_role) {
        // 能力门控建议的**前置拼接**（与节点既有行为一致）
        request.prompt = params.system_prompt + "\n\n" + params.prompt;
    }

    const web::Session  session = config_.session_supplier();
    const WebChatResult result  = web_chat(session, request);
    out.ok                      = result.ok;
    out.http_status             = result.http_status;
    out.text                    = result.text;
    out.error                   = result.error;
    out.raw_head                = result.raw_head;
    out.cancelled               = result.cancelled;
    return out;
}

} // namespace aiwrite::ai
