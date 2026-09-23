#pragma once

// ============================================================================
//  节点执行实现（设计 §9.2；M2-04 起）
//
//  * 每个节点类型一个执行函数，签名见 engine::NodeExecutor（设计 §9.1）
//  * M2 落地：TextInput / ImageInput / PromptTemplate / TextMerge / ProviderConfig /
//    TextOutput / ImagePreview 共 7 个本地实现
//  * 占位：LLMGenerate / VLMGenerate 抛 NodeError（等待 M4-05 官方 API / M4-06 网页版接线）
//    —— 正好用于验证「节点失败 → 下游 Skipped」的失败传播（设计 §10.4）
//  * 入口点（app / api_probe）需在运行前调用 registerAllExecutors()（幂等）
// ============================================================================

#include <nlohmann/json.hpp>

#include "engine/executor.h"

namespace aiwrite::nodes {

nlohmann::json execute_text_input(const nlohmann::json& inputs, const nlohmann::json& params,
                                 engine::ExecutionContext& ctx);
nlohmann::json execute_image_input(const nlohmann::json& inputs, const nlohmann::json& params,
                                  engine::ExecutionContext& ctx);
nlohmann::json execute_prompt_template(const nlohmann::json& inputs, const nlohmann::json& params,
                                      engine::ExecutionContext& ctx);
nlohmann::json execute_text_merge(const nlohmann::json& inputs, const nlohmann::json& params,
                                 engine::ExecutionContext& ctx);
nlohmann::json execute_provider_config(const nlohmann::json& inputs, const nlohmann::json& params,
                                      engine::ExecutionContext& ctx);
nlohmann::json execute_text_output(const nlohmann::json& inputs, const nlohmann::json& params,
                                  engine::ExecutionContext& ctx);
nlohmann::json execute_image_preview(const nlohmann::json& inputs, const nlohmann::json& params,
                                    engine::ExecutionContext& ctx);
nlohmann::json execute_llm_generate(const nlohmann::json& inputs, const nlohmann::json& params,
                                   engine::ExecutionContext& ctx);
nlohmann::json execute_vlm_generate(const nlohmann::json& inputs, const nlohmann::json& params,
                                   engine::ExecutionContext& ctx);

// 注册全部节点执行函数（幂等；覆盖同名注册）
void registerAllExecutors();

} // namespace aiwrite::nodes
