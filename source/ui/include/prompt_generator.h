#pragma once

// ============================================================================
//  提示词生成（M_8nodes_extension L1-d · §4.3）：PromptTemplate「生成提示词」按钮的异步后端
//
//  * 线程模型同 connection_test：**工作线程**执行 AI 调用（界面不阻塞）→ 结果存入共享状态，
//    主线程（参数面板）**每帧轮询消费**（`prompt_generation_consume`），写回 `template` 前压撤销快照。
//  * AI 配置 = 节点自身参数（复用 N-05 解析：`ai::resolve_api_base` / `resolve_model` /
//    `resolve_key_ref` + 凭据库）—— **无需额外连线**（文档 §4.3）。
//  * 通道：本版**官方 / 兼容 API**（official_chat）；`mode=web` 时按钮提示「暂不支持生成」。
// ============================================================================

#include <nlohmann/json.hpp>

#include <string>

namespace aiwrite::ui {

// 请求生成（异步）：`params` 为节点参数的**快照**（生成线程只读，不碰 UI 线程的 graph）
void requestPromptGeneration(const std::string& node_id, const nlohmann::json& params);

// 正在生成？（面板据此显示「正在生成…」并禁用按钮）
bool promptGenerationRunning();

// 主线程消费：若最近一次结果属于 `node_id` → 取出 result / error（成功 result 非空）并清空；
// 返回 true 表示有结果可取。
bool promptGenerationConsume(const std::string& node_id, std::string* result, std::string* error);

// 退出前收尾：等待生成线程结束（同 stop_diagnostics 口径）
void stopPromptGeneration(int wait_ms = 4000);

} // namespace aiwrite::ui
