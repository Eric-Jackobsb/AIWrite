#include "anthropic_provider.h"

#include "deepseek_official_provider.h" // `encode_image_data_url`
#include "http_json.h"
#include "provider_common.h" // 去重专项：图片编码 / 失败归因
#include "log.h"

#include <algorithm>

namespace aiwrite::ai {
namespace {

// Anthropic `temperature` 取值域为 0…1（与 OpenAI 的 0…2 不同）⇒ 越界钳制而非报错
double clamp_anthropic_temperature(double value)
{
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

} // namespace

std::string anthropic_build_endpoint(const std::string& api_base, const std::string& chat_path,
                                     const std::string& model)
{
    std::string base = api_base.empty() ? std::string("https://api.anthropic.com") : api_base;
    while (!base.empty() && (base.back() == '/' || base.back() == ' ')) {
        base.pop_back();
    }
    std::string       path        = chat_path.empty() ? std::string("/v1/messages") : chat_path;
    const std::string placeholder = "{model}";
    for (std::size_t pos = path.find(placeholder); pos != std::string::npos && !model.empty();
         pos = path.find(placeholder, pos + model.size())) {
        path.replace(pos, placeholder.size(), model);
    }
    return base + path;
}

nlohmann::json anthropic_build_body(const GenerateParams& params, const std::string& model,
                                    bool send_seed, const std::vector<std::string>& image_data_urls)
{
    (void)send_seed; // Anthropic Messages API 无 seed 字段（`caps.seed=false`）⇒ 恒不发送
    nlohmann::json body;
    body["model"]       = model.empty() ? std::string("claude-sonnet-4-5") : model;
    body["max_tokens"]  = params.max_tokens > 0 ? params.max_tokens : 2048;
    body["temperature"] = clamp_anthropic_temperature(params.temperature);
    if (params.top_p > 0.0 && params.top_p < 1.0) {
        body["top_p"] = params.top_p;
    }
    if (!params.system_prompt.empty()) {
        body["system"] = params.system_prompt; // 顶层字段（不是 messages 里的 role）
    }
    nlohmann::json message;
    message["role"] = "user";
    if (image_data_urls.empty()) {
        message["content"] = params.prompt; // 纯文本路径
    }
    else {
        nlohmann::json parts = nlohmann::json::array();
        for (const std::string& data_url : image_data_urls) {
            std::string mime;
            std::string base64;
            if (!splitDataUrl(data_url, &mime, &base64)) {
                continue; // 非法 data URL 不静默冒充图片（调用方已按张数校验）
            }
            parts.push_back({{"type", "image"},
                             {"source",
                              {{"type", "base64"}, {"media_type", mime}, {"data", base64}}}});
        }
        parts.push_back({{"type", "text"}, {"text", params.prompt}});
        message["content"] = parts;
    }
    body["messages"] = nlohmann::json::array({message});
    return body;
}

std::string anthropicExtractText(const nlohmann::json& body)
{
    std::string text;
    const auto  content_it = body.find("content");
    if (content_it == body.end() || !content_it->is_array()) {
        return text;
    }
    for (const nlohmann::json& block : *content_it) {
        if (!block.is_object()) {
            continue;
        }
        const auto type_it = block.find("type");
        const auto text_it = block.find("text");
        if (type_it != block.end() && type_it->is_string() &&
            type_it->get<std::string>() == "text" && text_it != block.end() && text_it->is_string()) {
            text += text_it->get<std::string>();
        }
    }
    return text;
}

std::string anthropicErrorText(int http_status, const nlohmann::json& body)
{
    std::string message = jsonErrorMessage(body);
    const auto  error_it = body.find("error");
    if (error_it != body.end() && error_it->is_object() && !message.empty()) {
        const auto type_it = error_it->find("type");
        if (type_it != error_it->end() && type_it->is_string()) {
            message = "[" + type_it->get<std::string>() + "] " + message;
        }
    }
    if (message.empty()) {
        return {};
    }
    return "Anthropic（HTTP " + std::to_string(http_status) + "）：" + message;
}

AnthropicProvider::AnthropicProvider(ProviderRuntimeConfig config) : config_(std::move(config))
{
    caps_ = capsFromSpec(config_.provider_id.empty() ? nullptr
                                                       : providerSpecs().find(config_.provider_id));
}

std::string AnthropicProvider::name() const
{
    return "anthropic-messages";
}

std::string AnthropicProvider::protocol() const
{
    return "anthropic";
}

ProviderCaps AnthropicProvider::caps() const
{
    return caps_;
}

GenerateResult AnthropicProvider::generate(const GenerateParams& params)
{
    GenerateResult out;
    out.protocol      = protocol();
    out.provider_name = name();

    // ---- 图片编码（与 openai 兼容路径同口径：逐张 data URL；失败 ⇒ 可操作错误）----
    std::vector<std::string> image_data_urls;
    if (!prepare_image_data_urls(params, &image_data_urls, &out)) {
        return out;
    }

    const std::string api_base =
        config_.api_base.empty() ? config_.options.api_base : config_.api_base;
    const std::string chat_path = config_.options.chat_path.empty()
                                      ? std::string("/v1/messages")
                                      : config_.options.chat_path;

    HttpJsonRequest request;
    request.url     = anthropic_build_endpoint(api_base, chat_path, config_.model);
    request.headers = common_json_headers(config_.options, config_.api_key);
    request.body =
        anthropic_build_body(params, config_.model, caps_.seed, image_data_urls).dump();
    request.connect_timeout_s = params.connect_timeout_s > 0 ? params.connect_timeout_s
                                                             : config_.options.connect_timeout_s;
    request.read_timeout_s    = params.read_timeout_s > 0 ? params.read_timeout_s
                                                         : config_.options.read_timeout_s;
    request.cancel            = params.cancel;

    const HttpJsonResult http = httpPostJson(request);
    if (map_http_failure(http, "Anthropic", &out)) {
        return out;
    }
    if (http.http_status < 200 || http.http_status >= 300) {
        nlohmann::json body;
        std::string    parse_error;
        if (parseJsonText(http.body, &body, &parse_error)) {
            const std::string message = anthropicErrorText(http.http_status, body);
            out.error = message.empty() ? ("Anthropic 调用失败：" + parse_error) : message;
        }
        else {
            out.error = "Anthropic 调用失败（HTTP " + std::to_string(http.http_status) +
                        "）：" + parse_error;
        }
        return out;
    }
    nlohmann::json body;
    std::string    parse_error;
    if (!parseJsonText(http.body, &body, &parse_error)) {
        out.error = "Anthropic 响应解析失败：" + parse_error;
        return out;
    }
    out.text = anthropicExtractText(body);
    if (out.text.empty()) {
        out.error = "Anthropic 响应里没有文本内容（`content[].text` 为空）" +
                    (http.raw_head.empty() ? std::string() : ("；原文：" + http.raw_head));
        return out;
    }
    out.ok = true;
    if (params.on_delta) {
        params.on_delta(out.text); // 流式数据源暂停（`PB-03`）：整段到达一次
    }
    return out;
}

} // namespace aiwrite::ai
