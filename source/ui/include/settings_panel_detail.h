#pragma once

// ============================================================================
//  设置面板 —— **模块内契约**(ARCH-Z1 第 3 组)
//
//  * 只允许 `ui/src/settings_panel*.cpp` 自己 include；**不是**公开 API（公开入口 = `settings_panel.h`）
//  * 逐字搬迁（零语义变化）：原 944 行拆成「主面板 / 新建网页版站点模态 / 新建 API 提供商模态」
//  * 跨 TU 只需 5 个内部函数 + 2 个模态窗口名（其余 4 个工具函数各归其 TU）
//  * 拆分版图：
//      `settings_panel.cpp`           工具（重取判定 / 文本桥 / 载入副本）+ 打开/保存 + 6 个分区 + 底部动作
//      `settings_panel_site.cpp`      ① 新建网页版站点模态（组合框 + 加载报告定位 + 草稿编辑/落盘）
//      `settings_panel_official.cpp`  ② 新建 API 提供商模态（模板来源 / 校验 / DPAPI 入库）
// ============================================================================

#include "settings_panel.h"
#include "provider_spec.h"  // ai::SpecLoadReport（加载报告原文行）

#include <cstddef>
#include <string>

namespace aiwrite::ui {

// 模态窗口名：原 `settings_panel.cpp` 匿名命名空间常量（只读常量表 ⇒ 规则 6 合法）
//  * 生成方（open_new_*_modal，主面板 TU）与消费方（OpenPopup，模态 TU）跨 TU 共享
constexpr const char* kNewSiteModal     = "新建网页版站点";
constexpr const char* kNewOfficialModal = "新建 API 提供商";

namespace settings_detail {

bool input_text_string(const char* id, std::string& value, std::size_t capacity);
bool id_in_table(const std::string& id);
std::string site_report_line(const ai::SpecLoadReport& report, const std::string& id);
void draw_new_site_modal(SettingsPanelState& state, SettingsPanelResult& result);
void draw_new_official_modal(SettingsPanelState& state, SettingsPanelResult& result);

}  // namespace settings_detail
}  // namespace aiwrite::ui
