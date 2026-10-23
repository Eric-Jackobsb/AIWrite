#pragma once

#include <string>

// Console 面板（设计文档 12.4）：级别过滤 + 搜索 + 虚拟化渲染 + 自动滚动
//  * PA-05 增强：复制全部 / 复制可见（过滤结果）/ 导出到文件；上滚自动暂停 + 「回到底部」
//  * default_height 来自 config.toml [ui] console_height（PA-08 接线）
namespace aiwrite::ui {

// 日志级别过滤（ARCH-A6：从 console_panel.cpp 的匿名命名空间提到头文件，
// 让面板状态可以是一个显式对象；序号与 consoleVisibleText 的 level_filter 口径一致）
enum class LevelFilter : int { All = 0, Info = 1, Warn = 2, Error = 3 };

// ---------------------------------------------------------------------------
//  ARCH-A6：Console 面板的跨帧状态（原 `console_panel.cpp` 3 条文件级 static）
//  * 归属 = `EditorController::console_panel()`（面板与主循环同寿）
// ---------------------------------------------------------------------------
struct ConsolePanelState {
    LevelFilter level_filter = LevelFilter::All; // 级别过滤
    char        search[128]  = {};               // 搜索框（大小写不敏感子串）
    bool        auto_scroll  = true;             // 自动滚动（用户上滚时自动置 false）
};

void drawConsolePanel(const char* title, bool* open, ConsolePanelState& state,
                      float default_height = 220.0f);

// 过滤后的日志纯文本（供「复制可见 / 导出」复用；与面板渲染共用同一个 log::filter，
//  level_filter 与 console_panel.cpp 内的 LevelFilter 序号一致：0=全部 1=INFO 2=WARN 3=ERROR）
std::string consoleVisibleText(int level_filter, const std::string& search);

} // namespace aiwrite::ui
