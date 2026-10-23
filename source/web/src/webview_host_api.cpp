// ============================================================================
//  webview_host_api.cpp — 句柄 / 自检 / 窗口诊断 / prepare_window
//  * ARCH-Z1b：自 3514 行单文件 `webview_host.cpp` 逐字拆出，**零语义变化**
//  * 模块内契约见 `webview_host_detail.h`；公开 API 见 `webview_host.h`
// ============================================================================

#include "webview_host_detail.h"
#include "log.h"
#include "probe_verdict.h"

namespace aiwrite::web {

// -------------------------------------------------------- LoginWindow -------
//  `PW-02`：`LoginWindow` 从「进程内单例窗口」改为**窗口句柄**（绑定一个 `owner`）——
//   * 界面用 `login_window()`（owner = `界面#登录窗口`）：它也是**独立**窗口，
//     不再被节点的 `ensure_session` 抢走（真机错配的另一半）
//   * `start()` 把句柄指向 `(owner, 该请求站点)` 的窗口；**只关同一个 owner 的旧窗口**
//     （界面上切换站点仍是「一个可见窗口」⇒ 与今天一致；节点 owner 之间互不影响 · `I24`）
LoginWindow::LoginWindow(WebOwner owner) : owner_(std::move(owner)) {}

LoginWindow::~LoginWindow()
{
    requestClose();
    join();
}

bool LoginWindow::start(const LoginRequest& request, std::string* error)
{
    const std::string site = siteKeyOf(request.url);

    // ① 同一个 owner 在其他站点还有窗口 ⇒ 先关它（换站点 = 换这一个可见窗口；`I24` 允许：同一个 owner）
    const std::vector<std::string> owned = [this] {
        std::lock_guard<std::mutex> lock(windowRegistry().table_mutex);
        return windowRegistry().table.keys_of_owner(owner_);
    }();
    for (const std::string& key : owned) {
        if (key == window_key(owner_, site)) {
            continue;
        }
        std::shared_ptr<WindowCtx> other;
        {
            std::lock_guard<std::mutex> lock(windowRegistry().table_mutex);
            const auto                  it = windowRegistry().windows.find(key);
            if (it != windowRegistry().windows.end()) {
                other = it->second;
            }
        }
        if (other != nullptr) {
            log::info("[网页版窗口] 同一归属换站点：关闭 " + key + "（新站点 " +
                      (site.empty() ? std::string("(默认)") : site) + "）");
            ctx_close(other, -1, /*wait=*/true);
        }
    }

    // ② 该站点已有窗口：**在跑** ⇒ 明确拒绝（不抢占）；**已结束** ⇒ 先收尾再重开
    if (std::shared_ptr<WindowCtx> current = ctx_find(owner_, site); current != nullptr) {
        if (ctx_alive(current)) {
            if (error != nullptr) {
                *error = "已有登录窗口在运行（" + owner_tag(owner_) + " · 站点 " +
                         (site.empty() ? std::string("(默认)") : site) + "）";
            }
            return false;
        }
        ctx_close(current, -1, /*wait=*/true); // 收尾旧壳（线程已结束）
        ::Sleep(kReopenSettleMs); // 旧 controller 浏览器侧可能仍在释放（避 0x802A000C）
    }

    LoginRequest owned_request = request;
    owned_request.owner        = owner_;
    std::shared_ptr<WindowCtx> ctx = ctx_start(owner_, owned_request, error);
    if (ctx == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(ctx->status_mutex);
        ctx->status = "正在打开登录窗口…";
    }
    last_ = ctx;
    return true;
}

void LoginWindow::requestClose()
{
    if (last_ == nullptr) {
        return;
    }
    last_->stop = true;
    if (const HWND window = last_->window.load(); window != nullptr) {
        ::PostMessageW(window, WM_CLOSE, 0, 0);
    }
}

void LoginWindow::join()
{
    if (last_ != nullptr && last_->thread.joinable()) {
        last_->thread.join();
    }
}

bool LoginWindow::running() const
{
    return last_ != nullptr && last_->running.load();
}

int LoginWindow::result() const
{
    return last_ == nullptr ? -1 : last_->result.load();
}

std::string LoginWindow::status() const
{
    if (last_ == nullptr) {
        return "(未打开)";
    }
    std::lock_guard<std::mutex> lock(last_->status_mutex);
    return last_->status;
}

// ------------------------------------------------------------- 自检 --------
int selftest(int timeout_seconds)
{
    attach_parent_console();

    const int timeout = timeout_seconds > 0 ? timeout_seconds : 30;
    log::info("[网页版登录] 自检开始（离屏窗口，超时 " + std::to_string(timeout) + " 秒）");

    LoginRequest request;
    request.offscreen                = true;
    request.timeout_seconds          = timeout;
    request.auto_close_after_cookies = true;

    LoginWindow window(selftest_owner()); // `PW-02`：自检/CLI 用**独立归属**（不与节点窗口互抢）
    std::string error;
    if (!window.start(request, &error)) {
        std::printf("[网页版登录] 自检启动失败: %s\n", error.c_str());
        return 1;
    }
    window.join();

    const Session session = SessionStore::instance().snapshot();
    std::printf("\n===== 网页版会话（脱敏，仅内存）=====\n");
    for (const Cookie& cookie : session.cookies) {
        std::printf("  %-34s %-34s httpOnly=%d session=%d\n", cookie.name.c_str(),
                    maskValue(cookie.value).c_str(), cookie.http_only ? 1 : 0,
                    cookie.session ? 1 : 0);
    }
    std::printf("====================================\n");

    const int   code    = window.result();
    const bool  pass    = (code == 0) && session.cookieCount() > 0;
    const char* verdict = pass ? "PASS" : (code == 2 ? "TIMEOUT" : "FAIL");
    std::printf("[网页版登录] 自检结束：%s（退出码 %d，Cookie %zu 条，url=%s）\n", verdict, code,
                session.cookieCount(), request.url.c_str());
    log::info(std::string("[网页版登录] 自检结束：") + verdict + "（Cookie " +
              std::to_string(session.cookieCount()) + " 条，url=" + request.url + "）");

    SessionStore::instance().clear(); // 自检不留会话
    return pass ? 0 : (code == 2 ? 2 : 1);
}

LoginWindow& loginWindow()
{
    // 故意用裸 new：不在退出期析构，避免静态析构顺序导致的线程/日志竞态。
    // 生命周期由 stop_login_window() 显式收尾。
    static LoginWindow* instance = new LoginWindow(ui_window_owner());
    return *instance;
}

// ---- `PW-02` / `PW-05` / `PW-08`：公开 API 的公共前置（**唯一**入口，避免各调用点各写一套）----

// 在**指定**窗口内请求一次协议探测（跨线程 Post；不抢占别的窗口）
void ctx_post_probe(const std::shared_ptr<WindowCtx>& ctx)
{
    if (!ctx_alive(ctx)) {
        return;
    }
    if (const HWND window = ctx->window.load(); window != nullptr) {
        log::info("[网页版探测] 已请求（" + ctx->key + "）");
        ::PostMessageW(window, kProbeMessage, 0, 0);
    }
}

// 取「最近打开且活着的窗口」（供**界面**「探测网页版协议」按钮 / 无站点参数的老路径使用）
std::shared_ptr<WindowCtx> ctx_latest_alive()
{
    std::vector<std::string> order;
    {
        std::lock_guard<std::mutex> lock(windowRegistry().table_mutex);
        order = windowRegistry().table.keys_in_close_order(); // 后开的在前
    }
    for (const std::string& key : order) {
        std::shared_ptr<WindowCtx> ctx;
        {
            std::lock_guard<std::mutex> lock(windowRegistry().table_mutex);
            const auto                  it = windowRegistry().windows.find(key);
            if (it != windowRegistry().windows.end()) {
                ctx = it->second;
            }
        }
        if (ctx_alive(ctx)) {
            return ctx;
        }
    }
    return nullptr;
}

// `PW-08`（`D-35③`）：为「一次节点级调用」准备**本 owner 自己的**该站点窗口
//  * 复用 / 新建：由**窗口表**决定（同站点不同 owner 各开各的；跨站点**不关旧窗**）
//  * 复位：`plan_reset`（首次 + 失败时 · 单次重试）= **关窗重开**（窗口级；`login_url` 即新会话页）
//  * 输出 `ctx`：调用方用 `CtxScope` 绑定后，既有实现（`st()`）即指向该窗口
bool prepare_window(const LoginRequest& site_request, std::shared_ptr<WindowCtx>* out,
                    std::string* error)
{
    const std::string          site = siteKeyOf(site_request.url);
    std::shared_ptr<WindowCtx> ctx  = ctx_find(site_request.owner, site);

    const auto start_own_window = [&site_request, &site, error]() {
        LoginRequest request     = site_request;
        request.offscreen        = true;  // 离屏：不抢焦点（用户在界面开的窗口是**另一个 owner**）
        request.probe_after_load = site_request.probe_applicable;
        return ctx_start(site_request.owner, request, error);
    };

    if (!ctx_alive(ctx)) {
        if (ctx != nullptr) {
            ctx_close(ctx, -1, /*wait=*/true); // 旧壳（线程已结束）先收尾
            ::Sleep(kReopenSettleMs); // 旧 controller 浏览器侧可能仍在释放（避 0x802A000C）
        }
        ctx = start_own_window();
        if (ctx == nullptr) {
            return false;
        }
        log::info("[网页版窗口] 新开（" + ctx->key + "）；窗口表：" + [&] {
            std::lock_guard<std::mutex> lock(windowRegistry().table_mutex);
            return windowRegistry().table.report();
        }());
        if (out != nullptr) {
            *out = ctx;
        }
        return true;
    }

    const int resets_before = ctx->resets_done;
    const ResetAction action = plan_reset(/*opened_now=*/false,
                                          /*reused_external=*/!ctx->used_before,
                                          /*failed_before=*/ctx->failed_before,
                                          /*resets_done=*/ctx->resets_done);
    if (action == ResetAction::NavigateToSession) {
        // `PW-08`（`D-35③` 修订 2026-10-17）：**首选** = 同一控制器「导航回新会话页」
        //（login_url 即新会话页；关窗后同毫秒重开会撞 0x802A000C 竞态，故不复用关窗重开）
        const bool failure_driven = ctx->failed_before;
        log::info("[网页版窗口] 复位 " + reset_action_name(action) + "：" + ctx->key +
                  (failure_driven ? "（上一次未就绪 ⇒ 重试前复位）" : "（首次使用 ⇒ 回到新会话页）"));
        ctx->resets_done = resets_before + 1; // 单次重试预算：跨「复位」也要记账
        std::string nav_error;
        if (navigate_now(ctx.get(), ctx->request.url, kResetNavigateTimeoutMs, &nav_error)) {
            log::info("[网页版窗口] 复位导航完成：" + ctx->key +
                      "（已回到新会话页；就绪确认由调用方 wait_ready(L1→L2→L3) 完成）");
        }
        else {
            log::warn("[网页版窗口] 复位导航失败：" + ctx->key + "（" + nav_error +
                      "；记 failed_before，下一次调用再复位一次）");
            ctx->failed_before = true;
        }
    }
    else if (action == ResetAction::CloseAndReopen) {
        // 兜底（罕见：导航不可行 / 窗口需真正重建）—— 带 `kReopenSettleMs` 避让 0x802A000C 竞态
        const bool failure_driven = ctx->failed_before;
        log::info("[网页版窗口] 复位 " + reset_action_name(action) + "：" + ctx->key +
                  (failure_driven ? "（上一次未就绪 ⇒ 重试前复位）" : "（首次使用 ⇒ 回到新会话页）"));
        ctx_close(ctx, -1, /*wait=*/true);
        ::Sleep(kReopenSettleMs); // 等旧 controller 浏览器侧完全释放（否则重开撞 0x802A000C）
        ctx = start_own_window();
        if (ctx == nullptr) {
            return false;
        }
        ctx->resets_done = resets_before + 1; // 单次重试预算：跨「重开」也要记账
    }
    ctx->used_before = true;
    if (out != nullptr) {
        *out = ctx;
    }
    return true;
}

void requestProtocolProbe()
{
    const std::shared_ptr<WindowCtx> ctx = ctx_latest_alive();
    if (ctx == nullptr) {
        log::warn("[网页版探测] 没有已打开的登录窗口：请先点「打开登录窗口（WebView2）」并完成登录");
        return;
    }
    ctx_post_probe(ctx);
}

// 当前窗口所属站点（origin；无窗口 = 空）—— **按归属**（`PW-02`：不再是「进程里那一个窗口」）
std::string currentWindowSite(const WebOwner& owner, const std::string& site_key)
{
    const std::shared_ptr<WindowCtx> ctx = ctx_find(owner, site_key);
    if (!ctx_alive(ctx)) {
        return {};
    }
    return ctx->site_key;
}

// 该归属在**指定站点**是否有活着的窗口（界面判定 / 调用前置）
bool windowOnSite(const WebOwner& owner, const std::string& site)
{
    if (site.empty()) {
        return false;
    }
    const std::shared_ptr<WindowCtx> ctx = ctx_find(owner, site);
    return ctx_alive(ctx) && ctx->site_key == site;
}

// 窗口表诊断行（`PW-02`：谁开着哪些窗口 —— 日志 / 断言 / 界面可读）
std::string windowsReport()
{
    std::lock_guard<std::mutex> lock(windowRegistry().table_mutex);
    return windowRegistry().table.report();
}

// 关闭**某归属**的窗口（显式收尾 / 换站点用；不碰别的归属）
void closeOwnerWindow(const WebOwner& owner, const std::string& site_key)
{
    const std::shared_ptr<WindowCtx> ctx = ctx_find(owner, site_key);
    if (ctx == nullptr) {
        return;
    }
    log::info("[网页版窗口] 显式关闭 " + ctx->key);
    ctx_close(ctx, -1, /*wait=*/true);
}

// 兼容路径（不指定站点）：改造前语义 —— 最近打开的窗口的站点 / 内置默认站点
//  * `PW-02`：**不再**读「进程里那一个窗口」；改为「最近打开且活着的窗口」（无则内置默认站点）

} // namespace aiwrite::web
