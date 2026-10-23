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
#include "official_template.h" // ARCH-A6：新建 API 提供商模态的草稿（活状态归 SettingsPanelState）
#include "site_template.h"     // ARCH-A6：新建网页版站点模态的草稿

namespace aiwrite::ui {

struct SettingsPanelResult {
    bool        saved              = false; // 本帧刚保存成功
    bool        visibility_changed = false; // 面板可见性 / 画布选项变了 ⇒ 宿主需重取本地状态
    std::string message;                    // 状态栏文案（空 = 无）
    // PB2-24 要点②：本帧「重新加载配置表」/「新建网页版站点」成功 ⇒ 宿主需重建 `provider`
    //  参数枚举（`engine::refresh_provider_options(graph)`）——否则刚建的站点 id 会在
    //  运行前校验被判「取值不在枚举范围内」
    bool provider_table_reloaded = false;
};

// ============================================================================
//  ARCH-A6：面板状态显式化（原 19 条文件级 `static` → 一个对象）
//
//  为什么：面板的「窗口开关 / Config 工作副本 / 两个新建模态的草稿」原先是
//  文件级可变变量 —— 隐式单实例、无法并排开两套、也无法离线断言其不变式。
//  现在它是一个显式对象，**归属 = EditorController**（启动构造 · 与 AppContext 同寿）：
//    * 菜单（`AppCommand::Settings`）/ 首启引导 / 主循环绘图 三处取**同一实例**
//    * 跨帧不变式收敛为两条：① 只有 `drawSettingsPanel()` 会改 `edit`；
//      ② `openSettingsPanel()` 重置 `loaded` ⇒ 每次打开都从调用方 Config 重取
//      （避免展示过期值）
//    * 新增 UI 部件若需要自己的跨帧状态，照此加成员，**不许**再引入文件级 static
//  （判据 2 / 规则 6；扫描器 `source/tools/check_globals.ps1`）
// ============================================================================
struct SettingsPanelState {
    bool        open   = false; // 设置窗口是否打开
    bool        loaded = false; // 工作副本是否需要从调用方 Config 重取
    Config      edit;           // 工作副本（只有「保存」才写回磁盘与调用方）
    Config      baseline;       // 打开时的快照（判断宿主是否需要重取本地 UI 状态）
    std::string error;          // 最近一次「保存」被拒的原因
    std::string notice;         // 最近一次动作的结果（绿字）

    // PB2-24 要点①：「新建网页版站点…」模态草稿（名单见 `ai::site_template`）
    bool                site_modal_open   = false; // 本帧请求 OpenPopup（同一窗口上下文内）
    ai::WebSiteTemplate site_tpl;                  // 工作副本
    int                 site_source_index = 0;     // 0 = 空白模板；>0 = 表内 web 条目（照抄）
    bool                site_overwrite    = false; // 覆盖已有条目 / 同名文件（R8：默认不覆盖）
    std::string         site_error;                // 红字（校验 / 落盘 / 生效失败）
    std::string         site_notice;               // 绿字（已创建 + 新的加载摘要）

    // 自定义 AI 收口：「新建 API 提供商…」模态草稿（`ai::official_template`）
    bool                         off_modal_open   = false;
    ai::OfficialProviderTemplate off_tpl;
    int                          off_source_index = 0;
    bool                         off_overwrite    = false;
    std::string                  off_error;
    std::string                  off_notice;
    char                         off_key[512] = {}; // API Key（密码框；只入凭据库，不进模板/JSON）
};

// 菜单调用：打开设置窗口
void openSettingsPanel(SettingsPanelState& state);
void closeSettingsPanel(SettingsPanelState& state);
bool settingsPanelVisible(const SettingsPanelState& state);

// 每帧调用（窗口未打开时为空操作 / 零副作用）
//  * 保存成功时把工作副本写回 `config`（调用方持有的实例）并返回 saved = true
SettingsPanelResult drawSettingsPanel(SettingsPanelState& state, Config& config);

// 说明：配置**校验**实现已移到 `utils/settings_validation.h`（`utils::validate_settings`）——
// 它是纯函数，放在核心库才能被 `api_probe --settings-assert` 离线断言（GUI 目标不可断言）。

} // namespace aiwrite::ui
