#include "ai/deepseek_official_provider.h"

#include "utils/log.h"

#include <chrono>
#include <cstdlib>
#include <sstream>

#include <httplib.h>

namespace aiwrite::ai {
namespace {

std::string trim_right_slashes(std::string base)
{
    while (!base.empty() && (base.back() == '/' || base.back() == ' ')) {
        base.pop_back();
    }
    return base;
}

// 错误分类（PB-05：给出可操作提示，而非裸状态码）
std::string classify_http_error(int status, const std::string& body)
{
    std::ostringstream stream;
    stream << "HTTP " << status;
    switch (status) {
    case 400: stream << "（请求不合法：请检查模型名 / 参数）；"; break;
    case 401: stream << "（API Key 无效或未授权：请检查「提供商配置 → API Key」或环境变量 DEEPSEEK_API_KEY）；"; break;
    case 402: stream << "（账户余额不足：请到 DeepSeek 平台充值）；"; break;
    case 429: stream << "（请求过于频繁被限流：请稍后重试）；"; break;
    case 500:
    case 502:
    case 503:
    case 504: stream << "（服务端错误：请稍后重试）；"; break;
    default: break;
    }
    if (!body.empty()) {
        stream << body.substr(0, 240);
    }
    return stream.str();
}

} // namespace

std::string build_endpoint(const std::string& api_base)
{
    const std::string base = trim_right_slashes(api_base.empty() ? "https://api.deepseek.com" : api_base);
    return base + "/chat/completions";
}

std::string resolve_api_key(const std::string& param_key)
{
    if (!param_key.empty()) {
        return param_key;
    }
    const char* env_key = std::getenv("DEEPSEEK_API_KEY");
    return (env_key != nullptr && *env_key != '\0') ? std::string(env_key) : std::string();
}

nlohmann::json build_request_body(const OfficialChatRequest& request)
{
    nlohmann::json body;
    body["model"]       = request.model;
    body["stream"]      = false; // 流式随“实时返回”计划暂停（PB-03）
    body["temperature"] = request.temperature;
    body["max_tokens"]  = request.max_tokens;
    body["top_p"]       = request.top_p;
    body["messages"]    = nlohmann::json::array();
    if (!request.system_prompt.empty()) {
        body["messages"].push_back({{"role", "system"}, {"content", request.system_prompt}});
    }
    body["messages"].push_back({{"role", "user"}, {"content", request.prompt}});
    return body;
}

OfficialChatResult official_chat(const OfficialChatRequest& request)
{
    OfficialChatResult result;
    const std::string  endpoint = build_endpoint(request.api_base);
    const std::string  base     = endpoint.substr(0, endpoint.size() - std::string("/chat/completions").size());
    const std::string  path     = "/chat/completions";

    httplib::Client client(base);
    client.set_connection_timeout(15, 0);
    client.set_read_timeout(180, 0);
    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + request.api_key);
    headers.emplace("Accept", "application/json");

    const auto started = std::chrono::steady_clock::now();
    const auto response = client.Post(path, headers, build_request_body(request).dump(), "application/json");
    result.elapsed_ms   = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (!response) {
        result.error = "调用失败（网络/超时/DNS）：" + httplib::to_string(response.error());
        log::error("[官方 API] " + result.error);
        return result;
    }
    result.http_status = response->status;
    result.raw_head    = response->body.substr(0, 240);
    if (response->status != 200) {
        result.error = classify_http_error(response->status, response->body);
        log::error("[官方 API] " + result.error);
        return result;
    }
    try {
        const nlohmann::json data = nlohmann::json::parse(response->body);
        if (data.contains("choices") && data["choices"].is_array() && !data["choices"].empty()) {
            const nlohmann::json& message = data["choices"][0].contains("message") ? data["choices"][0]["message"] : nlohmann::json::object();
            if (message.contains("content") && message["content"].is_string()) {
                result.text = message["content"].get<std::string>();
            }
        }
    }
    catch (const std::exception& error) {
        result.error = std::string("响应解析失败：") + error.what();
        return result;
    }
    result.ok = !result.text.empty();
    if (!result.ok && result.error.empty()) {
        result.error = "HTTP 200 但未取到 choices[0].message.content";
    }
    return result;
}

} // namespace aiwrite::ai
