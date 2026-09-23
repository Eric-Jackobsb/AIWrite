#include "ai/deepseek_web_client.h"

#include "ai/web_pow.h"
#include "utils/log.h"

#include <sstream>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace aiwrite::ai {
namespace {

constexpr const char* kHost           = "https://chat.deepseek.com";
constexpr const char* kCompletionPath = "/api/v0/chat/completion";
constexpr const char* kChallengePath  = "/api/v0/chat/create_pow_challenge";

httplib::Headers base_headers(const web::Session& session)
{
    httplib::Headers headers;
    headers.emplace("Accept", "application/json, text/plain, */*");
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Authorization", "Bearer " + session.user_token);
    headers.emplace("Origin", kHost);
    headers.emplace("Referer", std::string(kHost) + "/");
    headers.emplace("User-Agent",
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                    "Chrome/126.0.0.0 Safari/537.36");
    headers.emplace("x-client-platform", "web");

    if (!session.cookies.empty()) {
        std::string cookie_line;
        for (const web::Cookie& cookie : session.cookies) {
            if (!cookie_line.empty()) {
                cookie_line += "; ";
            }
            cookie_line += cookie.name + "=" + cookie.value;
        }
        headers.emplace("Cookie", cookie_line);
    }
    return headers;
}

// 递归提取 SSE 负载中的文本
//  形态 A（较新）：{"p":"response/fragments/-1/content","v":"文本"}
//  形态 B：{"content":"文本"} / {"text":"文本"}
std::string extract_text(const nlohmann::json& node)
{
    std::string out;
    if (node.is_object()) {
        if (node.contains("p") && node.contains("v") && node["v"].is_string()) {
            if (node.value("p", std::string()).find("content") != std::string::npos) {
                return node["v"].get<std::string>();
            }
        }
        if (node.contains("content") && node["content"].is_string()) {
            out += node["content"].get<std::string>();
        }
        if (node.contains("text") && node["text"].is_string()) {
            out += node["text"].get<std::string>();
        }
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (it.key() == "content" || it.key() == "text") {
                continue;
            }
            out += extract_text(it.value());
        }
    }
    else if (node.is_array()) {
        for (const nlohmann::json& item : node) {
            out += extract_text(item);
        }
    }
    return out;
}

std::string trim(const std::string& text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}


// 从 {"data":{"biz_data":{...}}} 里取一层（缺失返回 nullptr，避免 operator[] 的 UB）
const nlohmann::json* biz_data_of(const nlohmann::json& json)
{
    if (!json.contains("data")) {
        return nullptr;
    }
    const nlohmann::json& data = json["data"];
    if (!data.contains("biz_data")) {
        return nullptr;
    }
    return &data["biz_data"];
}

// 建新会话：POST /api/v0/chat_session/create（字段名按实测响应探测；失败返回空）
std::string create_chat_session(httplib::Client& client, const httplib::Headers& headers)
{
    const auto response =
        client.Post("/api/v0/chat_session/create", headers, "{}", "application/json");
    if (!response || response->status != 200) {
        return {};
    }
    try {
        const nlohmann::json  json = nlohmann::json::parse(response->body);
        const nlohmann::json* biz  = biz_data_of(json);
        if (json.value("code", -1) != 0 || biz == nullptr) {
            return {};
        }
        if (biz->contains("chat_session") && (*biz)["chat_session"].is_object()) {
            return (*biz)["chat_session"].value("id", std::string());
        }
        if (biz->contains("id") && (*biz)["id"].is_string()) {
            return (*biz)["id"].get<std::string>();
        }
    }
    catch (const std::exception&) {
    }
    return {};
}

// 复用最近会话：GET /api/v0/chat_session/fetch_page（字段名取自实测响应）
bool reuse_recent_session(httplib::Client& client, const httplib::Headers& headers,
                          std::string* session_id, long long* current_message_id, std::string* error)
{
    const auto response = client.Get("/api/v0/chat_session/fetch_page", headers);
    if (!response) {
        *error = "会话列表请求失败: " + httplib::to_string(response.error());
        return false;
    }
    try {
        const nlohmann::json  json = nlohmann::json::parse(response->body);
        const nlohmann::json* biz  = biz_data_of(json);
        if (biz == nullptr || !biz->contains("chat_sessions") ||
            !(*biz)["chat_sessions"].is_array() || (*biz)["chat_sessions"].empty()) {
            *error = "账号下没有可用会话（请先在网页版里手动发一条消息）";
            return false;
        }
        const nlohmann::json& first = (*biz)["chat_sessions"][0];
        *session_id         = first.value("id", std::string());
        *current_message_id = first.value("current_message_id", 0LL);
        return !session_id->empty();
    }
    catch (const std::exception& ex) {
        *error = std::string("会话列表解析失败: ") + ex.what();
        return false;
    }
}

} // namespace

WebChatResult web_chat(const web::Session& session, const WebChatRequest& request)
{
    WebChatResult result;
    if (session.user_token.empty()) {
        result.error = "会话缺少 userToken：请先在网页版登录窗口完成登录";
        return result;
    }

    httplib::Client client(kHost);
    client.set_connection_timeout(15, 0);
    client.set_read_timeout(180, 0);
    client.enable_server_certificate_verification(true);

    // ---------------------------------------------------------- 1) 挑战 -----
    const httplib::Headers headers = base_headers(session);
    const auto             challenge_resp =
        client.Post(kChallengePath, headers, R"({"target_path":"/api/v0/chat/completion"})",
                    "application/json");
    if (!challenge_resp) {
        result.error = "挑战请求失败: " + httplib::to_string(challenge_resp.error());
        return result;
    }

    PowChallenge challenge;
    std::string  parse_error;
    if (!parse_pow_challenge(challenge_resp->body, &challenge, &parse_error)) {
        result.error =
            "挑战解析失败: " + parse_error + " | 响应: " + challenge_resp->body.substr(0, 300);
        return result;
    }
    log::info("[网页版] 挑战已获取：algorithm=" + challenge.algorithm +
              " difficulty=" + std::to_string(challenge.difficulty));

    // ---------------------------------------------------------- 2) 求解 -----
    long long         attempts = 0;
    std::string       solve_error;
    const std::string answer = solve_pow(challenge, &attempts, &solve_error);
    result.pow_attempts      = attempts;
    log::info("[网页版] PoW 求解：" + (answer.empty() ? std::string("失败") : "nonce=" + answer) +
              "，尝试 " + std::to_string(attempts) + " 次");
    if (answer.empty()) {
        result.error = "PoW 求解失败: " + solve_error;
        return result;
    }

    // ---------------------------------------------------------- 3) 调用 -----
    httplib::Headers call_headers = headers;
    call_headers.emplace("Accept", "text/event-stream");
    call_headers.emplace("x-ds-pow-response", make_pow_header_value(challenge, answer));

    // 会话与 parent（网页版要求 chat_session_id 为 UUID）
    std::string session_id  = request.chat_session_id;
    long long   parent_id   = request.parent_message_id;
    if (session_id.empty()) {
        session_id = create_chat_session(client, headers);
        if (!session_id.empty()) {
            log::info("[网页版] 已新建会话（" + session_id + "）");
            parent_id = 0;
        }
        else {
            std::string session_error;
            if (!reuse_recent_session(client, headers, &session_id, &parent_id, &session_error)) {
                result.error = "无法建立会话: " + session_error;
                return result;
            }
            log::info("[网页版] 复用最近会话（" + session_id + "，parent=" +
                      std::to_string(parent_id) + "）");
        }
    }

    nlohmann::json body;
    body["chat_session_id"] = session_id;
    body["parent_message_id"] =
        (parent_id > 0) ? nlohmann::json(parent_id) : nlohmann::json(nullptr);
    body["model_type"]       = request.model_type;
    body["prompt"]           = request.prompt;
    body["ref_file_ids"]     = nlohmann::json::array();
    body["thinking_enabled"] = request.thinking_enabled;
    body["search_enabled"]   = request.search_enabled;

    const auto response =
        client.Post(kCompletionPath, call_headers, body.dump(), "application/json");
    if (!response) {
        result.error = "调用失败: " + httplib::to_string(response.error());
        return result;
    }
    result.http_status = response->status;

    // ---------------------------------------------------------- 4) 解析 -----
    std::istringstream stream(response->body);
    std::string        line;
    int                line_index = 0;
    while (std::getline(stream, line)) {
        if (line_index < request.raw_head_lines) {
            result.raw_head += line + "\n";
        }
        ++line_index;

        const std::string trimmed = trim(line);
        if (trimmed.rfind("data:", 0) != 0) {
            continue;
        }
        const std::string payload = trim(trimmed.substr(5));
        if (payload.empty()) {
            continue;
        }
        if (payload == "[DONE]" || payload == "finished") {
            break;
        }
        try {
            result.text += extract_text(nlohmann::json::parse(payload));
        }
        catch (const std::exception&) {
            // 非 JSON 的 SSE 行（心跳 / 结束标记）忽略
        }
    }

    result.ok = (response->status == 200);
    if (!result.ok) {
        result.error =
            "HTTP " + std::to_string(response->status) + "：" + response->body.substr(0, 300);
    }
    else if (result.text.empty()) {
        result.error = "HTTP 200 但未解析出文本（请查看 raw_head 原始行以对齐解析规则）";
    }
    return result;
}

} // namespace aiwrite::ai
