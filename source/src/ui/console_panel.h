#pragma once

// Console 面板（设计文档 12.4）：级别过滤 + 搜索 + 虚拟化渲染 + 自动滚动
namespace aiwrite::ui {

void draw_console_panel(const char* title, bool* open);

} // namespace aiwrite::ui
