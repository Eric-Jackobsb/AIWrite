// ============================================================================
//  webview_host_session.cpp — 按站点会话：ensure_session / logout_site / 回执 / 上传 / CDP 输入
//  * ARCH-Z1b：自 3514 行单文件 `webview_host.cpp` 逐字拆出，**零语义变化**
//  * 模块内契约见 `webview_host_detail.h`；公开 API 见 `webview_host.h`
// ============================================================================

#include "webview_host_detail.h"
#include "log.h"
#include "ready_level.h"
#include "probe_verdict.h"

namespace aiwrite::web {

bool ensureSession(int timeout_ms, std::string* error)
{
    // 已有凭证 → 幂等返回（默认槽 = 最近写入的站点）
    if (!SessionStore::instance().snapshot().user_token.empty()) {
        return true;
    }

    LoginRequest request; // 默认 = 内置默认站点（DeepSeek）
    if (const std::shared_ptr<WindowCtx> ctx = ctx_latest_alive(); ctx != nullptr &&
                                                                   !ctx->request.url.empty()) {
        // 有活窗口：沿用其站点与站点参数（原地补探测，不另开窗）
        request.url             = ctx->request.url;
        request.window_title    = ctx->request.window_title;
        request.profile_dir     = ctx->request.profile_dir;
        request.provider_id     = ctx->request.provider_id;
        request.probe_paths     = ctx->request.probe_paths;
        request.challenge_path  = ctx->request.challenge_path;
        request.completion_path = ctx->request.completion_path;
        request.token_expr      = ctx->request.token_expr;
    }
    return ensureSession(request, timeout_ms, error);
}

// M_patchB L1 续（PB2-17/18/19）：按**站点**确保凭证 + 取用**本归属自己的**窗口
//  * `PW-02`（`I24`）**核心修正**：站点不同 ⇒ **不再关别人的窗口**，而是**开/取该站点自己的**窗口
//    （真机 2026-10-06 19:39:12 那条「串行复用：先关闭旧窗口」由此消失）
//  * `PW-05`：失败文案带 `owner` 标签（谁要的）+ 站点 + 可操作下一步
bool ensureSession(const LoginRequest& site_request, int timeout_ms, std::string* error)
{
    const std::string site = siteKeyOf(site_request.url);
    // ---- M_patchB L4（PB2-27 / 决策 D-27 / 不变量 I15）：**站点无关**的就绪判据 ----
    //  * 内置协议站点（probe_applicable=true，如 deepseek-web）：仍需内存 userToken
    //    （页面内 PoW / 端点调用依赖它 → 逐字保持改造前语义，守 I2）
    //  * DOM 站点（probe_applicable=false）：登录态由浏览器 profile 维持 →
    //    **只看该 origin 有没有 Cookie**，绝不因「内存里没有 userToken」空等到超时（AB2-20）
    const bool needs_token = site_request.probe_applicable;
    const auto ready       = [&site, needs_token]() {
        if (needs_token) {
            return SessionStore::instance().hasToken(site);
        }
        return !SessionStore::instance().snapshot(site).cookies.empty();
    };
    const DWORD       deadline =
        ::GetTickCount() + static_cast<DWORD>(timeout_ms > 0 ? timeout_ms : 30000);
    const std::string where = site.empty() ? std::string("(默认)") : site;

    std::shared_ptr<WindowCtx> ctx        = ctx_find(site_request.owner, site);
    const bool                 window_ready = ctx_alive(ctx);
    if (!window_ready) {
        if (ctx != nullptr) {
            ctx_close(ctx, -1, /*wait=*/true); // 旧壳（线程已结束）先收尾
            ::Sleep(kReopenSettleMs); // 旧 controller 浏览器侧可能仍在释放（避 0x802A000C）
        }
        // 起（或重建）**本归属自己的**离屏窗口；`probe_after_load` 会刷新 Cookie 与 userToken
        LoginRequest request     = site_request;
        request.offscreen        = true;   // 离屏可见窗口（完全隐藏时 WebView2 不创建控制器）
        request.timeout_seconds  = 0;
        request.probe_after_load = needs_token; // L4：DOM 站点不触发协议探测（探测不适用，I16）
        ctx = ctx_start(site_request.owner, request, error);
        if (ctx == nullptr) {
            return false;
        }
        log::info("[网页版会话] " + owner_tag(site_request.owner) + " 站点 " + where +
                  (ready() ? (needs_token
                                  ? " 已有凭证但该归属尚无该站点窗口：已开**自己的**窗口（独立窗口，I24）"
                                  : " 已有 Cookie 但该归属尚无该站点窗口：已开**自己的**窗口（独立窗口，I24）")
                           : " 内存无会话：已离屏启动**本归属自己的**登录窗口（页面加载后自动提取该站点 Cookie" +
                                 std::string(needs_token ? "并探测协议）" : "）")));
    }
    else if (ready()) {
        return true; // 幂等：窗口与站点态都已就绪
    }
    else if (needs_token) {
        // 窗口已在该站点但没有凭证（用户手动开的窗口不会自动探测 / 上次探测失败）→ 显式补一次探测
        ctx_post_probe(ctx);
        log::info("[网页版会话] 站点 " + where +
                  " 内存无凭证：复用**本归属自己的**已开窗口，已请求一次协议探测");
    }
    else {
        // L4：DOM 站点 —— 不适用协议探测；窗口自身会定时复读 Cookie（登录后自动变「已登录」）
        log::info("[网页版会话] 站点 " + where +
                  " 尚未读到 Cookie：复用本归属自己的已开窗口（DOM 站点不适用协议探测，等待登录后自动复读）");
    }

    // ---- 等站点就绪（内置站点：页面加载后自动探测 / 每 6 秒补一次；DOM 站点：等 Cookie 复读）----
    const DWORD retry_ms   = 6000; // 每 6 秒补一次探测（登录动作可能发生在页面加载之后）
    int         probes     = window_ready ? 1 : 0;
    DWORD       next_retry = ::GetTickCount() + retry_ms;

    while (::GetTickCount() < deadline) {
        if (ready()) {
            if (needs_token) {
                log::info("[网页版会话] 站点 " + where + " 已取得内存凭证（userToken 长度 " +
                          std::to_string(SessionStore::instance().snapshot(site).user_token.size()) +
                          "，仅内存；路径=" + (window_ready ? "复用已开窗口补探测" : "离屏开窗探测") + "）");
            }
            else {
                log::info("[网页版会话] 站点 " + where + " 已读到该站点 Cookie（" +
                          std::to_string(SessionStore::instance().snapshot(site).cookies.size()) +
                          " 条；DOM 站点不依赖内存 userToken，仅内存，程序退出即销毁）");
            }
            return true;
        }
        // 窗口被关掉（用户手动关 / 复位）⇒ 不再空等：如实返回，由调用方按归因处理
        if (!ctx_alive(ctx)) {
            if (error != nullptr) {
                *error = owner_tag(site_request.owner) + " 站点 " + where +
                         " 的窗口已关闭（无法继续等待凭证）：请重新运行；若反复出现请检查窗口是否被手动关闭";
            }
            return false;
        }
        const DWORD now = ::GetTickCount();
        if (needs_token && probes < 3 && now >= next_retry) {
            ++probes;
            next_retry = now + retry_ms;
            ctx_post_probe(ctx);
            log::info("[网页版会话] 站点 " + where + " 仍未取得凭证，补一次协议探测（第 " +
                      std::to_string(probes) + " 次；窗口" + (window_ready ? "已打开" : "本次新开") + "）");
        }
        ::Sleep(100);
    }

    if (error != nullptr) {
        if (needs_token) {
            *error = owner_tag(site_request.owner) + " 站点 " + where +
                     " 未取得网页版凭证（内存里没有 userToken）"
                     "：请在**该站点窗口**内点「探测网页版协议（dev）」后重试，"
                     "或运行 aiwrite.exe --web-probe 确认登录态（退出码 0 = 可用）";
        }
        else {
            // L4：站点无关文案 —— 不再提 userToken；也不要求用户去点「探测网页版协议」
            *error = owner_tag(site_request.owner) + " 站点 " + where +
                     " 未读到该站点 Cookie（浏览器 profile 里没有该站点的登录态）："
                     "请点参数面板的「打开登录窗口（WebView2）」在该站点手动登录；"
                     "该站点不适用协议探测，登录后**无需**再点「探测网页版协议」";
        }
    }
    return false;
}

// M_patchB L1 续（PB2-19）：按**站点**注销
//  * 清该站点的**内存会话**（立即生效，其他站点不受影响）
//  * 删除该站点 origin 的 **Cookie** 与 **localStorage / sessionStorage**（在窗口线程内完成）
//  * **不**删除登录 profile：删除整个 profile（= 清掉所有站点）属高级操作，由参数面板单独入口提供
//  * `PW-02`：只使用**本归属自己的**该站点窗口（不再「切过去」= 不再关别人的窗口）
bool logoutSite(const LoginRequest& site_request, int timeout_ms, std::string* error)
{
    const std::string site = siteKeyOf(site_request.url);

    // 1) 先清内存会话：UI 立刻看到「未登录」，且不影响其他站点
    SessionStore::instance().clear(site);

    // 2) 取用**本归属自己的**该站点窗口（没有就开一个，仅用于删 Cookie / localStorage）
    LoginRequest request     = site_request;
    request.offscreen        = true;
    request.timeout_seconds  = 0;
    request.probe_after_load = false; // 注销不需要探测
    std::shared_ptr<WindowCtx> ctx = ctx_find(site_request.owner, site);
    if (!ctx_alive(ctx)) {
        if (ctx != nullptr) {
            ctx_close(ctx, -1, /*wait=*/true); // 旧壳（线程已结束）先收尾
        }
        ctx = ctx_start(site_request.owner, request, error);
        if (ctx == nullptr) {
            if (error != nullptr && error->empty()) {
                *error = "按站点注销需要打开该站点窗口（站点 " + site + "）";
            }
            return false;
        }
    }

    // 3) 等控制器就绪（CookieManager 需要 `ICoreWebView2_2`）
    {
        const DWORD ready_deadline = ::GetTickCount() + 10000;
        while (ctx->window.load() != nullptr && ctx->webview == nullptr &&
               ::GetTickCount() < ready_deadline) {
            ::Sleep(100);
        }
        if (ctx->webview == nullptr) {
            ctx_close(ctx, -1, /*wait=*/true);
            if (error != nullptr) {
                *error = owner_tag(site_request.owner) +
                         " 按站点注销：WebView2 在 10 秒内未就绪（站点 " + site + "）";
            }
            return false;
        }
    }

    // 4) 窗口线程执行：删该 origin 的 Cookie + 清同源 localStorage
    {
        std::lock_guard<std::mutex> lock(ctx->logout_mutex);
        ctx->logout_done    = false;
        ctx->logout_ok      = false;
        ctx->logout_error.clear();
        ctx->logout_deleted = 0;
    }
    if (HWND window = ctx->window.load(); window != nullptr) {
        ::PostMessageW(window, kLogoutMessage, 0, 0);
    }

    bool        ok  = false;
    std::string why;
    {
        std::unique_lock<std::mutex> lock(ctx->logout_mutex);
        const auto wait_ms = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 15000);
        if (ctx->logout_cv.wait_for(lock, wait_ms, [&ctx] { return ctx->logout_done; })) {
            ok  = ctx->logout_ok;
            why = ctx->logout_error;
        }
        else {
            why = "按站点注销超时（Cookie 删除未在时限内完成）";
        }
    }

    // 5) 收尾：关闭**该归属的**窗口（该站点已登出）—— 不碰别的归属
    ctx_close(ctx, -1, /*wait=*/true);

    if (!ok && error != nullptr) {
        *error = why;
    }
    log::info("[网页版会话] 按站点注销：" + site +
              (ok ? " 完成（内存会话 + Cookie + localStorage 已清）" : " 失败（" + why + "）"));
    return ok;
}

// L3：在**已开的窗口**里执行脚本并同步等待结果（不做任何会话/开窗前置）
bool run_script_now(const std::string& script_js, int timeout_ms, std::string* json_result,
                    std::string* error)
{
    if (script_js.empty()) {
        if (error != nullptr) {
            *error = "脚本为空";
        }
        return false;
    }
    const HWND window = st().window.load();
    if (window == nullptr || st().webview == nullptr) {
        if (error != nullptr) {
            // `PW-05`：**状态问题说成状态问题**（不再劝用户「先去登录一次」—— 那是误归因 `G2`/`G6`）
            *error = web_error_text(WebErrorKind::NotReady, st().owner, st().site_key,
                                    "窗口 / 页面尚未创建完成（窗口 " + st().key + "）");
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(st().script_mutex);
        st().script_js = script_js;
        st().script_result.clear();
        st().script_error.clear();
        st().script_done = false;
    }
    ::PostMessageW(window, kRunScriptMessage, 0, 0);

    std::unique_lock<std::mutex> lock(st().script_mutex);
    const auto wait_ms = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 15000);
    if (!st().script_cv.wait_for(lock, wait_ms, [] { return st().script_done; })) {
        if (error != nullptr) {
            *error = "脚本执行超时（窗口可能未就绪或页面无响应）";
        }
        return false;
    }
    if (json_result != nullptr) {
        *json_result = st().script_result;
    }
    if (error != nullptr) {
        *error = st().script_error;
    }
    return st().script_error.empty();
}

// （`wait_page_ready` 已删除：`PW-02` 起所有前置统一走 `prepare_window` + `wait_ready(L1→L2→L3)`，
//   不再保留「等 readyState=complete」的第二条路径 —— 它与 `I25` 的三级判据重复且更弱）

// ---- M8-14 续（`PW-04` · `I26`）：网络回执**按归属** ----
//  * 回执存在**该归属自己的窗口**里 ⇒ 结构上不可能跨 owner 串（旧实现 = 一个全进程缓冲）
//  * 取用时再用 `owner_accepts` 核一次「回执归属 == 要它的归属」：
//    未绑定 / 跨归属 ⇒ **丢弃并告警**（`I26`：未绑定或跨 owner 的证据**不得**用于 `I18`）
void beginResourceCapture(const WebOwner& owner, const std::string& site_key)
{
    const std::shared_ptr<WindowCtx> ctx = ctx_find(owner, site_key);
    if (ctx == nullptr) {
        log::warn(std::string("[图片上传] begin_resource_capture：") + owner_tag(owner) +
                  " 在站点 " + site_key + " 没有窗口 ⇒ 本次采集为空（回执不跨归属，I26）");
        return;
    }
    std::lock_guard<std::mutex> lock(ctx->net_mutex);
    ctx->net_records.clear();
}

std::vector<WebResourceRecord> take_resource_capture(const WebOwner& owner,
                                                    const std::string& site_key)
{
    const std::shared_ptr<WindowCtx> ctx = ctx_find(owner, site_key);
    if (ctx == nullptr) {
        return {};
    }
    std::vector<WebResourceRecord> raw;
    {
        std::lock_guard<std::mutex> lock(ctx->net_mutex);
        raw.swap(ctx->net_records); // 增量取走（调用方累积）
    }

    std::vector<WebResourceRecord> out;
    out.reserve(raw.size());
    std::size_t rejected = 0;
    for (WebResourceRecord& record : raw) {
        if (owner_accepts(record.owner, owner)) {
            out.push_back(std::move(record));
            continue;
        }
        ++rejected; // 未绑定 / 跨归属 ⇒ 丢弃（绝不让它参与 `I18` 判定）
    }
    if (rejected > 0) {
        log::warn(std::string("[图片上传] 丢弃 ") + std::to_string(rejected) + " 条**归属不符**的网络回执（" +
                  owner_tag(owner) + " · 站点 " + site_key + "；I26：证据必须绑定产生它的归属）");
    }
    return out;
}

bool resourceCaptureReady(const WebOwner& owner, const std::string& site_key)
{
    const std::shared_ptr<WindowCtx> ctx = ctx_find(owner, site_key);
    return ctx != nullptr && ctx->net_ready;
}
bool set_file_input_files(const LoginRequest& site_request, const std::string& css_selector,
                          const std::vector<std::string>& absolute_paths, int timeout_ms,
                          std::string* detail, std::string* error)
{
    if (css_selector.empty()) {
        if (error != nullptr) {
            *error = "选择器为空（站点单元未给 attach_selector）";
        }
        return false;
    }
    if (absolute_paths.empty()) {
        if (error != nullptr) {
            *error = "文件列表为空";
        }
        return false;
    }

    // 前置：`PW-02` / `PW-08` —— 取用**本归属自己的**该站点窗口（复用 / 新建 / 首次复位）
    const std::string site_key = siteKeyOf(site_request.url);
    std::shared_ptr<WindowCtx> ctx;
    if (!prepare_window(site_request, &ctx, error)) {
        return false;
    }
    CtxScope scope(ctx.get());

    // `PW-05`：同窗口串行执行守卫
    bool      busy_ok = false;
    BusyGuard busy(*ctx, site_key, &busy_ok, error);
    if (!busy_ok) {
        return false;
    }

    // ---- M_patchC PW-03（不变量 I25）：**上传前置 = L1 + L2 + L3（含附件入口）** ----
    //  * 关键：`input[type=file]` 是**按需挂载**的 —— 未挂载时**不得**报「该站点用拖拽 / 粘贴入口」
    //    （真机 41 ms 假失败的根因，`G6`）；这里等到它挂载，否则给出「页面未就绪」的可操作原因
    const int   need_ready = kReadyUpload;
    std::string ready_error;
    const int   reached = waitReady(site_request, need_ready,
                                     std::min(timeout_ms > 0 ? timeout_ms : 20000, 20000),
                                     &ready_error);
    if (!ready_satisfied(reached, need_ready)) {
        if (error != nullptr) {
            *error = ready_error.empty()
                         ? web_error_text(WebErrorKind::NotReady, site_request.owner, site_key,
                                          "未交件（就绪未达成）")
                         : ready_error;
        }
        return false;
    }

    const HWND window = st().window.load();
    if (window == nullptr || st().webview == nullptr) {
        if (error != nullptr) {
            *error = web_error_text(WebErrorKind::NotReady, site_request.owner, site_key,
                                    "登录窗口在就绪检查后被关闭（窗口 " + st().key + "）");
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(st().files_mutex);
        st().files_selector = css_selector;
        st().files_paths    = absolute_paths;
        st().files_done     = false;
        st().files_ok       = false;
        st().files_detail.clear();
        st().files_error.clear();
    }
    ::PostMessageW(window, kSetFilesMessage, 0, 0);

    bool ok = false;
    {
        std::unique_lock<std::mutex> lock(st().files_mutex);
        const auto wait_ms = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 20000);
        if (!st().files_cv.wait_for(lock, wait_ms, [] { return st().files_done; })) {
            if (error != nullptr) {
                *error = web_error_text(WebErrorKind::NotReady, site_request.owner, site_key,
                                        "交件超时（页面可能未就绪，或选择器无匹配元素）");
            }
            ctx->failed_before = true; // `PW-08`：失败 ⇒ 下一次调用复位一次
            return false;
        }
        if (detail != nullptr) {
            *detail = st().files_detail;
        }
        if (error != nullptr) {
            *error = st().files_error;
        }
        ok = st().files_ok;
    }
    ctx->failed_before = !ok;
    return ok;
}

// ---- M_patchC（`PW-07f` · 2026-10-08 · **自 `wip/pw07n-20261008` 移植回主分支**）----
//  CDP 真实输入 / 发送（公开 API）：详见头文件同名小节
bool cdp_input_text(const LoginRequest& site_request, const std::string& text, bool press_enter,
                    int timeout_ms, std::string* detail, std::string* error)
{
    if (text.empty() && !press_enter) {
        if (error != nullptr) {
            *error = "CDP 真实输入：text 与 press_enter 均为空";
        }
        return false;
    }

    // 前置：`PW-02` / `PW-08` —— 取用**本归属自己的**该站点窗口（复用 / 新建 / 首次复位）
    const std::string site_key = siteKeyOf(site_request.url);
    std::shared_ptr<WindowCtx> ctx;
    if (!prepare_window(site_request, &ctx, error)) {
        return false;
    }
    CtxScope scope(ctx.get());

    bool      busy_ok = false;
    BusyGuard busy(*ctx, site_key, &busy_ok, error);
    if (!busy_ok) {
        return false;
    }

    // ---- M_patchC PW-03（不变量 I25）：前置 = L1 窗口 + L2 页面 + L3 输入框（**不要求**附件入口）----
    const int   need_ready = kReadyScript;
    std::string ready_error;
    const int   reached = waitReady(site_request, need_ready,
                                     std::min(timeout_ms > 0 ? timeout_ms : 15000, 15000),
                                     &ready_error);
    if (!ready_satisfied(reached, need_ready)) {
        if (error != nullptr) {
            *error = ready_error.empty()
                         ? web_error_text(WebErrorKind::NotReady, site_request.owner, site_key,
                                          "CDP 真实输入未执行（就绪未达成）")
                         : ready_error;
        }
        return false;
    }

    const HWND window = st().window.load();
    if (window == nullptr || st().webview == nullptr) {
        if (error != nullptr) {
            *error = web_error_text(WebErrorKind::NotReady, site_request.owner, site_key,
                                    "登录窗口在就绪检查后被关闭（窗口 " + st().key + "）");
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(st().cdp_mutex);
        st().cdp_text   = text;
        st().cdp_enter  = press_enter;
        st().cdp_done   = false;
        st().cdp_ok     = false;
        st().cdp_detail.clear();
        st().cdp_error.clear();
    }
    ::PostMessageW(window, kCdpInputMessage, 0, 0);

    bool ok = false;
    {
        std::unique_lock<std::mutex> lock(st().cdp_mutex);
        const auto wait_ms = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 15000);
        if (!st().cdp_cv.wait_for(lock, wait_ms, [] { return st().cdp_done; })) {
            if (error != nullptr) {
                *error = web_error_text(WebErrorKind::NotReady, site_request.owner, site_key,
                                        "CDP 真实输入超时");
            }
            ctx->failed_before = true;
            return false;
        }
        if (detail != nullptr) {
            *detail = st().cdp_detail;
        }
        if (error != nullptr) {
            *error = st().cdp_error;
        }
        ok = st().cdp_ok;
    }
    ctx->failed_before = !ok;
    return ok;
}

} // namespace aiwrite::web
