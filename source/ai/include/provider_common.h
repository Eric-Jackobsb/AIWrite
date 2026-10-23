// ARCH-Z1 去重专项（v1.14）：Anthropic / Gemini 共用「图片预处理 + HTTP 失败归因」片段
//  * 两家 wire 字段不同，但图片编码口径 / 失败归因 / 状态回填**完全一致** ⇒ 收在一处
//  * 只允许 `ai/src/anthropic_provider.cpp` / `ai/src/gemini_provider.cpp` include；**不是**公开 API
//  * 行为与改造前逐字一致（错误文案前缀由 `provider_label` 传入）
#pragma once

#include "http_json.h"          // `HttpJsonResult`
#include "inference_provider.h" // `GenerateParams` / `GenerateResult`

#include <string>
#include <vector>

namespace aiwrite::ai {

// 逐张编码 data URL：失败 ⇒ 填 `result->error`（可操作原因）并返回 false，调用者直接 `return *result`
//  * 成功累加 `result->encoded_bytes`（上传体积诊断）
bool prepare_image_data_urls(const GenerateParams& params, std::vector<std::string>* urls,
                             GenerateResult* result);

// HTTP 失败归因：回填 `http_status / elapsed_ms / raw_head`；
//  * 取消 ⇒ `cancelled = true`；传输失败 ⇒ `error = "<label> 调用失败：…"`
//  * 返回 true 表示已填 `result`，调用者应立即返回
bool map_http_failure(const HttpJsonResult& http, const std::string& provider_label,
                      GenerateResult* result);

}  // namespace aiwrite::ai
