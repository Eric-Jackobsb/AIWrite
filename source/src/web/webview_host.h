#pragma once

// ============================================================================
//  内嵌 WebView2 登录窗口（设计 §8.5：网页版有头登录）
//
//  * 有头登录：用户在窗口内手动完成登录，程序不代填账号密码、不自动刷新会话
//  * Cookie 提取后写入 web::SessionStore（仅内存，程序退出即销毁）
//  * 线程模型：WebView2 需要 STA + 自己的消息泵，因此窗口与消息循环跑在**独立线程**，
//    避免阻塞 ImGui 主线程（登录期间主界面保持响应，可随时点「关闭登录窗口」）
//  * 自动自检：`aiwrite.exe --login-selftest [--timeout N]`
//    → 离屏窗口跑「环境 → 控制器 → 导航 → 提取 Cookie」，退出码 0=通过 / 1=失败 / 2=超时
// ============================================================================

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace aiwrite::web {

struct LoginRequest {
    std::string url          = "https://chat.deepseek.com/";
    std::string profile_dir;                                  // 空 = ~/.brain-ai/webview2
    std::string window_title = "AIwrite · 网页版登录（登录后关闭窗口即可）";
    bool        offscreen       = false;  // true：窗口放在桌面可见区域之外（自检用）
    int         timeout_seconds = 0;      // >0：到时自动关闭（自检用；0 = 直到用户关闭）
    bool        auto_close_after_cookies = false; // true：取到 Cookie 且稳定 3 秒后自动关闭（自检用）
};

class LoginWindow {
public:
    LoginWindow() = default;
    ~LoginWindow();

    LoginWindow(const LoginWindow&)            = delete;
    LoginWindow& operator=(const LoginWindow&) = delete;

    // 启动登录窗口线程；已有窗口在运行时返回 false 并给出原因
    bool start(const LoginRequest& request, std::string* error);

    bool running() const { return running_.load(); }
    void request_close();  // 请求关闭（UI 线程可调，异步）
    void join();           // 等待线程结束
    int  result() const { return result_.load(); }  // -1 未结束 / 0 成功 / 1 失败 / 2 超时
    std::string status() const;

private:
    void set_status(const std::string& text);
    void run_thread(LoginRequest request);

    std::thread        thread_;
    std::atomic<bool>  running_{false};
    std::atomic<int>   result_{-1};
    mutable std::mutex status_mutex_;
    std::string        status_;
};

// 自检：离屏起一次登录窗口并等待结果（同步阻塞），打印脱敏 Cookie 清单
// 返回 0=通过 / 1=失败 / 2=超时
int selftest(int timeout_seconds);

// 进程内单例：参数面板与状态栏共用一个登录窗口（故意不析构，避免退出期竞态）
LoginWindow& login_window();

// 请求关闭并等待登录窗口线程结束（退出主流程前调用，确保在日志关闭前收尾）
void stop_login_window();

} // namespace aiwrite::web
