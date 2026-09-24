#pragma once

// ============================================================================
//  只读文本视图（PA-03：结果呈现共用）
//
//  * 供「输出面板」与「参数面板 → 运行结果」共用同一实现，保证呈现一致
//  * 缓冲按「控件 id + 内容」缓存，仅在内容变化时重建（避免每帧拷贝大文本）
//  * 只读但**可选中**（InputTextMultiline + ReadOnly），并提供统一的复制助手
// ============================================================================

#include <cstddef>
#include <string>

namespace aiwrite::ui {

// 绘制只读多行文本；display_limit 为面板内显示上限（0 = 不限制；超出只截断显示）
void draw_readonly_text(const char* id, const std::string& text, float height_lines = 10.0f,
                        std::size_t display_limit = 40000);

// 复制文本到剪贴板并写日志（log_prefix 形如 "[输出面板]"）
void copy_text(const std::string& text, const std::string& log_prefix);

} // namespace aiwrite::ui
