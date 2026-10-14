#include "http_json.h"

#include "http_net.h" // M_patchAB §2.3（PB-06 剩余）：代理 / 自签证书
#include "log.h"

#include <chrono>
#include <mutex>

#include <httplib.h>

namespace aiwrite::ai {
namespace {

// 响应前若干字符（单行化）—— 解析失败时用它回答「站点到底回了什么」
std::string head_of(const std::string& text, std::size_t limit = 240)
{
    std::string out = text.substr(0, limit);
    for (char& ch : out) {
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            ch = ' ';
        }
    }
    if (text.size() > limit) {
        out += "…";
    }
    return out;
}

} // namespace

std::string append_query_param(const std::string& url, const std::string& name,
                               const std::string& value)
{
    if (name.empty() || value.empty()) {
        return url;
    }
    std::string out = url;
    out += (out.find('?') == std::string::npos) ? '?' : '&';
    out += name + "=" + value;
    return out;
}

bool parse_json_text(const std::string& text, nlohmann::json* out, std::string* error)
{
    if (out == nullptr) {
        return false;
    }
    if (text.empty()) {
        if (error != nullptr) {
            *error = "响应为空（站点未返回正文；请检查地址 / 凭证 / 网络）";
        }
        return false;
    }
    try {
        *out = nlohmann::json::parse(text);
        return true;
    }
    catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = std::string("响应不是合法 JSON：") + ex.what() +
                     "（原文前 240 字：" + head_of(text) + "）";
        }
        return false;
    }
}

std::string json_error_message(const nlohmann::json& body)
{
    if (body.is_object()) {
        const auto error_it = body.find("error");
        if (error_it != body.end()) {
            if (error_it->is_object()) {
                const auto message_it = error_it->find("message");
                if (message_it != error_it->end() && message_it->is_string()) {
                    return message_it->get<std::string>();
                }
                const auto type_it = error_it->find("type");
                if (type_it != error_it->end() && type_it->is_string()) {
                    return type_it->get<std::string>();
                }
            }
            if (error_it->is_string()) {
                return error_it->get<std::string>();
            }
        }
        const auto message_it = body.find("message");
        if (message_it != body.end() && message_it->is_string()) {
            return message_it->get<std::string>();
        }
        // Gemini：`{"error":{"code":400,"message":"...","status":"INVALID_ARGUMENT"}}`
        const auto status_it = body.find("status");
        if (status_it != body.end() && status_it->is_string()) {
            return status_it->get<std::string>();
        }
    }
    else if (body.is_string()) {
        return body.get<std::string>();
    }
    return {};
}

HttpJsonResult http_post_json(const HttpJsonRequest& request)
{
    HttpJsonResult result;
    if (request.url.empty()) {
        result.error = "请求地址为空（请检查配置表 api_base / chat_path）";
        return result;
    }

    // ---- `PB-02` 真取消：**可取消执行**（主路径）----
    //  * 与 `official_chat` 同一套语义：工作线程跑 HTTP，调用方轮询令牌立即返回
    //  * ⚠️ `job` 里的东西由工作线程自持（值语义）—— 取消后本函数已返回，不得引用栈上对象
    struct HttpJob {
        std::string      url;
        httplib::Headers headers;
        std::string      body;
        int              connect_timeout_s = 15;
        int              read_timeout_s    = 180;
        std::size_t      max_body_bytes    = 4u * 1024u * 1024u;
        // M_patchAB §2.3（PB-06 剩余）：代理 / 证书校验（由调用方按配置 + 环境解析后传入）
        std::string      proxy;
        bool             verify_tls        = true;
        std::mutex       mutex;
        httplib::Result  response{nullptr, httplib::Error::Success, httplib::Headers{}};
        double           elapsed_ms = 0.0;
    };
    auto job = std::make_shared<HttpJob>();
    job->url = request.url;
    for (const std::pair<std::string, std::string>& header : request.headers) {
        job->headers.emplace(header.first, header.second);
    }
    job->body              = request.body;
    job->connect_timeout_s = request.connect_timeout_s > 0 ? request.connect_timeout_s : 15;
    job->read_timeout_s    = request.read_timeout_s > 0 ? request.read_timeout_s : 180;
    job->max_body_bytes =
        request.max_body_bytes > 0 ? request.max_body_bytes : (4u * 1024u * 1024u);
    // M_patchAB §2.3（PB-06 剩余）：网络策略在主线程解析（读进程配置缓存），**值语义**进作业对象
    const HttpNetOptions net_options =
        request.net.has_value() ? *request.net : resolve_http_net(request.url);
    job->proxy         = net_options.proxy;
    job->verify_tls    = net_options.verify_tls;

    const auto started = std::chrono::steady_clock::now();
    const bool aborted = run_cancellable(request.cancel.get(), [job] {
        // URL 自带 query（Gemini `?key=`）⇒ 按「scheme://host + path?query」切开交给 httplib
        const std::size_t scheme_end = job->url.find("://");
        const std::size_t host_begin = (scheme_end == std::string::npos) ? 0 : scheme_end + 3;
        const std::size_t path_begin = job->url.find('/', host_begin);
        const std::string host =
            job->url.substr(0, path_begin == std::string::npos ? job->url.size() : path_begin);
        const std::string path =
            (path_begin == std::string::npos) ? std::string("/") : job->url.substr(path_begin);

        httplib::Client client(host);
        client.set_connection_timeout(job->connect_timeout_s, 0);
        client.set_read_timeout(job->read_timeout_s, 0);
        apply_http_net(client, HttpNetOptions{job->proxy, std::string(), job->verify_tls});
        const auto begin    = std::chrono::steady_clock::now();
        auto       response = client.Post(path, job->headers, job->body, "application/json");
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                .count();
        std::lock_guard<std::mutex> lock(job->mutex);
        job->response   = std::move(response);
        job->elapsed_ms = ms;
    });
    if (aborted) {
        result.cancelled = true;
        result.error     = "已取消（用户点了「停止」）：请求被中断（已立即返回；"
                           "后台线程继续收尾，结果丢弃）";
        log::info("[HTTP] 已取消：请求被中断（立即返回）");
        return result;
    }

    httplib::Result response{nullptr, httplib::Error::Success, httplib::Headers{}};
    double          elapsed_ms = 0.0;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        response   = std::move(job->response);
        elapsed_ms = job->elapsed_ms;
    }
    result.elapsed_ms =
        elapsed_ms > 0.0
            ? elapsed_ms
            : std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                  .count();
    if (!response) {
        result.error = std::string("调用失败（网络/超时/DNS）：") +
                       httplib::to_string(response.error());
        return result;
    }
    result.ok          = true;
    result.http_status = response->status;
    result.body        = response->body.size() > job->max_body_bytes
                             ? response->body.substr(0, job->max_body_bytes)
                             : response->body;
    result.raw_head    = head_of(result.body);
    return result;
}

} // namespace aiwrite::ai
