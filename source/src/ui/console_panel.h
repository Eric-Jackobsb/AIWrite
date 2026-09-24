#pragma once

#include <string>

// Console 面板（设计文档 12.4）：级别过滤 + 搜索 + 虚拟化渲染 + 自动滚动
//  * PA-05 增强：复制全部 / 复制可见（过滤结果）/ 导出到文件；上滚自动暂停 + 「回到底部」
//  * default_height 来自 config.toml [ui] console_height（PA-08 接线）
namespace aiwrite::ui {

void draw_console_panel(const char* title, bool* open, float default_height = 220.0f);

// 过滤后的日志纯文本（供「复制可见 / 导出」复用；与面板渲染共用同一个 log::filter，
//  level_filter 与 console_panel.cpp 内的 LevelFilter 序号一致：0=全部 1=INFO 2=WARN 3=ERROR）
std::string console_visible_text(int level_filter, const std::string& search);

} // namespace aiwrite::ui
