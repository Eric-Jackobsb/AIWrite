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
    bool        probe_after_load    = false;      // 页面加载完成后自动执行协议探测（M4-06/M4-08）
    bool        auto_close_after_probe = false;   // 探测完成即关闭窗口（命令行自检用）
};

// ---------------------------------------------------------------------------
// 会话自动引导决策（**纯逻辑，便于离线断言**；由 ensure_session 使用）
//
// 教训（2026-09-26 真 bug）：窗口“已打开”**不等于**内存里有凭证——
// 用户手动点「打开登录窗口」时页面加载后并不探测，于是 userToken 永远是空的，
// 而旧版 ensure_session 见到窗口已开就只“等”，必然等到超时 →
// 「未取得网页版凭证」。因此“窗口已开”必须映射为 **ReuseAndProbe（补一次探测）**。
// ---------------------------------------------------------------------------
enum class SessionBoot {
    HaveToken,      // 内存已有 userToken：直接用，不做任何事
    ReuseAndProbe,  // 登录窗口已开且浏览器就绪：在现有页面里补一次协议探测
    StartAndProbe,  // 无可用窗口：离屏起一个（probe_after_load=true 自动探测）
};

inline SessionBoot plan_session_boot(bool has_token, bool window_open)
{
    if (has_token) {
        return SessionBoot::HaveToken;
    }
    // 只看「窗口是否开着」，**不**看浏览器是否已就绪：已开窗口时再 start() 会因
    // 「已有登录窗口在运行」失败，而探测请求是**可重试**的（ensure_session 每 6 秒补一次），
    // 所以“还没就绪”只需重试，不该另开窗口。
    return window_open ? SessionBoot::ReuseAndProbe : SessionBoot::StartAndProbe;
}

// 参数面板「打开登录窗口（WebView2）」使用的请求：**页面加载完成后自动探测一次**
// （手动开的窗口同样必须拿到内存凭证，否则运行时 ensure_session 只能等到超时）
inline LoginRequest interactive_login_request()
{
    LoginRequest request;
    request.url              = "https://chat.deepseek.com/";
    request.window_title     = "AIwrite · DeepSeek 网页版登录（登录后关闭本窗口）";
    request.probe_after_load = true;
    return request;
}

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

// 在**已打开**的登录窗口里请求一次协议探测（异步；结果写入 web::SessionStore.probe）
// 典型用途：用户登录后点参数面板的「探测网页版协议」，把真实协议（challenge / Token / 端点）取回来
void request_protocol_probe();

// 协议探测自检：离屏起登录窗口 → 页面加载后自动探测 → 打印结果（脱敏）
// 前置：profile 里已有登录态（用户至少登录过一次）；返回 0=探测成功 / 1=失败
int protocol_probe(int timeout_seconds);

// 用**登录窗口内的官方 PoW worker** 求解（M4-06/M4-09 方案 A：版本自适应、零逆向）
//  * challenge_json：/api/v0/chat/create_pow_challenge 的原始响应
//  * 若登录窗口尚未打开，会离屏启动一个（profile 已登录即可用），用户无需干预
//  * 同步阻塞至多 timeout_ms；成功返回 answer（>=0），失败返回 -1 并写 error
long long solve_pow_via_page(const std::string& challenge_json, int timeout_ms,
                             std::string* error);

// 确保内存会话里有可用凭证（Cookie + userToken）：没有则离屏起登录窗口并等一次协议探测完成
// 返回 true 表示已具备凭证（幂等，已有凭证时立即返回）
bool ensure_session(int timeout_ms, std::string* error);

// 进程内单例：参数面板与状态栏共用一个登录窗口（故意不析构，避免退出期竞态）
LoginWindow& login_window();

// 请求关闭并等待登录窗口线程结束（退出主流程前调用，确保在日志关闭前收尾）
void stop_login_window();

} // namespace aiwrite::web
