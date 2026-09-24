#pragma once

// ============================================================================
//  轻量错误条（PA-06 / Patch A2）
//
//  * 运行结束时若存在失败节点 → 状态栏出现红色错误条（单行省略 + 悬停全文）
//  * 点击 → 选中该失败节点并让视图跟随（复用 EditorState 的导航请求）
//  * 右侧 × 关闭；下次运行成功会自动清空（EditorState::refresh_last_error）
// ============================================================================

#include "ui/editor_state.h"

namespace aiwrite::ui {

void draw_error_bar(EditorState& state);

} // namespace aiwrite::ui
