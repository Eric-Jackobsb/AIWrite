#pragma once

// ============================================================================
//  通用 JSON POST（M_patchAB §3.1 · `B2-b` L2 底座）
//
//  动机：Anthropic（Messages）/ Gemini（generateContent）与既有 OpenAI 兼容协议**共用**同一套
//  「带上凭证 → POST JSON → 读响应」的传输语义；把它抽到一处，保证三条协议在
//  **取消 / 超时 / 错误归因** 上口径完全一致（沿用 `PB-02` 的既有结论：取消 ≠ 站点错误）。
//
//  设计约束：
//   * 本文件**不含任何厂商字段** —— 请求体构造在各自协议适配器里（纯函数，可离线断言 `V-13`）
//   * `http_post_json` 走 `run_cancellable`（工作线程 + 调用方轮询令牌）⇒ 取消后**立即返回**
//     （实测：`httplib 0.15.3` 的 `Client::stop()` 不能唤醒阻塞读，见 `loop_dev` 陷阱 11）
//   * `auth_style=query` 的 Key 拼接（Gemini `?key=`）由纯函数 `append_query_param` 提供
//   * 头文件**不暴露 httplib**（实现细节留在 .cpp）
// ============================================================================

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cancel_token.h" // `PB-02`：协作式取消
#include "http_net.h"      // M_patchAB §2.3（PB-06 剩余）：代理 / 自签证书

namespace aiwrite::ai {

struct HttpJsonRequest {
    std::string                                      url; // 完整 URL（含 query）
    std::vector<std::pair<std::string, std::string>> headers;
    std::string                                      body; // JSON 文本（调用方自行 dump）
    int         connect_timeout_s = 15;
    int         read_timeout_s    = 180;
    std::size_t max_body_bytes    = 4u * 1024u * 1024u; // 响应体上限（防御异常大响应）
    std::shared_ptr<const CancelToken> cancel;
    // M_patchAB §2.3（`PB-06` 剩余）：网络策略**显式覆盖**（缺省 = 由配置 + 环境自动解析）
    //  * 用途：自检 / 工具需要**确定性**（不受开发机 `HTTPS_PROXY` / `NO_PROXY` 干扰）
    std::optional<HttpNetOptions> net;
};

struct HttpJsonResult {
    bool        ok          = false; // 传输成功（**不代表** 2xx；状态码看 http_status）
    bool        cancelled   = false; // 因取消而中断（取消 ≠ 站点 / 网络错误）
    int         http_status = 0;
    std::string body;      // 响应体原文（超上限时已截断）
    std::string raw_head;  // 响应前若干字符（诊断：解析失败时看清站点到底回了什么）
    std::string error;     // 传输层错误（可操作文案；ok=false 时非空）
    double      elapsed_ms = 0.0;
};

// 阻塞 POST（**可取消**）：拿到 HTTP 响应即 ok=true，具体状态码由调用方判定
HttpJsonResult http_post_json(const HttpJsonRequest& request);

// ---- 纯函数（离线断言用）----

// URL 追加 query 参数（已有 `?` ⇒ 用 `&`）；`name` / `value` 为空 ⇒ 原样返回
std::string append_query_param(const std::string& url, const std::string& name,
                               const std::string& value);

// 解析 JSON 文本：失败 ⇒ false + 可操作 error（空串 / 非 JSON 各有明确文案）
bool parse_json_text(const std::string& text, nlohmann::json* out, std::string* error);

// 从错误响应里取**可读消息**（`error.message` → `message` → `error` 字符串 → 空串）
//  * 兼容三种形状：OpenAI 兼容 `{error:{message}}` / Anthropic `{error:{type,message}}`
//    / Gemini `{error:{code,message,status}}`
std::string json_error_message(const nlohmann::json& body);

} // namespace aiwrite::ai
