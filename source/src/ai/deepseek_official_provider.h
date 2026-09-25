#pragma once

// ============================================================================
//  官方 DeepSeek API Provider（PB-05 / 原 M4-05；落地“API 使用”）
//
//  * POST {api_base}/chat/completions（OpenAI 兼容）
//  * 纯函数 build_endpoint / build_request_body / resolve_api_key → 供离线自检（V-11）
//  * 阻塞调用；UI 场景由执行器的工作线程承载（PB-01 已线程化）
//  * API Key 优先级：节点参数 → 环境变量 DEEPSEEK_API_KEY（凭据管理器见 PB-06）
//  * 官方 SSE 流式（stream=true）随“实时返回”计划暂停（见 M_patchA PB-03/PB-08）
// ============================================================================

#include <string>

#include <nlohmann/json.hpp>

namespace aiwrite::ai {

struct OfficialChatRequest {
    std::string api_base = "https://api.deepseek.com";
    std::string api_key;   // 已解析；为空时调用方应先报错
    std::string model     = "deepseek-chat";
    std::string system_prompt;
    std::string prompt;
    double      temperature = 0.7;
    int         max_tokens  = 2048;
    double      top_p       = 1.0;
    int         seed        = 0; // M_rerun：>0 时随请求发送（0 = 不指定）
};

struct OfficialChatResult {
    bool        ok          = false;
    int         http_status = 0;
    std::string text;
    std::string error;
    std::string raw_head;
    double      elapsed_ms = 0.0;
};

// ---- 纯函数（离线断言用）----
std::string    build_endpoint(const std::string& api_base);
std::string    resolve_api_key(const std::string& param_key);
nlohmann::json build_request_body(const OfficialChatRequest& request);

// 实际调用（阻塞）
OfficialChatResult official_chat(const OfficialChatRequest& request);

} // namespace aiwrite::ai
