#pragma once

#include <string>
#include <vector>

#include "graph.h"

// ============================================================================
//  参数面板内部共享接口（CLN-12 拆分 · 2026-10-20）
//
//  仅供 property_panel.cpp / panel_widgets.cpp / panel_web.cpp 三个 TU 使用，
//  不进入公共 API（公共入口仍见 property_panel.h）。实现分属：
//    · param_label → panel_widgets.cpp（参数控件域）
//    · is_web_mode / draw_web_session_section / build_provider_choices /
//      sync_provider_id_with_mode → panel_web.cpp（网页会话 + 提供商域）
// ============================================================================

namespace aiwrite::ui {

// 提供商下拉「合并显示」的候选项（M_patchB L4 / v16；构建见 panel_web.cpp）
//  * id = 主 id（无后缀条目；无后缀可用时 = 该 web 条目自身）
//  * web_id = 网页版孪生 id（空 = 无孪生）
//  * label = 下拉显示名（永不含 `-web`）
struct ProviderChoice {
    std::string id;
    std::string web_id;
    std::string label;
};

// 参数显示名（必填加星号）—— 定义见 panel_widgets.cpp
std::string paramLabel(const engine::Param& param);

// 该节点是否处于网页版（mode=web）模式 —— 定义见 panel_web.cpp
bool isWebMode(const engine::Node& node);

// 网页版会话状态 + 登录入口（PB2-17/18/19）—— 定义见 panel_web.cpp
//  * 返回 changed = 就地改了节点参数（外层据此压快照 + 标记变更）
bool drawWebSessionSection(engine::Node& node, const engine::Graph& graph);

// 提供商下拉候选项（合并显示规则，只改界面层）—— 定义见 panel_web.cpp
std::vector<ProviderChoice> buildProviderChoices();

// 切换「模式」时让 provider 跟随孪生 id（official ↔ web）—— 定义见 panel_web.cpp
void syncProviderIdWithMode(engine::Node& node);

} // namespace aiwrite::ui
