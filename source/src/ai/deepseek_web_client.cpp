#include "ai/deepseek_web_client.h"

#include "ai/web_pow.h"
#include "utils/log.h"
#include "web/webview_host.h"

#include <sstream>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace aiwrite::ai {
namespace {

constexpr const char* kHost           = "https://chat.deepseek.com";
constexpr const char* kCompletionPath = "/api/v0/chat/completion";
constexpr const char* kChallengePath  = "/api/v0/chat/create_pow_challenge";

httplib::Headers base_headers(const web::Session& session, const std::string& host)
{
    const std::string origin = host.empty() ? std::string(kHost) : host;
    httplib::Headers headers;
    headers.emplace("Accept", "application/json, text/plain, */*");
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Authorization", "Bearer " + session.user_token);
    headers.emplace("Origin", origin);
    headers.emplace("Referer", origin + "/");
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

// SSE 负载 → **增量**文本
//  形态 A（较新）：{"p":"response/fragments/-1/content","o":"APPEND","v":"文本"}
//  形态 B（旧版）  ：{"p":"response/content","v":"文本"}
std::string delta_text_of(const nlohmann::json& node)
{
    if (!node.is_object()) {
        return {};
    }
    // 紧凑增量形态：{"v":"文本"}（对象里只有 v 一个键）
    if (node.size() == 1 && node.contains("v") && node["v"].is_string()) {
        return node["v"].get<std::string>();
    }
    if (!node.contains("p") || !node.contains("v") || !node["v"].is_string()) {
        return {};
    }
    const std::string path = node.value("p", std::string());
    if (path.find("content") == std::string::npos) {
        return {};
    }
    if (node.contains("o")) {
        const std::string op = node.value("o", std::string());
        if (!op.empty() && op != "APPEND") {
            return {}; // 非追加操作（如 REPLACE）不叠加
        }
    }
    return node["v"].get<std::string>();
}

// SSE 负载 → 完整快照正文：{"v":{"response":{"fragments":[{"content":"…"},…]}}}
//（服务端会周期性下发完整快照；只在拿不到增量时兜底使用，避免重复拼接）
std::string snapshot_text_of(const nlohmann::json& node)
{
    if (!node.is_object() || !node.contains("v") || !node["v"].is_object()) {
        return {};
    }
    const nlohmann::json& v = node["v"];
    if (!v.contains("response") || !v["response"].is_object()) {
        return {};
    }
    const nlohmann::json& response = v["response"];
    if (!response.contains("fragments") || !response["fragments"].is_array()) {
        return {};
    }
    std::string out;
    for (const nlohmann::json& fragment : response["fragments"]) {
        if (fragment.is_object() && fragment.contains("content") &&
            fragment["content"].is_string()) {
            out += fragment["content"].get<std::string>();
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

// 建新会话：POST {session_create_path}（字段名按实测响应探测；失败返回空）
std::string create_chat_session(httplib::Client& client, const httplib::Headers& headers,
                                const std::string& path)
{
    const auto response = client.Post(path, headers, "{}", "application/json");
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

// 复用最近会话：GET {session_fetch_path}（字段名取自实测响应）
bool reuse_recent_session(httplib::Client& client, const httplib::Headers& headers,
                          const std::string& path, std::string* session_id,
                          long long* current_message_id, std::string* error)
{
    const auto response = client.Get(path, headers);
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

    // M_patchB L1（PB2-05）：站点端点全部来自 request.endpoints（默认 = 改造前写死的常量）
    const ProviderWebEndpoints& endpoints = request.endpoints;

    httplib::Client client(endpoints.host.empty() ? std::string(kHost) : endpoints.host);
    client.set_connection_timeout(15, 0);
    client.set_read_timeout(180, 0);
    client.enable_server_certificate_verification(true);

    // ---------------------------------------------------------- 1) 挑战 -----
    const httplib::Headers headers = base_headers(session, endpoints.host);
    const std::string      challenge_path = endpoints.challenge_path.empty()
                                                ? std::string(kChallengePath)
                                                : endpoints.challenge_path;
    const std::string      completion_path = endpoints.completion_path.empty()
                                                 ? std::string(kCompletionPath)
                                                 : endpoints.completion_path;
    const auto             challenge_resp =
        client.Post(challenge_path, headers,
                    nlohmann::json{{"target_path", completion_path}}.dump(),
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
    // 方案 A（默认）：交给登录窗口内的**官方 PoW worker** 求解（版本自适应、零逆向）
    // 方案 B（回退）：C++ 求解器（官方为自定义哈希，通常不会命中，仅作兜底）
    long long         attempts = 0;
    std::string       answer;
    std::string       page_error;
    // PB2-23：按**站点**求解（站点 = 本请求的端点 host；不再用无参 ensure_session → 内置默认站点）
    const long long   page_answer =
        web::solve_pow_via_page(endpoints.host, challenge_resp->body, 90000, &page_error);
    if (page_answer >= 0) {
        answer = std::to_string(page_answer);
        result.pow_attempts = 1;
        log::info("[网页版] PoW：页面内官方 worker 求解成功，answer=" + answer);
    }
    else {
        std::string solve_error;
        answer = solve_pow(challenge, &attempts, &solve_error);
        result.pow_attempts = attempts;
        log::warn("[网页版] PoW：页面内求解不可用（" + page_error + "），回退 C++ 求解器（尝试 " +
                  std::to_string(attempts) + " 次）");
        if (answer.empty()) {
            result.error = "PoW 求解失败：页面内求解不可用（" + page_error + "）；C++ 求解器：" +
                           solve_error;
            return result;
        }
    }

    // ---------------------------------------------------------- 3) 调用 -----
    httplib::Headers call_headers = headers;
    call_headers.emplace("Accept", "text/event-stream");
    call_headers.emplace("x-ds-pow-response", make_pow_header_value(challenge, answer));

    // 会话与 parent（网页版要求 chat_session_id 为 UUID）
    std::string session_id  = request.chat_session_id;
    long long   parent_id   = request.parent_message_id;
    if (session_id.empty()) {
        session_id = create_chat_session(
            client, headers,
            endpoints.session_create_path.empty() ? std::string("/api/v0/chat_session/create")
                                                  : endpoints.session_create_path);
        if (!session_id.empty()) {
            log::info("[网页版] 已新建会话（" + session_id + "）");
            parent_id = 0;
        }
        else {
            std::string session_error;
            if (!reuse_recent_session(client, headers,
                                  endpoints.session_fetch_path.empty()
                                      ? std::string("/api/v0/chat_session/fetch_page")
                                      : endpoints.session_fetch_path,
                                  &session_id, &parent_id, &session_error)) {
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
        client.Post(completion_path, call_headers, body.dump(), "application/json");
    if (!response) {
        result.error = "调用失败: " + httplib::to_string(response.error());
        return result;
    }
    result.http_status = response->status;

    // ---------------------------------------------------------- 4) 解析 -----
    std::istringstream stream(response->body);
    std::string        line;
    std::string        snapshot_text;
    int                line_index = 0;
    while (std::getline(stream, line)) {
        if (line_index < request.raw_head_lines) {
            result.raw_head += line + "\n";
        }
        ++line_index;

        const std::string trimmed = trim(line);
        const std::string payload =
            (trimmed.rfind("data:", 0) == 0) ? trim(trimmed.substr(5)) : trimmed;
        if (payload.empty() || payload == "[DONE]" || payload == "finished") {
            if (payload == "[DONE]" || payload == "finished") {
                break;
            }
            continue;
        }
        try {
            const nlohmann::json data = nlohmann::json::parse(payload);
            const std::string    delta = delta_text_of(data); // 增量（权威且有序）
            result.text += delta;
            if (!delta.empty() && request.on_delta) {
                request.on_delta(delta); // PB-03-min：边收边吐 → 执行器 → UI 逐字呈现
            }
            const std::string snapshot = snapshot_text_of(data);
            if (!snapshot.empty()) {
                snapshot_text = snapshot; // 记录最后一份完整快照
            }
        }
        catch (const std::exception&) {
            // 非 JSON 行（事件名 / 心跳）忽略
        }
    }
    // 取更完整的一份（快照通常包含完整正文；流被截断时增量更全）
    if (snapshot_text.size() > result.text.size()) {
        result.text = snapshot_text;
    }

    result.ok = (response->status == 200);
    if (!result.ok) {
        result.error =
            "HTTP " + std::to_string(response->status) + "：" + response->body.substr(0, 300);
    }
    else if (result.text.empty()) {
        result.error = "HTTP 200 但未解析出文本（请查看 raw_head 原始行以对齐解析规则）";
    }

    // ---- M_patchB L4（PB2-28④ / PB2-25）：**会话失效识别** ----
    //  * 401 / code=40002 → 必须重新登录：作废**该站点**的内存会话（面板随即显示「未登录」）
    //  * code=40003 → 请求被拒（PoW / 频率 / 前端版本）：给可操作提示，不作废会话
    const std::string failure_hint =
        web_session_failure_hint(response->status, response->body);
    if (!failure_hint.empty()) {
        if (!result.error.empty()) {
            result.error += "；";
        }
        result.error += failure_hint;
        log::warn("[网页版] " + failure_hint);
        if (web_session_failure_needs_relogin(response->status, response->body)) {
            const std::string site = web::site_key_of(request.endpoints.host);
            web::SessionStore::instance().clear(site);
            log::warn("[网页版会话] 站点 " + (site.empty() ? std::string("(默认)") : site) +
                      " 的内存会话已作废（需重新登录；Cookie 仍在浏览器 profile 里）");
        }
    }
    return result;
}

// M_patchB L4（PB2-28④ / PB2-25）：DeepSeek 网页版**会话失效**判据（纯函数，离线可断言 `VB2-27`）
//  * 只看 HTTP 状态与响应体里的错误码 —— **不**读 `userToken`（不变量 `I15` 的口径）
namespace {

// 响应体里是否出现「错误码 code」（容错：`"code":40002` / `"code": 40002` / `"biz_code":40003` 都算）
bool body_has_error_code(const std::string& body, int code)
{
    const std::string digits = std::to_string(code);
    std::size_t       pos    = body.find(digits);
    while (pos != std::string::npos) {
        const std::size_t begin  = pos > 24 ? pos - 24 : 0;
        const std::string window = body.substr(begin, pos - begin);
        if (window.find("code") != std::string::npos) {
            return true;
        }
        pos = body.find(digits, pos + digits.size());
    }
    return false;
}

} // namespace

std::string web_session_failure_hint(int http_status, const std::string& body)
{
    if (http_status == 401) {
        return "会话已失效（HTTP 401）：登录态已过期或被站点拒绝 —— 请在参数面板点"
               "「打开登录窗口（WebView2）」重新登录该站点后重试";
    }
    if (http_status == 403) {
        return "站点拒绝访问（HTTP 403）：可能触发风控 / 需要人机验证 —— 请在参数面板点"
               "「打开登录窗口（WebView2）」完成验证后重试";
    }
    if (body_has_error_code(body, 40002)) {
        return "会话已失效（code 40002）：登录态无效或已过期 —— 请在参数面板点"
               "「打开登录窗口（WebView2）」重新登录该站点后重试";
    }
    if (body_has_error_code(body, 40003)) {
        return "请求被站点拒绝（code 40003）：常见于 PoW 校验失败 / 频率限制 / 前端版本变化 —— "
               "先重新登录；若持续出现，请用 `aiwrite.exe --web-probe` 复核 challenge 与端点"
               "（M_patchB §9.4 / R19·R20）";
    }
    return {};
}

bool web_session_failure_needs_relogin(int http_status, const std::string& body)
{
    return http_status == 401 || body_has_error_code(body, 40002);
}

} // namespace aiwrite::ai
