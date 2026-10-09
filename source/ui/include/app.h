#pragma once

namespace aiwrite::ui {

// 主程序启动参数
struct AppOptions {
    bool console = false; // 为日志额外分配控制台窗口（便于开发调试）
};

int run(const AppOptions& options);

} // namespace aiwrite::ui
