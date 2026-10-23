#pragma once

namespace aiwrite::ui {

class EditorController;

// 节点库（设计 §7.2 / M3-09）：按分类列出 8 个节点，点击即在画布中心添加
// ARCH-A3：状态栏 / 节点创建走启动注入的 EditorController（不再有全局编辑器单例）
void drawNodeLibrary(const char* title, bool* open, EditorController& editor);

} // namespace aiwrite::ui
