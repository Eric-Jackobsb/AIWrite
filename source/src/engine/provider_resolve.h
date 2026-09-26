#pragma once

// ============================================================================
//  生效提供商解析（P1-a / P1-b）
//
//  设计 §9.2：ProviderConfig 输出 provider 句柄 → 推理节点的 provider 输入。
//  生效规则（与 nodes/local_nodes.cpp 的取值顺序**完全一致**，界面与执行共用同一函数）：
//    1) provider 输入已连接 → 取所连 ProviderConfig 节点的 provider/mode/model（**覆盖**节点自身）
//    2) 未连接 → 取节点自身参数（LLMGenerate 默认 mode=web）
//
//  用途：
//    * 参数面板 / 画布节点体：显示「生效模式 + 来源」，避免"改了节点模式却不生效"的困惑
//    * 工具栏运行前提示：official（官方 API，PB-04/PB-05 未接线）分支本次运行必定失败
//
//  纯模型层：不依赖 ImGui，可被界面与自检共用。
// ============================================================================

#include <string>

#include "engine/graph.h"

namespace aiwrite::engine {

// 生效中的提供商配置
struct EffectiveProvider {
    bool        resolved  = false;            // 是否解析到配置（仅推理节点会解析）
    bool        from_edge = false;            // true = 来自 provider 输入连线（覆盖节点自身参数）
    std::string source_node;                  // 来源节点 id（from_edge 为 true 时有效）
    std::string provider = "deepseek";
    std::string mode     = "official";
    // M5-02：`model` 恒为**生效模型名**（`model_custom` 非空时覆盖枚举 model）
    std::string model        = "deepseek-chat";
    std::string model_custom;                 // 自定义模型名（空 = 未覆盖，用枚举/默认值）
};

// 是否使用 provider 句柄的推理节点（LLMGenerate / VLMGenerate）
bool uses_provider(const std::string& node_type);

// 解析节点生效的提供商配置（provider 输入优先）
EffectiveProvider resolve_effective_provider(const Graph& graph, const Node& node);

// 是否处于「官方 API 尚未接线」的必定失败组合（mode = official）
bool official_not_wired(const EffectiveProvider& provider);

// 该节点「本次运行必定失败」的原因（空串 = 未发现已知必定失败）
//  * LLMGenerate + 生效 mode=official + 无 Key → 缺少 API Key（PB-05/PB-06）
//  * VLMGenerate  + 生效 mode=web             → 图片理解暂不支持网页版（M5-02）
std::string unwired_reason(const Graph& graph, const Node& node);

} // namespace aiwrite::engine
