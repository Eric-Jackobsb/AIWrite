#pragma once

// ============================================================================
//  官方 DeepSeek API Provider（PB-05 / 原 M4-05；落地“API 使用”）
//
//  * POST {api_base}/chat/completions（OpenAI 兼容）
//  * 纯函数 build_endpoint / build_request_body / resolve_api_key → 供离线自检（V-11）
//  * 阻塞调用；UI 场景由执行器的工作线程承载（PB-01 已线程化）
//  * API Key 优先级：节点参数 → 环境变量 DEEPSEEK_API_KEY（凭据管理器见 PB-06）
//  * 官方 SSE 流式（stream=true）随“实时返回”计划暂停（见 M_patchA PB-03/PB-08）
//
//  M5-02（多模态）：`images` 非空时 user content 变为数组
//  （`[{type:"text",text},{type:"image_url",image_url:{url:"data:<mime>;base64,..."}}]`），
//  即 OpenAI 兼容的视觉请求格式（智谱 GLM 视觉系列 / 硅基流动 / 本地 Ollama 通用）。
//  图片路径编码为 data URL 的纯函数 `encode_image_data_url` 同样供离线断言使用。
// ============================================================================

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ai/provider_spec.h"   // M_patchB L1 / PB2-04：按表取端点·认证·env·超时

namespace aiwrite::ai {

// 请求参数化（M_patchB L1 / PB2-04）：端点 / 认证 / env 名 / 超时**全部来自配置表**
//  * 默认值 = 改造前的内置行为（DeepSeek 官方 API），因此「不传 options」时行为逐字不变
struct ProviderOptions {
    std::string api_base;                     // 空 = 用 OfficialChatRequest.api_base
    std::string chat_path = "/chat/completions";
    std::string auth_style = "bearer";        // bearer | api-key | x-api-key | query | none
    std::string auth_header;                  // 空 = 按 auth_style 默认
    std::vector<std::pair<std::string, std::string>> extra_headers;
    std::vector<std::string> env_names;       // 空 = {"DEEPSEEK_API_KEY"}
    int connect_timeout_s = 15;
    int read_timeout_s    = 180;
};

// 由配置表条目生成请求参数（纯函数；节点 / 自检共用）
ProviderOptions provider_options_from(const ProviderSpec& spec);

struct OfficialChatRequest {
    std::string api_base = "https://api.deepseek.com";
    std::string api_key;   // 已解析；为空时调用方应先报错
    std::string model     = "deepseek-chat";
    std::string system_prompt;
    std::string prompt;
    // M5-02：图片输入（按序，本地路径）；非空 → 走多模态 content 数组
    std::vector<std::string> images;
    // 单张图片上限（0 = 不限制）；超限报可操作错误，不做隐式压缩
    std::size_t image_max_bytes = 8u * 1024u * 1024u;
    double      temperature = 0.7;
    int         max_tokens  = 2048;
    double      top_p       = 1.0;
    int         seed        = 0; // M_rerun：>0 时随请求发送（0 = 不指定）
    // M_patchB L1：请求参数（端点 / 认证 / env / 超时）；默认值 = 改造前行为
    ProviderOptions options;
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
// M_patchB L1 / PB2-04：可指定请求路径（如 /v1/messages、含 {model} 占位）
std::string    build_endpoint(const std::string& api_base, const std::string& chat_path);
std::string    resolve_api_key(const std::string& param_key);
// M_patchB L1 / PB2-04：环境变量名按序尝试（空列表 = 仅 DEEPSEEK_API_KEY）
std::string    resolve_api_key(const std::string& param_key,
                               const std::vector<std::string>& env_names);
// M_patchB L1 / PB2-04：按认证风格生成认证头（bearer/api-key/x-api-key/none）
std::vector<std::pair<std::string, std::string>> build_auth_headers(const ProviderOptions& options,
                                                                    const std::string& api_key);
// M_patchB L1 / PB2-04：把 {model} 占位替换为实际模型名
std::string    resolve_chat_path(const std::string& chat_path, const std::string& model);

// M7-05：内容（魔数）→ MIME；认不出来返回空串
std::string    image_mime_from_bytes(const unsigned char* data, std::size_t size);
// M5-02 / M7-05：路径 → MIME（**内容优先**，扩展名回退；未知扩展名仍是 image/png）
std::string    image_mime_from_path(const std::string& path);
// M5-02：读文件 → "data:<mime>;base64,<...>"；失败返回空串并把原因写入 *error
std::string    encode_image_data_url(const std::string& path, std::size_t max_bytes,
                                     std::string* error);

nlohmann::json build_request_body(const OfficialChatRequest& request);
// M5-02：已编码图片版本的请求体（official_chat 用它避免重复编码；失败图不应出现在列表里）
nlohmann::json build_request_body(const OfficialChatRequest& request,
                                  const std::vector<std::string>& image_data_urls);

// 实际调用（阻塞）
OfficialChatResult official_chat(const OfficialChatRequest& request);

} // namespace aiwrite::ai
