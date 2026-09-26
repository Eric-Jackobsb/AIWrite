#include "ai/deepseek_official_provider.h"

#include "utils/log.h"

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <httplib.h>
#include <openssl/evp.h>

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

// ---- M5-02：多模态辅助（纯函数；离线可断言）----

std::string lower_text(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

// 字节数 → 便于阅读的 MB 文本（1 位小数）
std::string megabytes_text(std::size_t bytes)
{
    std::ostringstream stream;
    stream.precision(1);
    stream << std::fixed << (static_cast<double>(bytes) / (1024.0 * 1024.0));
    return stream.str();
}

// Base64（OpenSSL EVP_EncodeBlock：无新依赖，aiwrite_core 已链接 OpenSSL::Crypto）
std::string base64_encode(const std::string& bytes)
{
    if (bytes.empty()) {
        return {};
    }
    const int         input_length = static_cast<int>(bytes.size());
    const int         capacity     = 4 * ((input_length + 2) / 3);
    std::string       encoded(static_cast<std::size_t>(capacity), '\0');
    const int         written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(&encoded[0]),
        reinterpret_cast<const unsigned char*>(bytes.data()), input_length);
    if (written <= 0) {
        return {};
    }
    encoded.resize(static_cast<std::size_t>(written));
    return encoded;
}

} // namespace

std::string image_mime_from_path(const std::string& path)
{
    const std::string extension = lower_text(std::filesystem::path(path).extension().string());
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    if (extension == ".webp") {
        return "image/webp";
    }
    if (extension == ".bmp") {
        return "image/bmp";
    }
    if (extension == ".gif") {
        return "image/gif";
    }
    return "image/png"; // 未知扩展名回退（服务端多按内容嗅探）
}

std::string encode_image_data_url(const std::string& path, std::size_t max_bytes,
                                 std::string* error)
{
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return std::string();
    };
    if (path.empty()) {
        return fail("图片路径为空：请在「图片输入」节点选择图片");
    }
    const std::filesystem::path file(path);
    std::error_code             code;
    if (!std::filesystem::exists(file, code) || std::filesystem::is_directory(file, code)) {
        return fail("图片文件不存在: " + path);
    }
    const auto file_size = static_cast<std::size_t>(std::filesystem::file_size(file, code));
    if (code) {
        return fail("无法读取图片大小: " + path);
    }
    if (file_size == 0) {
        return fail("图片文件为空: " + path);
    }
    if (max_bytes > 0 && file_size > max_bytes) {
        return fail("图片过大（" + megabytes_text(file_size) + " MB，上限 " +
                    megabytes_text(max_bytes) + " MB）：请压缩后重试 —— " + path);
    }
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return fail("无法打开图片文件: " + path);
    }
    std::string bytes(file_size, '\0');
    stream.read(&bytes[0], static_cast<std::streamsize>(file_size));
    if (stream.gcount() != static_cast<std::streamsize>(file_size)) {
        return fail("读取图片文件不完整: " + path);
    }
    const std::string encoded = base64_encode(bytes);
    if (encoded.empty()) {
        return fail("图片 base64 编码失败: " + path);
    }
    return "data:" + image_mime_from_path(path) + ";base64," + encoded;
}

nlohmann::json build_request_body(const OfficialChatRequest& request)
{
    // M5-02：纯函数重载 —— 内部编码图片（失败图跳过），供离线断言与单调用方使用
    std::vector<std::string> data_urls;
    data_urls.reserve(request.images.size());
    for (const std::string& path : request.images) {
        std::string data_url = encode_image_data_url(path, request.image_max_bytes, nullptr);
        if (!data_url.empty()) {
            data_urls.push_back(std::move(data_url));
        }
    }
    return build_request_body(request, data_urls);
}

nlohmann::json build_request_body(const OfficialChatRequest& request,
                                  const std::vector<std::string>& image_data_urls)
{
    nlohmann::json body;
    body["model"]       = request.model;
    body["stream"]      = false; // 流式随“实时返回”计划暂停（PB-03）
    body["temperature"] = request.temperature;
    body["max_tokens"]  = request.max_tokens;
    body["top_p"]       = request.top_p;
    if (request.seed > 0) {
        body["seed"] = request.seed; // M_rerun：新 seed 重跑（后端若不支持则忽略）
    }
    body["messages"]    = nlohmann::json::array();
    if (!request.system_prompt.empty()) {
        body["messages"].push_back({{"role", "system"}, {"content", request.system_prompt}});
    }
    if (image_data_urls.empty()) {
        // 纯文本路径：与 M4/PB-05 的线上行为完全一致（回归基线）
        body["messages"].push_back({{"role", "user"}, {"content", request.prompt}});
        return body;
    }
    // M5-02：多模态路径 —— content 数组（文本块 + 每图一个 image_url 块；data URL）
    nlohmann::json parts = nlohmann::json::array();
    parts.push_back({{"type", "text"}, {"text", request.prompt}});
    for (const std::string& data_url : image_data_urls) {
        parts.push_back({{"type", "image_url"}, {"image_url", {{"url", data_url}}}});
    }
    body["messages"].push_back({{"role", "user"}, {"content", parts}});
    return body;
}

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

OfficialChatResult official_chat(const OfficialChatRequest& request)
{
    OfficialChatResult result;

    // M5-02：多模态请求先整体编码/校验图片（失败则不发起 HTTP，直接给出可操作错误）
    std::vector<std::string> image_data_urls;
    image_data_urls.reserve(request.images.size());
    for (const std::string& image_path : request.images) {
        std::string       image_error;
        std::string       data_url =
            encode_image_data_url(image_path, request.image_max_bytes, &image_error);
        if (data_url.empty()) {
            result.error = image_error.empty() ? ("图片不可用: " + image_path) : image_error;
            log::error("[官方 API] " + result.error);
            return result;
        }
        image_data_urls.push_back(std::move(data_url));
    }

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
    const auto response =
        client.Post(path, headers, build_request_body(request, image_data_urls).dump(),
                    "application/json");
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
