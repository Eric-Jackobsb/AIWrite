// =============================================================================
//  AIwrite 主程序入口（M1：技术验证 + 项目骨架）
//  GUI 程序（/SUBSYSTEM:WINDOWS + mainCRTStartup），启动时初始化日志与配置目录
// =============================================================================
#include "ui/app.h"
#include "utils/log.h"
#include "utils/paths.h"
#include "web/webview_host.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void print_usage()
{
    std::printf("AIwrite - AI 小说/剧本创作软件 (M1 技术验证)\n");
    std::printf("用法: aiwrite [选项]\n");
    std::printf("  --console        为日志额外分配控制台窗口（便于开发调试）\n");
    std::printf("  --login-selftest [--timeout <秒>]\n");
    std::printf("                   网页版登录自检：离屏跑「WebView2 环境 → 导航 → 提取 Cookie」\n");
    std::printf("                   退出码 0=通过 / 1=失败 / 2=超时（默认超时 30 秒）\n");
    std::printf("  --help           显示本帮助\n");
}

} // namespace

int main(int argc, char** argv)
{
    aiwrite::ui::AppOptions options;
    bool login_selftest   = false;
    int  selftest_timeout = 30;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--console") {
            options.console = true;
        }
        else if (arg == "--login-selftest") {
            login_selftest = true;
        }
        else if (arg == "--timeout") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --timeout 缺少取值\n");
                return 2;
            }
            selftest_timeout = std::atoi(argv[++i]);
        }
        else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            print_usage();
            return 2;
        }
    }

    aiwrite::log::init(options.console);
    aiwrite::log::info("AIwrite 启动（M1 技术验证 + 项目骨架）");

    const int dir_failures = aiwrite::paths::ensure_data_dirs();
    if (dir_failures != 0) {
        aiwrite::log::warn("有 " + std::to_string(dir_failures) + " 个数据目录创建失败，请检查权限");
    }
    aiwrite::log::info("数据目录: " + aiwrite::paths::data_root().string());

    // 网页版登录自检（不需要 GUI，不进消息循环）
    if (login_selftest) {
        const int selftest_code = aiwrite::web::selftest(selftest_timeout);
        aiwrite::log::info("AIwrite 登录自检退出，返回码 " + std::to_string(selftest_code));
        aiwrite::log::shutdown();
        return selftest_code;
    }

    const int exit_code = aiwrite::ui::run(options);

    // 若用户留着网页版登录窗口就退出了程序：先收尾（关窗口、等线程结束），再关日志
    aiwrite::web::stop_login_window();

    aiwrite::log::info("AIwrite 退出，返回码 " + std::to_string(exit_code));
    aiwrite::log::shutdown();
    return exit_code;
}
