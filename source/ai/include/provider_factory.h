#pragma once

// ============================================================================
//  Provider 工厂（M_patchAB §3.1 · `B2-b` L2 底座 · `PB2-09`）
//
//  **按表 `protocol` 分派**（协议 = 数据；表里换一行 = 换一条链路）：
//    * `openai`       → `OpenAiCompatProvider`（deepseek / zhipu / siliconflow / ollama / …）
//    * `anthropic`    → `AnthropicProvider`（Messages API）
//    * `gemini`       → `GeminiProvider`（generateContent）
//    * `deepseek-web` → `DeepSeekWebProvider`（内置网页版：PoW + SSE，收编既有 `web_chat()`）
//    * `dom`          → **明确报错**（DOM 生成由 `dom_chat` 承载：需要页面上下文，不经本工厂）
//    * 其它 / 未知    → **明确报错**（`R9`：绝不静默回落到别的协议）
//
//  风险对策（归档 `M_patchB` §5）：
//    * `R9` 表与代码能力脱节 ⇒ 未知 protocol **明确报错** + `--provider-selftest` 逐条核对
//    * `R12` adapter 不支持字段 ⇒ 各协议实现只在**自身**请求体里取用相关字段（不静默降级）
// ============================================================================

#include <memory>
#include <string>
#include <vector>

#include "inference_provider.h"

namespace aiwrite::ai {

struct ProviderCreation {
    std::unique_ptr<InferenceProvider> provider;             // 成功时非空
    std::string                        error;                // 失败原因（可操作；空 = 成功）
    bool unsupported_protocol = false;                       // true = 该 protocol 本版本未实现
};

// 建 provider（`config.provider_id` / `display` 会自动对齐到 `spec`，能力快照取自本条）
ProviderCreation create_provider(const ProviderSpec& spec, ProviderRuntimeConfig config);

// 本工厂是否实现该 protocol（纯判断；自检 / 提示 / R9 核对共用）
bool factory_implements(const std::string& protocol);

// 本工厂实现的 protocol 清单（顺序 = 分派顺序）
std::vector<std::string> factory_protocols();

// 分派顺序的展示文本（例 `openai` → `anthropic` → …）
std::string factory_protocols_text();

} // namespace aiwrite::ai
