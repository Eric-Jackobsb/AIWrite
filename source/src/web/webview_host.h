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
#include <vector>

#include "ai/provider_spec.h"     // M_patchB L1 续（PB2-17）：站点参数来自配置表条目
#include "web/session_store.h"    // 站点键（origin）/ 会话归档
#include "web/site_ref.h"         // M7B 批 2：站点描述 + 纯函数**已搬迁到此**（零语义；见该文件头注释）

namespace aiwrite::web {

// 站点描述（`LoginRequest` / `SiteRef`）与全部纯函数（`login_request_site` / `login_request_of` /
// `interactive_login_request` / `probe_login_request` / `boot_login_request` / `SessionBoot` /
// `plan_session_boot`）**已迁移到 `web/site_ref.h`**（M7B 批 2 · §6.2 · **零语义变化**）——
// 目的：新通道（`web/pydoll_channel`）取站点描述时**不必** include 本文件（本文件拉入 WebView2 依赖）。

// 渲染「协议探测 kickoff 脚本」：把站点路径从 LoginRequest 注入 JS 模板
//  * 不传任何站点参数时，渲染结果与改造前的常量脚本**逐字一致**
std::string probe_kickoff_script(const LoginRequest& request);

// 会话自动引导决策（`SessionBoot` / `plan_session_boot`）**同样已迁移到 `web/site_ref.h`**（见上）。

// 参数面板「打开登录窗口（WebView2）」使用的请求：**页面加载完成后自动探测一次**
// （手动开的窗口同样必须拿到内存凭证，否则运行时 ensure_session 只能等到超时）
//  * 无参重载 = 改造前的内置默认站点（DeepSeek），行为逐字不变（不变量 I2）
//  * 带站点参数的重载见上方 `interactive_login_request(web, id)`（PB2-17）

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

// M_patchB L1 修订（PB2-23）：按**配置表条目**的站点探测（严格解析，**不**回落）
//  * 站点不可用（非网页版条目 / 缺 web.login_url / 表外 id）→ 打印原因并返回 2（**不发请求、不开窗**）
int protocol_probe_for_provider(const std::string& provider_id, int timeout_seconds);

// ---- M_patchB L3（PB2-13 / PB2-15）：在**已登录窗口**内同步执行一段 JS 并取回结果 ----
//  * 内部：按站点 `ensure_session` → 把脚本 marshal 到窗口线程执行 → 返回 `ExecuteScript` 的**原始 JSON 结果**
//  * 脚本应 `return` 一个**对象**（JSON 可序列化）；本函数**不解释**内容（由调用方解析）
//  * 典型用途：DOM 适配器（注入提示词 / 触发发送 / 轮询答案）与选择器探测（PB2-15）
bool run_script_sync(const LoginRequest& site_request, const std::string& script_js, int timeout_ms,
                     std::string* json_result, std::string* error);

// 用**登录窗口内的官方 PoW worker** 求解（M4-06/M4-09 方案 A：版本自适应、零逆向）
//  * site_url：**目标站点**（origin / 登录页；PB2-23：不再用「内置默认站点」猜 —— 决策 D-22②）
//    空串 = 旧行为（内置默认站点，仅供 CLI 自检守 I2）
//  * challenge_json：/api/v0/chat/create_pow_challenge 的原始响应
//  * 若登录窗口尚未打开 / 不在该站点，会**按站点**离屏启动一个（profile 已登录即可用），用户无需干预
//  * 同步阻塞至多 timeout_ms；成功返回 answer（>=0），失败返回 -1 并写 error
long long solve_pow_via_page(const std::string& site_url, const std::string& challenge_json,
                             int timeout_ms, std::string* error);

// 确保内存会话里有可用凭证（Cookie + userToken）：没有则离屏起登录窗口并等一次协议探测完成
// 返回 true 表示已具备凭证（幂等，已有凭证时立即返回）
bool ensure_session(int timeout_ms, std::string* error);

// ---- M_patchB L1 续（PB2-17/18/19）：按站点 ----
// 当前登录窗口所属站点键（origin；无窗口 / 未启动 = 空）
std::string current_window_site();
// 登录窗口是否已开在指定站点上
bool        window_on_site(const std::string& site);
// 按**站点**确保凭证：站点与当前窗口不一致时**串行复用**同一个窗口（先关旧窗再按目标站点开窗，
// 决策 D-20）；轮询期间只补探测，不重复开窗
bool        ensure_session(const LoginRequest& site_request, int timeout_ms, std::string* error);
// 按**站点**注销：清该站点的内存会话 + 删除该站点 origin 的 Cookie 与 localStorage
// （不删除整个 profile，故不影响其他站点；PB2-19）
bool        logout_site(const LoginRequest& site_request, int timeout_ms, std::string* error);

// 进程内单例：参数面板与状态栏共用一个登录窗口（故意不析构，避免退出期竞态）
LoginWindow& login_window();

// 请求关闭并等待登录窗口线程结束（退出主流程前调用，确保在日志关闭前收尾）
void stop_login_window();

} // namespace aiwrite::web
