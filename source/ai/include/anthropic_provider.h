#pragma once

// ============================================================================
//  Anthropic Messages API 适配器（M_patchAB §3.1 · `PB2-10`）
//
//  协议事实（`providers.json` → `anthropic` 条目）：
//    * 端点：`{api_base}/v1/messages`
//    * 认证：`x-api-key: <key>`（**不是** `Authorization: Bearer`）+ `anthropic-version: 2023-06-01`
//    * 请求体：`{model, max_tokens, system, messages[{role, content}]}`
//      - `max_tokens` **必填**（缺省按 2048 兜底）；`temperature` 取值域 **0…1**（越界钳制）
//      - `system` 是**顶层字段**（不是 messages 里的 role）⇒ 对应 `caps.system_role=true`
//      - 图片：`content` 数组里的 `{type:"image", source:{type:"base64", media_type, data}}`
//    * 响应：`content` 数组（`{type:"text", text}` 逐块**拼接**）
//    * 错误：`{error:{type,message}}`；`seed` 不支持（`caps.seed=false`）⇒ 不发送
//
//  纯函数与联网实现分离：请求构造 / 响应解析可在 `--selftest` 里离线断言（`V-13`）。
// ============================================================================

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "inference_provider.h"

namespace aiwrite::ai {

// 端点：`{api_base}{chat_path}`，`{model}` 占位替换（表里可含 query）
std::string anthropic_build_endpoint(const std::string& api_base, const std::string& chat_path,
                                     const std::string& model);

// 请求体（`image_data_urls` = 已编码图片，按序；`send_seed` 由能力表决定）
nlohmann::json anthropic_build_body(const GenerateParams& params, const std::string& model,
                                    bool send_seed,
                                    const std::vector<std::string>& image_data_urls);

// 响应文本：`content[*].text`（type == "text"）逐块拼接
std::string anthropic_extract_text(const nlohmann::json& body);

// 响应里的可操作错误（`error.message`；另含 `stop_reason` 说明）——空串 = 未识别
std::string anthropic_error_text(int http_status, const nlohmann::json& body);

// 实现（`protocol = "anthropic"`）
class AnthropicProvider final : public InferenceProvider {
public:
    explicit AnthropicProvider(ProviderRuntimeConfig config);
    std::string    name() const override;
    std::string    protocol() const override;
    ProviderCaps   caps() const override;
    GenerateResult generate(const GenerateParams& params) override;

private:
    ProviderRuntimeConfig config_;
    ProviderCaps          caps_ = permissive_caps();
};

} // namespace aiwrite::ai
