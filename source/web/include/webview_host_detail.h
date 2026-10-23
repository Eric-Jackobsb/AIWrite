#pragma once

// ============================================================================
//  内嵌 WebView2 登录窗口 —— **模块内契约**(ARCH-Z1b)
//
//  * 只允许 `web/src/webview_host*.cpp` 自己 include；**不是**公开 API
//  * 公开语义入口 = `webview_host.h`（本文件只把「一个 TU 内部原本私有的东西」
//    显式化成 8 个 TU 之间的契约：`WindowCtx` / 状态对象 / 消息常量 / 入口）
//  * 逐字搬迁（零语义变化）：所有结构体定义、常量、函数体均自原 3514 行单文件原样搬出
//  * 拆分版图（行数见 `docs/audit/audit_arch_z1_web.md`）：
//      `webview_host.cpp`            状态 / 窗口表 / 通用工具
//      `webview_host_window.cpp`     WebView2 环境·控制器·`window_proc`·线程体·网络回执挂钩
//      `webview_host_probe.cpp`      探测脚本 / 探测两段式 / 就绪三级 / `wait_ready`
//      `webview_host_actions.cpp`    脚本槽 / 复位导航 / `input[type=file]` / CDP 输入（窗口内）
//      `webview_host_pow.cpp`        页面内 PoW 求解 + 按站点注销（窗口内）
//      `webview_host_api.cpp`        句柄 / 自检 / 窗口诊断 / `prepare_window` / 归属守卫
//      `webview_host_session.cpp`    按站点会话（`ensure_session` / `logout_site`）/ 回执 / 上传 / CDP 输入
//      `webview_host_upload.cpp`     附件入口**只读**自动识别（M8-37）
// ============================================================================

#include <windows.h>

#include <wrl/client.h> // Microsoft::WRL::ComPtr
#include <wrl/event.h>  // Microsoft::WRL::Callback

#include <WebView2.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "webview_host.h"   // LoginWindow / LoginRequest / WindowCtx 前向声明
#include "session_store.h"  // Cookie / Session / WebResourceRecord
#include "web_error.h"      // WebErrorKind / web_error_text（归属守卫文案）
#include "window_table.h"   // WindowTable（窗口表纯函数）

// 原单文件的文件级 `using`（`ComPtr` / `Callback` 在 8 个 TU 里同名同义使用）
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace aiwrite::web {

// ---- 窗口类 / 定时器 / 跨线程消息（值不变；原 `webview_host.cpp:41-64`）----

constexpr const wchar_t* kWindowClass   = L"AIwriteWebLoginWindow";
constexpr UINT_PTR       kTimerId       = 1;
constexpr UINT           kTimerTickMs   = 500;   // 定时器节拍
constexpr DWORD          kCookiePollMs  = 3000;  // 登录期间重取 Cookie 的间隔
constexpr DWORD          kAutoCloseGraceMs = 3000; // 自检：取到 Cookie 后静置多久自动收尾
constexpr int            kHotkeyExtract = 1;     // Ctrl+Alt+C：立即重新提取
constexpr int            kHotkeyClose   = 2;     // ESC：关闭窗口
constexpr UINT           kProbeMessage  = WM_APP + 1; // 请求协议探测（可在任意线程 Post）
constexpr UINT           kSolvePowMessage = WM_APP + 2; // 请求在页面内求解 PoW（任意线程 Post）
constexpr UINT           kLogoutMessage   = WM_APP + 3; // 请求按站点注销（删该 origin 的 Cookie + 清 localStorage）
constexpr UINT           kRunScriptMessage = WM_APP + 4; // L3：请求在页面内执行一段脚本并回传原始 JSON（任意线程 Post）
constexpr UINT           kSetFilesMessage  = WM_APP + 5; // M8-14：请求把本地文件交给 input[type=file]（任意线程 Post）
constexpr UINT           kCdpInputMessage = WM_APP + 6; // PW-07f：请求 CDP 真实输入 / 发送（任意线程 Post）
constexpr UINT           kNavigateMessage = WM_APP + 7; // PW-08（D-35③ 修订）：请求「同控制器导航回新会话页」（任意线程 Post）
// 页面加载完成后到开始协议探测的等待（给 SPA 一点就绪时间）
constexpr DWORD kProbeDelayMs = 1500;
// `PW-08`（D-35③ 修订 2026-10-17）：复位导航完成等待预算 + 关窗重开兜底的避让间隔
//  * 干净 profile 确定性复现：关窗后**同一毫秒**重开 ⇒ 第二个 CreateCoreWebView2Controller
//    同步失败 0x802A000C（浏览器侧瞬时竞态）⇒ 复位首选 = 导航；关窗重开降级为兜底且必须避让
//  * `D-35④`（2026-10-18）补充：批量探测的 0x802A000C 是**另一个**成因 —— 跨窗口线程复用
//    同一 environment（线程亲和违反），已改为每窗口线程自建环境（见 `ensure_environment`）。
//    这里的 `kReopenSettleMs` 仍用于兜底重开的浏览器侧释放避让。
constexpr DWORD kResetNavigateTimeoutMs = 15000;
constexpr DWORD kReopenSettleMs         = 800;

constexpr int kProbeMaxAttempts = 24;  // 24 × 500ms ≈ 12 秒
constexpr int kPowMaxAttempts   = 240; // 240 × 500ms ≈ 120 秒

constexpr std::size_t kNetRecordLimit = 400; // 网络回执有界缓冲（R13：不能无限增长）

// ---- 窗口运行时状态（`web/webview_host.h` 里前向声明为 `aiwrite::web::WindowCtx`）----
struct WindowCtx {
    // ---- ① 归属（`PW-02` · `I24`）----
    WebOwner      owner;     // 本窗口归属（`run#node` / `界面#登录窗口` / `自检#网页通道`）
    std::string   site_key;  // 本窗口站点（origin）
    std::string   key;       // 窗口键（`window_key(owner, site_key)`）
    std::uint64_t seq = 0;   // 创建序号（诊断用；收尾顺序由**窗口表**给出，不由调用者决定）
    LoginRequest  request;   // 开窗请求（`start` 时写入，窗口线程只读）

    // ---- ② 生命周期 ----
    std::thread       thread;
    std::atomic<bool> running{false};
    std::wstring      class_name; // 每窗口独立的窗口类名（多窗口并存必须各异）

    // ---- ③ 窗口线程内的运行时状态（原「进程级单例」，现按窗口隔离）----
    std::atomic<HWND>     window{nullptr};
    std::atomic<bool>     stop{false};
    std::atomic<int>      result{-1};
    std::atomic<unsigned> cookieCount{0};
    std::atomic<bool>     cookies_done{false};
    std::atomic<DWORD>    first_cookie_tick{0};
    // 协议探测状态：0=空闲 / 1=已注入（轮询等待结果）/ 2=完成
    int                   probe_stage   = 0;
    int                   probe_attempt = 0;
    DWORD                 probe_due_tick  = 0;
    DWORD                 probe_last_poll = 0;

    // 页面内 PoW 求解：跨线程同步（调用方等待，窗口线程执行）
    std::mutex              pow_mutex;
    std::condition_variable pow_cv;
    bool                    pow_done = false;
    long long               pow_answer = -1;
    std::string             pow_error;
    std::string             pow_challenge_json;
    int                     pow_stage      = 0;   // 0=空闲 / 1=已注入（轮询等待）/ 2=完成
    int                     pow_attempt    = 0;
    DWORD                   pow_last_poll  = 0;

    // ---- L3（PB2-13 / PB2-15）：通用「窗口内同步执行脚本」槽（**按窗口 / 按 owner**）----
    std::mutex              script_mutex;
    std::condition_variable script_cv;
    bool                    script_done = false;
    std::string             script_js;
    std::string             script_result;
    std::string             script_error;

    // ---- `PW-08` 修订（`D-35③` 评审 2026-10-17）：复位 = **同一控制器「导航回新会话页」** ----
    //  * 干净 profile 确定性复现：关窗后同毫秒重开 ⇒ 第二个 CreateCoreWebView2Controller
    //    同步失败 0x802A000C（浏览器侧瞬时竞态）⇒ 复位首选改为导航（login_url 即新会话页），
    //    关窗重开降级为兜底（带 `kReopenSettleMs` 避让）。
    //  * 跨线程协议照抄「脚本槽」：调用方 PostMessage(kNavigateMessage) + 等 nav_cv；
    //    NavigationCompleted 回调（窗口线程，见 `on_controller_ready`）置 nav_pending=false 并 notify。
    std::mutex              nav_mutex;
    std::condition_variable nav_cv;
    std::string             nav_url;
    bool                    nav_pending = false; // true = 正在等一次导航完成
    bool                    nav_done    = false; // 完成（成功或失败均置位）
    bool                    nav_ok      = false;
    std::string             nav_error;
    std::mutex              nav_call_mutex;      // 并发调用方串行（同窗口同时只允许一次复位导航）

    // 按站点注销（PB2-19）：跨线程同步（调用方等待，窗口线程删 Cookie + 清 localStorage）
    std::mutex              logout_mutex;
    std::condition_variable logout_cv;
    bool                    logout_done  = false;
    bool                    logout_ok    = false;
    std::string             logout_error;
    unsigned                logout_deleted = 0;
    DWORD                   start_tick = 0;
    DWORD                   last_poll  = 0;

    // ---- 浏览器对象（**环境按 profile 共享**；见 `ensure_environment`）----
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller>  controller;
    ComPtr<ICoreWebView2>            webview;

    // ---- `PW-05`（归因分离）：同窗口**串行执行**守卫 ----
    //  * 一个窗口的页面是**单线程**执行的（WebView2 官方：回调全部在该窗口线程上跑）
    //    ⇒ 同窗口上的两次并发调用会互相踩槽位；这里显式检测并给出**可分辨**的
    //    「窗口被占用」原因（旧实现是**静默串槽**：一个 `g_script_done` 被两个等待者共享）
    //  * 同一线程**重入**放行（`wait_ready` → `peek_page_state` 这类同链路嵌套不算并发）
    std::mutex      busy_mutex;
    bool            busy = false;
    std::thread::id busy_thread{};

    // ---- `PW-08`（`D-35③`）：复位记账 ----
    bool used_before   = false; // 本窗口是否已被用过（false ⇒ 下一次调用是「首次」：要复位到新会话页）
    bool failed_before = false; // 上一次使用以「未就绪」失败（⇒ 触发**一次**复位后重试）
    int  resets_done   = 0;     // 已复位次数（上限 `kMaxResetsPerWindow`）

    // ---- 状态文案（界面显示 / `LoginWindow::status()`）----
    mutable std::mutex status_mutex;
    std::string        status;

    // ---- M8-14：CDP「把本地文件交给页面」的**本窗口**槽（调用方等待，窗口线程执行）----
    std::mutex               files_mutex;
    std::condition_variable  files_cv;
    bool                     files_done = false;
    bool                     files_ok   = false;
    std::string              files_selector;
    std::vector<std::string> files_paths;
    std::string              files_detail;
    std::string              files_error;

    // ---- M_patchC（`PW-07f`）：CDP「真实输入 / 发送」的**本窗口**槽 ----
    std::mutex              cdp_mutex;
    std::condition_variable cdp_cv;
    bool                    cdp_done  = false;
    bool                    cdp_ok    = false;
    std::string             cdp_text;
    bool                    cdp_enter = false;
    std::string             cdp_detail;
    std::string             cdp_error;

    // ---- M8-14：网络回执采集（**本窗口**的回执 —— `PW-04` / `I26`：回执不跨 owner）----
    std::mutex                     net_mutex;
    std::vector<WebResourceRecord> net_records;
    bool                           net_ready = false;      // 是否挂钩成功

    // ---- M_patchC（`PW-03` / `I25`）：**本窗口**的就绪缓存 ----
    //  * 目的：轮询期间（`dom_chat` 每 500 ms 一轮）不必每轮都多跑一次页面状态探针
    //  * 失效点：窗口 start（换站点 / 重开）与 WM_DESTROY（窗口关闭）
    //  * 只缓存「已达掩码」，**不缓存**页面内容 —— 不改变任何判定语义
    std::mutex  ready_mutex;
    std::string ready_site;
    int         ready_mask = 0;
    DWORD       ready_tick = 0;
};


// ---- `PW-02`：**窗口表**（唯一真相 = 键与顺序；运行时对象见 `windows`）----
//  * `table` = 纯函数决策模型（`web/window_table.h`）：复用 / 新建 / 收尾顺序 / 诊断行
//  * `windows` = 窗口键 → 窗口运行时（`WindowCtx`）；**只在 `ctx_acquire` / `ctx_close` 里变更**
//    （两处同时改，保证与 `table` 不发散）
// ARCH-A7：原先是 3 条文件级 static（`g_table_mutex` / `g_table` / `g_windows`）
// ⇒ 归拢为 `WindowRegistry`，单一定义点 = `windowRegistry()`。
struct WindowRegistry {
    std::mutex                                        table_mutex;
    WindowTable                                       table;
    std::map<std::string, std::shared_ptr<WindowCtx>> windows;
};

WindowRegistry& windowRegistry();

// ---- 浏览器**环境**（`D-35④` 修订 2026-10-18：**每窗口线程各建一个**，不再跨线程共享）----
//  * WebView2 对象有**线程亲和性**（必须在创建它的线程上使用，见官方「Threading model」文档）。
//    此前把第一个窗口线程创建的环境缓存进 `g_envs`、交给**别的**窗口线程
//    `CreateCoreWebView2Controller` ⇒ 跨线程调用同步失败 `0x802A000C`
//    （`--web-adapter-selftest --provider all` 9/10 站失败的根因，真机 2026-10-17 日志实证）。
//  * 现在每个窗口线程在自己的线程上创建并独占自己的环境：本线程建、本线程用、窗口关闭即释放。
//    同 profile 的**并发创建**仍由 `creating` 串行化（首建时 user data folder 被占用）。
//  * `fail` = 创建失败码缓存：同一 profile 不反复重试空转（如实报错）。
// ARCH-A7：原先是 4 条文件级 static（`g_env_mutex` / `g_env_cv` / `g_env_fail` / `g_env_creating`）
// ⇒ 归拢为 `EnvCache`，单一定义点 = `envCache()`。
struct EnvCache {
    std::mutex                     mutex;
    std::condition_variable        cv;
    std::map<std::string, HRESULT> fail;     // profile 目录 → 创建失败码
    std::string                    creating; // 正在创建该 profile
};

EnvCache& envCache();

// ---- 「当前窗口」上下文（`thread_local`）----
//  * 窗口线程：线程启动时绑定**自己**的窗口 ⇒ 窗口线程内所有 `st()` 都指自己（含 COM 回调）
//  * 调用方线程：进入公开 API 时用 `CtxScope` 绑定**目标**窗口（由 `ctx_find` 得到）
//  * 这一层替换掉了改造前的「进程级单例」：**同一份代码**现在服务 N 个窗口
// ARCH-A7：`t_cur` → `tlsWindow()`（线程局部单实例的唯一读写口，不再是文件级 `thread_local` 变量）
WindowCtx*& tlsWindow();

// 绑定 / 还原「当前窗口」（作用域守卫；支持嵌套）
class CtxScope {
public:
    explicit CtxScope(WindowCtx* ctx) : prev_(tlsWindow()) { tlsWindow() = ctx; }
    ~CtxScope() { tlsWindow() = prev_; }
    CtxScope(const CtxScope&)            = delete;
    CtxScope& operator=(const CtxScope&) = delete;

private:
    WindowCtx* prev_;
};

// `PW-05`：同窗口**串行执行**守卫（同一线程重入放行）
//  * 检测到并发 ⇒ 返回 `false` + `WebErrorKind::Preempted` 文案（**可分辨**，不静默串槽）
class BusyGuard {
public:
    BusyGuard(WindowCtx& ctx, const std::string& site, bool* ok, std::string* error) : ctx_(ctx)
    {
        std::lock_guard<std::mutex> lock(ctx_.busy_mutex);
        const std::thread::id       self = std::this_thread::get_id();
        if (ctx_.busy && ctx_.busy_thread != self) {
            if (ok != nullptr) {
                *ok = false;
            }
            if (error != nullptr) {
                *error = web_error_text(WebErrorKind::Preempted, ctx_.owner, site,
                                        "该窗口正在被同一归属的另一次调用使用（" + ctx_.key + "）");
            }
            return;
        }
        if (!ctx_.busy) {
            ctx_.busy        = true;
            ctx_.busy_thread = self;
            owned_           = true;
        }
        if (ok != nullptr) {
            *ok = true;
        }
    }
    ~BusyGuard()
    {
        if (owned_) {
            std::lock_guard<std::mutex> lock(ctx_.busy_mutex);
            ctx_.busy = false;
        }
    }

    BusyGuard(const BusyGuard&)            = delete;
    BusyGuard& operator=(const BusyGuard&) = delete;

private:
    WindowCtx& ctx_;
    bool       owned_ = false;
};


// ---- 模块内入口（跨 TU；逐字搬迁自原单文件，**签名不变**）------------------
//  * 状态对象：唯一实例 = 函数内 static（单一定义点 = `webview_host.cpp`）
WindowRegistry& windowRegistry();
EnvCache&       envCache();
WindowCtx*&     tlsWindow();
inline WindowCtx& st() { return *tlsWindow(); } // 原 `st()`（thread_local 当前窗口）

// 通用工具（窗口线程 / 调用方线程共用）
void         forget_ready();
std::wstring to_wide(const std::string& text);
std::string  to_utf8(const std::wstring& text);
void         attach_parent_console();
void         resize_webview();
void         write_session(const std::vector<Cookie>& cookies);
void         extract_cookies(const char* reason);
void         close_window(int code);

// 窗口表（`PW-02` / `I24`）
std::shared_ptr<WindowCtx> ctx_find(const WebOwner& owner, const std::string& site_key);
bool                       ctx_alive(const std::shared_ptr<WindowCtx>& ctx);
std::shared_ptr<WindowCtx> ctx_start(const WebOwner& owner, LoginRequest request,
                                     std::string* error);
void                       ctx_close(const std::shared_ptr<WindowCtx>& ctx, int code, bool wait);
void                       ctx_close_all();
std::shared_ptr<WindowCtx> ctx_latest_alive();
void                       ctx_post_probe(const std::shared_ptr<WindowCtx>& ctx);

// 窗口线程体（`webview_host_window.cpp` 定义；`window_thread_main` 在窗口线程上调用）
int run_login_window(const LoginRequest& request);

// 窗口线程内的消息处理器（`window_proc` 按 `WM_APP + n` 分发；各自 TU 定义）
void start_protocol_probe();
void poll_protocol_probe();
void run_script_in_window();
void run_navigate_in_window();
bool navigate_now(WindowCtx* ctx, const std::string& url, int timeout_ms, std::string* error);
void run_set_files_in_window();
void run_cdp_input_in_window();
void run_pow_solve_in_page();
void poll_pow_solve();
void run_logout_in_page();

// 就绪三级（`PW-03` / `I25`）
void remember_ready(const std::string& site, int mask);
int  cached_ready(const std::string& site, int need);
bool wait_controller_ready(int timeout_ms, std::string* error);

// 公开 API 的公共前置（`PW-08`）：复用 / 新建 / 复位（**唯一**入口）
bool prepare_window(const LoginRequest& site_request, std::shared_ptr<WindowCtx>* out,
                    std::string* error);

// L3：在**已开的窗口**里执行脚本并同步等待（不做会话 / 开窗前置）
// * `webview_host_session.cpp` 定义；`webview_host_probe.cpp`（页面状态 / 脚本探测）复用
bool run_script_now(const std::string& script_js, int timeout_ms, std::string* json_result,
                    std::string* error);

// PB2-23：探测主体（两个公开入口共用，行为逐字一致）
// * `webview_host_upload.cpp` 定义；`protocolProbe` / `protocolProbeForProvider` 复用
int protocol_probe_with(const LoginRequest& base_request, const ai::ProviderSpec* spec,
                        const std::string& site_label, int timeout_seconds);

} // namespace aiwrite::web

