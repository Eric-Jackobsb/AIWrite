#pragma once

// ============================================================================
//  DeepSeek 网页版客户端（M4-06 / M4-08；复用 web::Session 的内存凭证）
//
//  流程：取 PoW 挑战 → C++ 求解（SHA3-256）→ POST /api/v0/chat/completion
//        headers: Authorization: Bearer <userToken> + Cookie + x-ds-pow-response
//        响应：text/event-stream（SSE，逐行 `data: {...}`）
//
//  说明：`raw_head` 会带回原始响应前若干行 —— 前端格式随版本变化时用它对齐解析规则。
// ============================================================================

#include <functional>
#include <string>

#include "ai/provider_spec.h"   // M_patchB L1（PB2-05）：站点端点来自配置表
#include "web/session_store.h"

namespace aiwrite::ai {

struct WebChatRequest {
    std::string prompt;
    std::string model_type       = "default"; // default / expert / ...（随网页版前端版本）
    bool        thinking_enabled = false;     // 深度思考
    bool        search_enabled   = false;     // 联网搜索
    int         raw_head_lines   = 12;        // 诊断：保留原始前 N 行
    // PB-03（最小化）：每解析到一段 SSE 增量就回调一次（UI 逐字呈现）；为空则只在结束时返回全文
    std::function<void(const std::string&)> on_delta;
    // 可选覆盖：为空时自动「新建会话（失败则复用最近会话）」
    std::string chat_session_id;
    long long   parent_message_id = 0;        // 0 = 使用会话的 current_message_id
    // M_patchB L1（PB2-05）：站点端点 —— 默认值 = 改造前写死的 DeepSeek 常量（行为不变）
    //  由 effective 配置表条目填入（`web.endpoints`），站点换域名/换 API 版本时改 JSON 即可
    ProviderWebEndpoints endpoints;
};

struct WebChatResult {
    bool        ok           = false;
    int         http_status  = 0;
    std::string text;         // 拼接出的生成文本
    std::string raw_head;     // 原始响应前 N 行（诊断）
    long long   pow_attempts = 0;
    std::string error;
};

// 用内存会话调用网页版生成；不落盘、不写日志明文（日志只记长度与状态）
WebChatResult web_chat(const web::Session& session, const WebChatRequest& request);

} // namespace aiwrite::ai
