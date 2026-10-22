#pragma once

// ============================================================================
//  Gemini generateContent 适配器（M_patchAB §3.1 · `PB2-11`）
//
//  协议事实（`providers.json` → `gemini` 条目）：
//    * 端点：`{api_base}/v1beta/models/{model}:generateContent`（**`{model}` 占位**）
//    * 认证：查询参数 `?key=<key>`（`auth_style=query`）—— **不是** 请求头
//    * 请求体：`{contents:[{role:"user",parts:[{text},{inline_data:{mime_type,data}}]}],
//               systemInstruction:{parts:[{text}]}, generationConfig:{...}}`
//      - `generationConfig`：`temperature` / `maxOutputTokens` / `topP` / `seed`
//      - 图片：`parts[].inline_data`（base64，**不带** data URL 前缀）
//    * 响应：`candidates[0].content.parts[*].text` 逐块拼接
//    * 被安全策略挡住时：`promptFeedback.blockReason`（**取不到文本但要给可操作原因**）
//
//  纯函数与联网实现分离：请求构造 / 响应解析可在 `--selftest` 里离线断言（`V-13`）。
// ============================================================================

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "inference_provider.h"

namespace aiwrite::ai {

// 端点：`{api_base}{chat_path}`（`{model}` 替换 + `?key=` 拼接；空 Key ⇒ 不拼 query）
std::string gemini_build_endpoint(const std::string& api_base, const std::string& chat_path,
                                  const std::string& model, const std::string& auth_name,
                                  const std::string& api_key);

// 请求体（`image_data_urls` = 已编码图片，按序）
nlohmann::json gemini_build_body(const GenerateParams& params, bool send_seed,
                                 const std::vector<std::string>& image_data_urls);

// 响应文本：`candidates[0].content.parts[*].text` 逐块拼接（可指定第几个候选）
std::string geminiExtractText(const nlohmann::json& body, std::size_t candidate = 0);

// 无文本时的可操作原因（`promptFeedback.blockReason` / `finishReason` / `error.message`）
std::string geminiBlockReason(const nlohmann::json& body);

// 实现（`protocol = "gemini"`）
class GeminiProvider final : public InferenceProvider {
public:
    explicit GeminiProvider(ProviderRuntimeConfig config);
    std::string    name() const override;
    std::string    protocol() const override;
    ProviderCaps   caps() const override;
    GenerateResult generate(const GenerateParams& params) override;

private:
    ProviderRuntimeConfig config_;
    ProviderCaps          caps_ = permissiveCaps();
};

} // namespace aiwrite::ai
