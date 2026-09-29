#include "ai/deepseek_official_provider.h"

#include "utils/image_decode.h" // M7-05：图片格式按内容嗅探（扩展名可能骗人）
#include "utils/log.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

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

// 把 API Key 作为查询参数拼到路径上（auth_style=query，如 Gemini 的 ?key=...）
std::string apply_query_key(std::string path, const std::string& name, const std::string& key)
{
    if (name.empty() || key.empty()) {
        return path;
    }
    path += (path.find('?') == std::string::npos) ? '?' : '&';
    path += name + "=" + key;
    return path;
}

// M7 / P7a-11：400 的「内容策略」判定（关键词表；大小写不敏感）
bool looks_like_content_policy(const std::string& body)
{
    static const char* kKeywords[] = {"content_policy", "content policy", "policy_violation",
                                      "moderation",      "safety",         "sensitive",
                                      "内容策略",        "内容安全",       "违规",
                                      "敏感"};
    std::string lower = body;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    for (const char* keyword : kKeywords) {
        std::string needle = keyword;
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (lower.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ---- M7 / P7a-08：编码缓存（模块级；执行器已线程化 → 必须加锁）+ 编码计数 ----
constexpr std::size_t kImageCacheLimit = 32; // 大图 data URL 很占内存 → 超限整体清空

std::mutex& image_cache_mutex()
{
    static std::mutex value;
    return value;
}

std::unordered_map<std::string, std::string>& image_cache()
{
    static std::unordered_map<std::string, std::string> value;
    return value;
}

std::size_t& image_encode_counter()
{
    static std::size_t value = 0;
    return value;
}

// ---- M5-02：多模态辅助（纯函数；离线可断言）----

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

// M7 / P7a-11：错误分类（PB-05 起就在用）—— 提为**公开纯函数**，供离线断言直接覆盖三类文案
std::string classify_http_error(int status, const std::string& body)
{
    std::ostringstream stream;
    stream << "HTTP " << status;
    switch (status) {
    case 400:
        // M7 / P7a-11：**内容策略**类与「请求不合法」同为 400 —— 用 body 关键词区分，
        //  让用户一眼看出「不是参数写错，而是内容被拒」（后端拒答属第三类错误）
        if (looks_like_content_policy(body)) {
            stream << "（**内容策略**类：后端按内容策略拒绝；请修改提示词 / 换一张图后重试，"
                      "本程序不会绕过该判定）；";
        }
        else {
            stream << "（请求不合法：请检查模型名 / 参数）；";
        }
        break;
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

// M7 / P7a-10：体积文本（自适应单位；小图不再显示成 "0.00 MB"）
std::string human_bytes(std::size_t bytes)
{
    std::ostringstream stream;
    stream.precision(1);
    stream << std::fixed;
    if (bytes < 1024u * 1024u) {
        stream << (static_cast<double>(bytes) / 1024.0) << " KB";
    }
    else {
        stream << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
    }
    return stream.str();
}

std::string image_mime_from_bytes(const unsigned char* data, std::size_t size)
{
    const utils::ImageMagic magic = utils::sniffImageBytes(data, size);
    const char*             mime  = utils::imageFormatMime(magic.format);
    return mime[0] != '\0' ? std::string(mime) : std::string();
}

std::string image_mime_from_path(const std::string& path)
{
    // M7-05：**内容优先**（魔数嗅探）—— 扩展名可能骗人（实测：WebP 存成 .png 时
    // 旧实现会贴 image/png 标签，后端可能因此拒图）；读不到文件头 / 格式不认识时
    // 回退扩展名（未知扩展名仍是 image/png，守 M5-02 既有语义）
    const utils::ImageMagic magic = utils::sniffImage(path);
    if (magic.format != utils::ImageFormat::Unknown) {
        const char* mime = utils::imageFormatMime(magic.format);
        if (mime[0] != '\0') {
            return mime;
        }
    }
    return utils::mimeFromExtension(path);
}

std::size_t image_encode_count()
{
    std::lock_guard<std::mutex> lock(image_cache_mutex());
    return image_encode_counter();
}

void reset_image_encode_cache()
{
    std::lock_guard<std::mutex> lock(image_cache_mutex());
    image_cache().clear();
    image_encode_counter() = 0;
}

std::string encode_image_data_url(const std::string& path, std::size_t max_bytes,
                                 std::string* error)
{
    // P7a-11：错误文案统一带**类别标签**（路径 / 格式 / 体积）—— 用户与断言都能一眼区分
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return std::string();
    };
    if (path.empty()) {
        return fail("图片路径为空（**路径**类错误）：请在「图片输入」节点选择图片");
    }
    const std::filesystem::path file(path);
    std::error_code             code;
    if (!std::filesystem::exists(file, code) || std::filesystem::is_directory(file, code)) {
        return fail("图片文件不存在（**路径**类错误）: " + path);
    }
    const auto file_size = static_cast<std::size_t>(std::filesystem::file_size(file, code));
    if (code) {
        return fail("无法读取图片大小（**路径**类错误）: " + path);
    }
    if (file_size == 0) {
        return fail("图片文件为空（**格式**类错误）: " + path + "；请换一张图片");
    }
    if (max_bytes > 0 && file_size > max_bytes) {
        // P7a-09：**直接拒绝**（不做隐式压缩）+ 可复制的诊断（实际 / 上限 / 下一步）
        return fail("图片过大（**体积**类错误）：实际 " + megabytes_text(file_size) + " MB（" +
                    std::to_string(file_size) + " 字节），上限 " + megabytes_text(max_bytes) +
                    " MB（" + std::to_string(max_bytes) + " 字节）→ 请压缩图片后重试，"
                    "或在配置表把该条目的 limits.image_max_bytes 调大；路径: " + path);
    }

    // P7a-08：编码缓存（键 = 路径 + 修改时间 + 大小；命中则不再 base64）
    std::error_code   time_code;
    const auto        write_time = std::filesystem::last_write_time(file, time_code);
    const std::string cache_key =
        path + "|" +
        std::to_string(time_code ? 0LL
                                : static_cast<long long>(write_time.time_since_epoch().count())) +
        "|" + std::to_string(file_size);
    {
        std::lock_guard<std::mutex> lock(image_cache_mutex());
        const auto                  found = image_cache().find(cache_key);
        if (found != image_cache().end()) {
            return found->second;
        }
    }

    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return fail("无法打开图片文件（**路径**类错误）: " + path);
    }
    std::string bytes(file_size, '\0');
    stream.read(&bytes[0], static_cast<std::streamsize>(file_size));
    if (stream.gcount() != static_cast<std::streamsize>(file_size)) {
        return fail("读取图片文件不完整（**格式**类错误）: " + path);
    }
    const std::string encoded = base64_encode(bytes);
    if (encoded.empty()) {
        return fail("图片 base64 编码失败（**格式**类错误）: " + path);
    }
    std::string data_url = "data:" + image_mime_from_path(path) + ";base64," + encoded;

    {
        std::lock_guard<std::mutex> lock(image_cache_mutex());
        if (image_cache().size() >= kImageCacheLimit) {
            image_cache().clear(); // 上限策略：超限整体清空（大图 data URL 很占内存）
        }
        image_cache()[cache_key] = data_url;
        ++image_encode_counter();
    }
    return data_url;
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

std::string resolve_chat_path(const std::string& chat_path, const std::string& model)
{
    std::string path = chat_path.empty() ? std::string("/chat/completions") : chat_path;
    if (!model.empty()) {
        const std::string placeholder = "{model}";
        for (std::size_t pos = path.find(placeholder); pos != std::string::npos;
             pos = path.find(placeholder, pos + model.size())) {
            path.replace(pos, placeholder.size(), model);
        }
    }
    return path;
}

std::string build_endpoint(const std::string& api_base, const std::string& chat_path)
{
    const std::string base =
        trim_right_slashes(api_base.empty() ? "https://api.deepseek.com" : api_base);
    return base + resolve_chat_path(chat_path, std::string());
}

std::string build_endpoint(const std::string& api_base)
{
    return build_endpoint(api_base, "/chat/completions"); // 与改造前逐字一致
}

std::string resolve_api_key(const std::string& param_key,
                            const std::vector<std::string>& env_names)
{
    if (!param_key.empty()) {
        return param_key;
    }
    if (env_names.empty()) {
        const char* env_key = std::getenv("DEEPSEEK_API_KEY");
        return (env_key != nullptr && *env_key != '\0') ? std::string(env_key) : std::string();
    }
    for (const std::string& name : env_names) {
        if (name.empty()) {
            continue;
        }
        const char* value = std::getenv(name.c_str());
        if (value != nullptr && *value != '\0') {
            return value;
        }
    }
    return {};
}

std::string resolve_api_key(const std::string& param_key)
{
    return resolve_api_key(param_key, std::vector<std::string>());
}

std::vector<std::pair<std::string, std::string>> build_auth_headers(const ProviderOptions& options,
                                                                    const std::string& api_key)
{
    std::vector<std::pair<std::string, std::string>> headers;
    const std::string style = options.auth_style.empty() ? std::string("bearer")
                                                        : options.auth_style;
    if (style == "none" || style == "query") {
        return headers; // query 走 URL 拼接（见 official_chat）
    }
    std::string name = options.auth_header;
    if (name.empty()) {
        name = (style == "api-key") ? "api-key"
                                    : (style == "x-api-key") ? "x-api-key" : "Authorization";
    }
    headers.emplace_back(name, (style == "bearer") ? ("Bearer " + api_key) : api_key);
    return headers;
}

ProviderOptions provider_options_from(const ProviderSpec& spec)
{
    ProviderOptions options;
    options.api_base         = spec.api_base;
    options.chat_path        = spec.chat_path.empty() ? std::string("/chat/completions")
                                                      : spec.chat_path;
    options.auth_style       = spec.auth_style.empty() ? std::string("bearer") : spec.auth_style;
    options.auth_header      = spec.auth_header;
    options.extra_headers    = spec.extra_headers;
    options.env_names        = spec.env_names;
    options.connect_timeout_s = spec.limits.connect_timeout_s;
    options.read_timeout_s    = spec.limits.read_timeout_s;
    return options;
}

OfficialChatResult official_chat(const OfficialChatRequest& request)
{
    OfficialChatResult result;

    // M5-02：多模态请求先整体编码/校验图片（失败则不发起 HTTP，直接给出可操作错误）
    // P7a-10：同时统计**编码后体积与耗时**（P7a-08 缓存命中时耗时接近 0）
    std::vector<std::string> image_data_urls;
    image_data_urls.reserve(request.images.size());
    const auto encode_started = std::chrono::steady_clock::now();
    for (const std::string& image_path : request.images) {
        std::string       image_error;
        std::string       data_url =
            encode_image_data_url(image_path, request.image_max_bytes, &image_error);
        if (data_url.empty()) {
            result.error = image_error.empty() ? ("图片不可用: " + image_path) : image_error;
            log::error("[官方 API] " + result.error);
            return result;
        }
        result.encoded_bytes += data_url.size();
        image_data_urls.push_back(std::move(data_url));
    }
    result.encode_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - encode_started)
            .count();

    // M_patchB L1 / PB2-04：端点 / 认证 / 超时按 options（空 = 用 request.api_base；默认 = 改造前行为）
    const ProviderOptions& options = request.options;
    const std::string      raw_base = options.api_base.empty() ? request.api_base : options.api_base;
    const std::string      base =
        trim_right_slashes(raw_base.empty() ? "https://api.deepseek.com" : raw_base);
    std::string path = resolve_chat_path(options.chat_path, request.model);
    if (options.auth_style == "query" && !request.api_key.empty()) {
        path = apply_query_key(
            path, options.auth_header.empty() ? std::string("key") : options.auth_header,
            request.api_key);
    }

    httplib::Client client(base);
    client.set_connection_timeout(options.connect_timeout_s, 0);
    client.set_read_timeout(options.read_timeout_s, 0);
    httplib::Headers headers;
    for (const std::pair<std::string, std::string>& header :
         build_auth_headers(options, request.api_key)) {
        headers.emplace(header.first, header.second);
    }
    for (const std::pair<std::string, std::string>& header : options.extra_headers) {
        headers.emplace(header.first, header.second);
    }
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
