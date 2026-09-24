#pragma once

// ============================================================================
//  窗口几何（F3 / FEA-M3-04 · 设计 §6.1 PD-04）
//
//  * 纯函数：把「期望窗口矩形」矫正到显示器工作区（越屏矫正）——不依赖 GLFW，可被自检直接覆盖
//  * 约定：pos_x / pos_y < 0 表示「未记录位置」（首次启动或配置缺失 → 由调用方决定居中/默认尺寸）
//  * 尺寸保护：不小于 min_width / min_height；若工作区更小则以工作区为上限
// ============================================================================

namespace aiwrite::utils {

struct WindowRect {
    int x      = 0;
    int y      = 0;
    int width  = 0;
    int height = 0;
};

// 是否记录了位置（-1 = 未记录）
bool has_position(const WindowRect& rect);

struct FitResult {
    WindowRect rect;
    bool       changed = false; // 与输入不同 → 调用方应把矫正结果写回配置
};

// 越屏矫正：先夹尺寸（≥ 最小值、≤ 工作区），再平移使整窗落在工作区内（尽量保留原位置）
//  * 工作区尺寸 ≤ 0（例如无显示器）时原样返回（changed = false）
FitResult fit_window_to_workarea(const WindowRect& desired, const WindowRect& workarea,
                                 int min_width = 640, int min_height = 480);

} // namespace aiwrite::utils
