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

#include <memory>
#include <string>
#include <vector>

#include "ai/provider_spec.h"   // M_patchB L1：生效条目（表驱动）
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

    // ---- M_patchB L1（PB2-05）：配置表驱动的生效值 ----
    // specs 持有快照，保证 spec 指针在「重新加载配置表」后仍有效（不悬垂）
    std::shared_ptr<const ai::ProviderSpecs> specs;
    const ai::ProviderSpec* spec  = nullptr;   // 生效条目（未命中 = nullptr）
    std::string             kind;              // official | web（未命中 = 空）
    std::string             display;           // 显示名（未命中 = provider id）
    std::string             api_base;          // 生效地址（节点参数优先；空 → 表默认）
    std::string             key_ref;           // 生效凭据引用名（节点参数优先；空 → 表默认）
    bool                    key_required = true; // auth_style=none 时无需 Key
    std::string             spec_origin;       // 条目来源（builtin / user / …）

    bool        is_web() const { return kind == "web"; }
    std::string model_hint() const;            // 表内候选模型（用于界面提示）
};

// 是否使用 provider 句柄的推理节点（LLMGenerate / VLMGenerate）
bool uses_provider(const std::string& node_type);

// 解析节点生效的提供商配置（provider 输入优先；仅对推理节点有意义）
EffectiveProvider resolve_effective_provider(const Graph& graph, const Node& node);

// 解析节点**自身参数**的生效提供商配置（不理会 provider 连线）
//  * 用途：「提供商配置」节点**本身就是配置来源** —— 参数面板 / 运行前校验必须按**它自己的条目**解析
//  * M_patchB L2 修订（PB2-20）：这两处此前误用 resolve_effective_provider()，而该函数对
//    非推理节点恒返回默认构造（spec = nullptr）→ 站点区/提示永远回落内置默认（见 §9.1）
EffectiveProvider resolve_self_provider(const Node& node);

// 界面 / 校验统一入口（PB2-20）：
//  * 推理节点（LLMGenerate / VLMGenerate）→ resolve_effective_provider（provider 连线优先）
//  * 其他节点（ProviderConfig 等）        → resolve_self_provider（按自身参数）
EffectiveProvider resolve_display_provider(const Graph& graph, const Node& node);

// 是否处于「官方 API 尚未接线」的必定失败组合（mode = official）
bool official_not_wired(const EffectiveProvider& provider);

// M_patchB L1 续（PB2-17）+ L2 修订（PB2-20 / 决策 D-21）：「模式」可选项 —— **恒为 {official, web}**
//  * **网页版与官方 API 同等优先级**：候选**不得**按条目 `kind` 裁剪（不变量 I13）
//  * `kind` 只影响 ① 切换提供商时的建议值（provider_mode_suggestion）② 不一致时的提示（mode_kind_hint）
//  * 入参保留 = 单点开关（将来若产品需要收窄，只改这一处）
std::vector<std::string> provider_mode_options(const Node& node);

// 「模式」的**建议值**（按当前「提供商」条目的 `kind`；表里没有该 id / kind 未知 → 空串）
//  * 仅供「切换提供商」时带出一次建议；**不**静默改写用户已选的值（D-21）
std::string provider_mode_suggestion(const Node& node);

// 「提供商」条目 `kind` 与「模式」不一致时的**提示文案**（空串 = 一致 / 无提示）
//  * official 条目 + `web`   → 将使用**内置默认站点**（DeepSeek 网页版）
//  * web 条目 + `official`   → 该条目**没有官方 API 通道**（按表内 `api_base` 解析，缺失会明确报错）
//  * 只提示、不改写：参数面板按橙色显示；运行前校验按 note 输出（D-21 / I13）
std::string mode_kind_hint(const EffectiveProvider& provider);

// 该节点「本次运行必定失败」的原因（空串 = 未发现已知必定失败）
//  * LLMGenerate + 生效 mode=official + 无 Key → 缺少 API Key（PB-05/PB-06）
//  * VLMGenerate  + 生效 mode=web             → 图片理解暂不支持网页版（M5-02）
std::string unwired_reason(const Graph& graph, const Node& node);

} // namespace aiwrite::engine
