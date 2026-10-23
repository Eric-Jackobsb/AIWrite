#pragma once

// ============================================================================
//  轻量错误条（PA-06 / Patch A2）
//
//  * 运行结束时若存在失败节点 → 状态栏出现红色错误条（单行省略 + 悬停全文）
//  * 点击 → 选中该失败节点并让视图跟随（复用 EditorController 的导航请求）
//  * 右侧 × 关闭；下次运行成功会自动清空（EditorController::refreshLastError）
//  * ARCH-A3：不再 include 编辑器头 —— 只前向声明，引用由参数注入
// ============================================================================

namespace aiwrite::ui {

class EditorController;

void drawErrorBar(EditorController& state);

} // namespace aiwrite::ui
