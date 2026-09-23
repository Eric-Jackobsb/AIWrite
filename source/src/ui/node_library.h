#pragma once

namespace aiwrite::ui {

// 节点库（设计 §7.2 / M3-09）：按分类列出 9 个节点，点击即在画布中心添加
void draw_node_library(const char* title, bool* open);

} // namespace aiwrite::ui
