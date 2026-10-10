#pragma once

// ============================================================================
//  设置面板（M6 `M6-01` / `PD-03`）：把 config.toml 的**可调项**做成界面
//
//  设计约束
//   * **单一权威**：面板编辑的是 `config.toml`；「保存」走 `save_config()`（自动备份
//     `config.toml.bak`）并 `set_app_config()` 同步进程缓存 ⇒ 界面与文件不漂移
//   * **不谎报接线**：当前版本**未接线**的字段照样可见、可改、可存，但一律打
//     「暂不生效（归口 …）」标记（与 `unwired_config_fields()` 同源口径）
//   * **窗口几何不进面板**：`ui.window_*` 是机器状态（由程序自动记录），不是给用户调的设置
//   * 校验失败 ⇒ 不写盘，红字说明「哪里不对、该怎么改」（`validate_settings()`）
//   * 校验函数是**纯函数** ⇒ 无需 GUI 即可断言（`api_probe --settings-assert`）
// ============================================================================

#include <string>

#include "config.h"

namespace aiwrite::ui {

struct SettingsPanelResult {
    bool        saved              = false; // 本帧刚保存成功
    bool        visibility_changed = false; // 面板可见性 / 画布选项变了 ⇒ 宿主需重取本地状态
    std::string message;                    // 状态栏文案（空 = 无）
};

// 菜单调用：打开设置窗口
void open_settings_panel();
void close_settings_panel();
bool settings_panel_visible();

// 每帧调用（窗口未打开时为空操作 / 零副作用）
//  * 保存成功时把工作副本写回 `config`（调用方持有的实例）并返回 saved = true
SettingsPanelResult draw_settings_panel(Config& config);

// 说明：配置**校验**实现已移到 `utils/settings_validation.h`（`utils::validate_settings`）——
// 它是纯函数，放在核心库才能被 `api_probe --settings-assert` 离线断言（GUI 目标不可断言）。

} // namespace aiwrite::ui
