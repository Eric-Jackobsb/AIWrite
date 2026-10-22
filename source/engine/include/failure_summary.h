#pragma once

// ============================================================================
//  失败汇总（DEBT-L8 / M6-04 步骤 5）：运行结束后**汇总所有失败节点**，
//  而不是只显示首个（既有错误条现状）。纯函数 —— UI 与离线断言共用。
//
//  * `collectFailures`   —— 按图序收集所有 `NodeState::Error` 节点；
//  * `summarizeFailures` —— 汇总文案：
//       单失败 → "1 个节点失败：n1（错误信息）"
//       多失败 → "N 个节点失败：n1（…）；n2（…）"
//       无失败 → 空串
// ============================================================================

#include "graph.h"

#include <string>
#include <vector>

namespace aiwrite::engine {

// 单个失败节点的汇总条目（顺序 = 图序）
struct FailureSummary {
    std::string node_id;
    std::string message; // error_message；空 → "执行失败"
};

// 收集所有 Error 状态节点（按图序）；无失败返回空
std::vector<FailureSummary> collectFailures(const Graph& graph);

// 汇总文案（见头注释）；无失败返回空串
std::string summarizeFailures(const Graph& graph);

} // namespace aiwrite::engine
