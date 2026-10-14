#include "gemini_provider.h"

#include "deepseek_official_provider.h" // `encode_image_data_url`
#include "http_json.h"
#include "log.h"

#include <algorithm>

namespace aiwrite::ai {

std::string gemini_build_endpoint(const std::string& api_base, const std::string& chat_path,
                                  const std::string& model, const std::string& auth_name,
                                  const std::string& api_key)
{
    std::string base =
        api_base.empty() ? std::string("https://generativelanguage.googleapis.com") : api_base;
    while (!base.empty() && (base.back() == '/' || base.back() == ' ')) {
        base.pop_back();
    }
    std::string path = chat_path.empty() ? std::string("/v1beta/models/{model}:generateContent")
                                        : chat_path;
    const std::string placeholder = "{model}";
    for (std::size_t pos = path.find(placeholder); pos != std::string::npos; ) {
        if (model.empty()) {
            break; // 无模型名 ⇒ 保留占位符（由调用方报错，不猜）
        }
        path.replace(pos, placeholder.size(), model);
        pos = path.find(placeholder, pos + model.size());
    }
    std::string url = base + path;
    if (!api_key.empty()) {
        url = append_query_param(url, auth_name.empty() ? std::string("key") : auth_name, api_key);
    }
    return url;
}

nlohmann::json gemini_build_body(const GenerateParams& params, bool send_seed,
                                 const std::vector<std::string>& image_data_urls)
{
    nlohmann::json parts = nlohmann::json::array();
    parts.push_back({{"text", params.prompt}});
    for (const std::string& data_url : image_data_urls) {
        std::string mime;
        std::string base64;
        if (!split_data_url(data_url, &mime, &base64)) {
            continue; // 非法 data URL 不静默冒充图片
        }
        parts.push_back({{"inline_data", {{"mime_type", mime}, {"data", base64}}}});
    }

    nlohmann::json body;
    body["contents"] = nlohmann::json::array({nlohmann::json{{"role", "user"}, {"parts", parts}}});
    if (!params.system_prompt.empty()) {
        body["systemInstruction"] = {
            {"parts", nlohmann::json::array({nlohmann::json{{"text", params.system_prompt}}})}};
    }
    nlohmann::json config;
    config["temperature"]    = params.temperature;
    config["topP"]           = params.top_p;
    config["maxOutputTokens"] = params.max_tokens > 0 ? params.max_tokens : 2048;
    if (send_seed && params.seed > 0) {
        config["seed"] = params.seed;
    }
    body["generationConfig"] = config;
    return body;
}

std::string gemini_extract_text(const nlohmann::json& body, std::size_t candidate)
{
    std::string text;
    const auto  candidates_it = body.find("candidates");
    if (candidates_it == body.end() || !candidates_it->is_array() ||
        candidate >= candidates_it->size()) {
        return text;
    }
    const nlohmann::json& first = (*candidates_it)[candidate];
    if (!first.is_object()) {
        return text;
    }
    const auto content_it = first.find("content");
    if (content_it == first.end() || !content_it->is_object()) {
        return text;
    }
    const auto parts_it = content_it->find("parts");
    if (parts_it == content_it->end() || !parts_it->is_array()) {
        return text;
    }
    for (const nlohmann::json& part : *parts_it) {
        if (!part.is_object()) {
            continue;
        }
        const auto text_it = part.find("text");
        if (text_it != part.end() && text_it->is_string()) {
            text += text_it->get<std::string>();
        }
    }
    return text;
}

std::string gemini_block_reason(const nlohmann::json& body)
{
    // ① 安全策略（整轮被挡）：`promptFeedback.blockReason`
    const auto feedback_it = body.find("promptFeedback");
    if (feedback_it != body.end() && feedback_it->is_object()) {
        const auto reason_it = feedback_it->find("blockReason");
        if (reason_it != feedback_it->end() && reason_it->is_string()) {
            return "提示词被站点安全策略拦截（blockReason=" + reason_it->get<std::string>() +
                   "）——请改写提示词或去掉可能触发策略的图片";
        }
    }
    // ② 完成原因（本候选没出文本）：`candidates[0].finishReason`
    const auto candidates_it = body.find("candidates");
    if (candidates_it != body.end() && candidates_it->is_array() && !candidates_it->empty() &&
        (*candidates_it)[0].is_object()) {
        const auto reason_it = (*candidates_it)[0].find("finishReason");
        if (reason_it != (*candidates_it)[0].end() && reason_it->is_string()) {
            const std::string reason = reason_it->get<std::string>();
            if (reason != "STOP") {
                return "生成未正常结束（finishReason=" + reason +
                       "）——请调整提示词 / 参数后重试";
            }
        }
    }
    return json_error_message(body);
}

GeminiProvider::GeminiProvider(ProviderRuntimeConfig config) : config_(std::move(config))
{
    caps_ = caps_from_spec(config_.provider_id.empty() ? nullptr
                                                       : provider_specs().find(config_.provider_id));
}

std::string GeminiProvider::name() const
{
    return "gemini-generate-content";
}

std::string GeminiProvider::protocol() const
{
    return "gemini";
}

ProviderCaps GeminiProvider::caps() const
{
    return caps_;
}

GenerateResult GeminiProvider::generate(const GenerateParams& params)
{
    GenerateResult out;
    out.protocol      = protocol();
    out.provider_name = name();

    std::vector<std::string> image_data_urls;
    for (const std::string& path : params.images) {
        std::string       encode_error;
        const std::string data_url =
            encode_image_data_url(path, params.image_max_bytes, &encode_error);
        if (data_url.empty()) {
            out.error = "图片准备失败：" + (encode_error.empty() ? path : encode_error);
            return out;
        }
        image_data_urls.push_back(data_url);
        out.encoded_bytes += data_url.size();
    }

    const std::string api_base =
        config_.api_base.empty() ? config_.options.api_base : config_.api_base;
    const std::string chat_path =
        config_.options.chat_path.empty() ? std::string("/v1beta/models/{model}:generateContent")
                                         : config_.options.chat_path;

    HttpJsonRequest request;
    request.url = gemini_build_endpoint(api_base, chat_path, config_.model,
                                        config_.options.auth_header, config_.api_key);
    // auth_style=query ⇒ 认证在 URL 里，**不**再产生凭证头（`build_auth_headers` 已按表处理）
    request.headers = common_json_headers(config_.options, config_.api_key);
    request.body    = gemini_build_body(params, caps_.seed, image_data_urls).dump();
    request.connect_timeout_s = params.connect_timeout_s > 0 ? params.connect_timeout_s
                                                             : config_.options.connect_timeout_s;
    request.read_timeout_s    = params.read_timeout_s > 0 ? params.read_timeout_s
                                                         : config_.options.read_timeout_s;
    request.cancel            = params.cancel;

    const HttpJsonResult http = http_post_json(request);
    out.http_status           = http.http_status;
    out.elapsed_ms            = http.elapsed_ms;
    out.raw_head              = http.raw_head;
    if (http.cancelled) {
        out.cancelled = true;
        out.error     = http.error;
        return out;
    }
    if (!http.ok) {
        out.error = std::string("Gemini 调用失败：") + http.error;
        return out;
    }
    nlohmann::json body;
    std::string    parse_error;
    const bool     parsed = parse_json_text(http.body, &body, &parse_error);
    if (http.http_status < 200 || http.http_status >= 300) {
        const std::string reason = parsed ? gemini_block_reason(body) : std::string();
        out.error = reason.empty()
                        ? ("Gemini 调用失败（HTTP " + std::to_string(http.http_status) +
                           "）：" + parse_error)
                        : ("Gemini（HTTP " + std::to_string(http.http_status) + "）：" + reason);
        return out;
    }
    if (!parsed) {
        out.error = "Gemini 响应解析失败：" + parse_error;
        return out;
    }
    out.text = gemini_extract_text(body);
    if (out.text.empty()) {
        const std::string reason = gemini_block_reason(body);
        out.error = reason.empty()
                        ? ("Gemini 响应里没有文本内容（`candidates[0].content.parts[].text` 为空）" +
                           (http.raw_head.empty() ? std::string() : ("；原文：" + http.raw_head)))
                        : reason;
        return out;
    }
    out.ok = true;
    if (params.on_delta) {
        params.on_delta(out.text); // 流式数据源暂停（`PB-03`）：整段到达一次
    }
    return out;
}

} // namespace aiwrite::ai
