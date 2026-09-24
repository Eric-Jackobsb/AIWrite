#include "utils/window_geometry.h"

#include <algorithm>

namespace aiwrite::utils {

bool has_position(const WindowRect& rect)
{
    return rect.x >= 0 && rect.y >= 0;
}

FitResult fit_window_to_workarea(const WindowRect& desired, const WindowRect& workarea,
                                 int min_width, int min_height)
{
    FitResult result;
    result.rect = desired;

    // 工作区未知（例如无显示器 / 未取到）→ 原样返回
    if (workarea.width <= 0 || workarea.height <= 0) {
        return result;
    }

    // 1) 尺寸：不小于最小尺寸，不超过工作区
    int width  = std::max(desired.width, min_width);
    int height = std::max(desired.height, min_height);
    width      = std::min(width, workarea.width);
    height     = std::min(height, workarea.height);

    // 2) 位置：整窗落在工作区内（尽量保留原位置）
    const int max_x = workarea.x + workarea.width - width;
    const int max_y = workarea.y + workarea.height - height;
    const int x     = std::min(std::max(desired.x, workarea.x), std::max(max_x, workarea.x));
    const int y     = std::min(std::max(desired.y, workarea.y), std::max(max_y, workarea.y));

    result.rect    = WindowRect{x, y, width, height};
    result.changed = result.rect.x != desired.x || result.rect.y != desired.y ||
                     result.rect.width != desired.width || result.rect.height != desired.height;
    return result;
}

} // namespace aiwrite::utils
