#pragma once

namespace aiwrite::ui {

class EditorController;

// 工具栏按钮行（在 DockHost 窗口内绘制一行；全部为鼠标操作，符合设计 §6.7 无快捷键）
// ARCH-A3：撤销/重做可用性与标签读启动注入的 EditorController（不再有全局编辑器单例）
void drawToolbarButtons(EditorController& editor);

} // namespace aiwrite::ui
