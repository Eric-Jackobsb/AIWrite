#pragma once

// ============================================================================
//  工作流校验（M2-06 三级校验的「加载校验」与「运行前校验」）
//
//  设计 §6.3 / T-17：编辑时校验（已在 graph.cpp）/ 加载校验 / 运行前校验
//   * validateWorkflow —— 加载后或运行前的基础结构校验：
//       节点类型已注册、id 唯一、参数合法、边两端节点与端口存在、
//       端口类型兼容、非可变长输入未被重复占用、无环
//   * validateBeforeRun —— 在加载校验之上追加运行前置条件：
//       必填输入已连接（可变长输入至少 1 条）、提供商配置可用性（仅 warning）
//
//  错误文案统一形如 "[n3] 必填输入 提示词 未连接" / "[边 e3] …"，可直接展示。
// ============================================================================

#include <string>
#include <vector>

#include "engine/graph.h"

namespace aiwrite::engine {

using ValidationMessages = std::vector<std::string>;

// 加载校验；errors 可为 nullptr；返回是否全部通过（errors 为空即通过）
bool validateWorkflow(const Graph& graph, ValidationMessages* errors);

// 运行前校验；warnings 记录不阻断运行的问题（如未配置 API Key）
// 返回是否允许运行（errors 为空即允许）
bool validateBeforeRun(const Graph& graph, ValidationMessages* errors,
                       ValidationMessages* warnings);

} // namespace aiwrite::engine
