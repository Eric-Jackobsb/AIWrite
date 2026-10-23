// ============================================================================
//  webview_host.cpp — 窗口运行时状态 / 窗口表 / 通用工具（模块内契约的定义点）
//  * ARCH-Z1b：自 3514 行单文件 `webview_host.cpp` 逐字拆出，**零语义变化**
//  * 模块内契约见 `webview_host_detail.h`；公开 API 见 `webview_host.h`
// ============================================================================

#include "webview_host_detail.h"
#include "log.h"
#include "paths.h"

namespace aiwrite::web {

// ============================================================================
//  M_patchC（`PW-02` / `PW-04` / `PW-06` · 不变量 `I24` / `I26`）：**窗口运行时状态**
//
//  一个 `WindowCtx` = **一个窗口** = **一个 owner 在一个站点上的**窗口（含它自己的
//  脚本槽 / 上传槽 / CDP 槽 / 网络回执 / 就绪缓存 / controller）—— 这就是 `I26`
//  「证据归属」的物理形态：**证据与产生它的窗口同槽** ⇒ 结构上不可能跨 owner 串。
//
//  取用规则由 `web/window_table.h`（纯函数）决定：
//    同 owner + 同站点 ⇒ 复用 · 同站点不同 owner ⇒ **各自独立窗口** · 跨站点**不关旧窗**
//  「谁调用谁关」在本文件**已删除**（真机 2026-10-06 19:39:12 那条
//  「切换站点…串行复用：先关闭旧窗口」从此**不应再出现**）。
// ============================================================================

WindowRegistry& windowRegistry()
{
    static WindowRegistry registry;
    return registry;
}
EnvCache& envCache()
{
    static EnvCache cache;
    return cache;
}
WindowCtx*& tlsWindow()
{
    thread_local WindowCtx* current = nullptr;
    return current;
}
void forget_ready()
{
    std::lock_guard<std::mutex> lock(st().ready_mutex);
    st().ready_site.clear();
    st().ready_mask = 0;
    st().ready_tick = 0;
}

std::wstring to_wide(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(),
                          size);
    return result;
}

std::string to_utf8(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(),
                          size, nullptr, nullptr);
    return result;
}

// GUI 程序（/SUBSYSTEM:WINDOWS）默认没有控制台：自检时挂到父进程控制台，便于自动化取输出。
// 注意：若 stdout 已被重定向（管道 / 文件），必须保留原句柄，不能抢成 CONOUT$，
//       否则 `Start-Process -RedirectStandardOutput` 之类的捕获会拿不到任何输出。
void attach_parent_console()
{
    const HANDLE existing = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (existing != nullptr && existing != INVALID_HANDLE_VALUE) {
        return;
    }
    if (::AttachConsole(ATTACH_PARENT_PROCESS) == FALSE && ::GetConsoleWindow() == nullptr) {
        return; // 没有父控制台：仅写日志文件
    }
    FILE* stream = nullptr;
    if (freopen_s(&stream, "CONOUT$", "w", stdout) != 0) {
        return;
    }
    FILE* error_stream = nullptr;
    (void)freopen_s(&error_stream, "CONOUT$", "w", stderr);
}

void resize_webview()
{
    HWND window = st().window.load();
    if (st().controller == nullptr || window == nullptr) {
        return;
    }
    RECT bounds{};
    ::GetClientRect(window, &bounds);
    st().controller->put_Bounds(bounds);
}

// 写会话：按**站点**（origin）归档 —— 多站点互不覆盖（PB2-18）
void write_session(const std::vector<Cookie>& cookies)
{
    Session session;
    session.url         = st().request.url;
    session.cookies     = cookies;
    session.site        = siteKeyOf(st().request.url);
    session.provider_id = st().request.provider_id;
    SessionStore::instance().set(std::move(session));
}

void extract_cookies(const char* reason)
{
    if (st().webview == nullptr) {
        return;
    }

    ComPtr<ICoreWebView2_2> webview2;
    if (FAILED(st().webview.As(&webview2)) || webview2 == nullptr) {
        log::error("[网页版登录] 当前 WebView2 运行时过旧，不支持 CookieManager（请更新 Runtime）");
        return;
    }

    ComPtr<ICoreWebView2CookieManager> manager;
    if (FAILED(webview2->get_CookieManager(&manager)) || manager == nullptr) {
        log::error("[网页版登录] 获取 ICoreWebView2CookieManager 失败");
        return;
    }

    const std::wstring target_url = to_wide(st().request.url);
    const HRESULT      hr         = manager->GetCookies(
        target_url.c_str(),
        Callback<ICoreWebView2GetCookiesCompletedHandler>(
            [reason](HRESULT result, ICoreWebView2CookieList* list) -> HRESULT {
                if (FAILED(result) || list == nullptr) {
                    log::warn("[网页版登录] Cookie 提取失败（HRESULT 异常）");
                    return S_OK;
                }

                UINT count = 0;
                list->get_Count(&count);

                std::vector<Cookie> cookies;
                cookies.reserve(count);
                std::string summary;
                for (UINT i = 0; i < count; ++i) {
                    ComPtr<ICoreWebView2Cookie> cookie;
                    if (FAILED(list->GetValueAtIndex(i, &cookie)) || cookie == nullptr) {
                        continue;
                    }

                    LPWSTR raw_name  = nullptr;
                    LPWSTR raw_value = nullptr;
                    cookie->get_Name(&raw_name);
                    cookie->get_Value(&raw_value);

                    BOOL   http_only  = FALSE;
                    BOOL   is_session = FALSE;
                    double expires    = 0.0;
                    cookie->get_IsHttpOnly(&http_only);
                    cookie->get_IsSession(&is_session);
                    cookie->get_Expires(&expires);

                    Cookie entry;
                    entry.name      = to_utf8(raw_name != nullptr ? raw_name : L"");
                    entry.value     = to_utf8(raw_value != nullptr ? raw_value : L"");
                    entry.http_only = http_only != FALSE;
                    entry.session   = is_session != FALSE;
                    entry.expires   = expires;
                    cookies.push_back(std::move(entry));

                    if (!summary.empty()) {
                        summary += "，";
                    }
                    summary += cookies.back().name + "=" + maskValue(cookies.back().value);

                    if (raw_name != nullptr) {
                        ::CoTaskMemFree(raw_name);
                    }
                    if (raw_value != nullptr) {
                        ::CoTaskMemFree(raw_value);
                    }
                }

                st().cookieCount = static_cast<unsigned>(cookies.size());
                st().cookies_done = true;
                if (st().first_cookie_tick.load() == 0) {
                    st().first_cookie_tick = ::GetTickCount();
                }
                write_session(cookies);
                log::info(std::string("[网页版登录] Cookie 已获取（") + reason +
                          "）：共 " + std::to_string(cookies.size()) + " 条，" + summary);
                return S_OK;
            })
            .Get());

    if (FAILED(hr)) {
        log::error("[网页版登录] 调用 GetCookies 失败 HRESULT=" + std::to_string(hr));
    }
}

// 关闭窗口：code >= 0 时使用给定退出码，否则按"是否拿到 Cookie"判定（有=0，无=1）
void close_window(int code)
{
    st().result = (code >= 0) ? code : ((st().cookieCount.load() > 0) ? 0 : 1);
    HWND window = st().window.load();
    if (window != nullptr) {
        ::KillTimer(window, kTimerId);
        ::DestroyWindow(window);
    }
}

// ---------------------------------------------------------------- 窗口表 ----
//  `PW-02`（`I24`）：**唯一**的开窗 / 关窗入口 —— 任何「先关旧窗再开新窗」的调用点都已被删除
//   * 取窗：按 `(owner, 站点)`；**找不到就返回空**（不新建；新建只由本文件的
//     `ensure_session` / 显式「打开登录窗口」/ `prepare_window` 做）
//   * 新建：登记窗口表 + 起**该窗口自己的**线程（不碰任何别的窗口）
//   * 关闭：请求关闭（可等线程结束）+ 从表里删除（**只关自己的**）
//   * 收尾：`ctx_close_all()` 按窗口表给出的顺序（后开的先关）逐窗口关闭

int run_login_window(const LoginRequest& request); // 线程体（定义在后面）

std::shared_ptr<WindowCtx> ctx_find(const WebOwner& owner, const std::string& site_key)
{
    const std::string           key = window_key(owner, site_key);
    std::lock_guard<std::mutex> lock(windowRegistry().table_mutex);
    const auto                  it = windowRegistry().windows.find(key);
    return it == windowRegistry().windows.end() ? nullptr : it->second;
}

bool ctx_alive(const std::shared_ptr<WindowCtx>& ctx)
{
    return ctx != nullptr && ctx->running.load();
}

// 窗口线程体（绑定 `tlsWindow()` → 跑 `run_login_window` → 回写结果 / `running`）
void window_thread_main(std::shared_ptr<WindowCtx> holder, LoginRequest request)
{
    CtxScope scope(holder.get());
    const int code = run_login_window(request);
    holder->result  = code;
    holder->running = false;
    {
        std::lock_guard<std::mutex> lock(holder->status_mutex);
        switch (code) {
        case 0:
            holder->status = "登录窗口已关闭（会话 Cookie 已保存在内存）";
            break;
        case 2:
            holder->status = "登录窗口已关闭（超时，未取得 Cookie）";
            break;
        default:
            holder->status = "登录窗口已关闭（未取得有效会话）";
            break;
        }
    }
    log::info("[网页版窗口] 线程结束 " + holder->key + "，退出码 " + std::to_string(code));
}

// 起一条**新窗口**（登记 + 线程）；失败 → 返回空 + `error`（不静默）
std::shared_ptr<WindowCtx> ctx_start(const WebOwner& owner, LoginRequest request,
                                     std::string* error)
{
    const std::string site = siteKeyOf(request.url);
    auto              ctx  = std::make_shared<WindowCtx>();
    ctx->owner    = owner;
    ctx->site_key = site;
    ctx->key      = window_key(owner, site);
    request.owner = owner;
    if (request.profile_dir.empty()) {
        request.profile_dir = paths::webview2_profile().string();
    }
    ctx->request = request;

    std::error_code ec;
    std::filesystem::create_directories(ctx->request.profile_dir, ec);
    if (ec) {
        log::warn("[网页版登录] profile 目录创建失败: " + ctx->request.profile_dir + "（" +
                  ec.message() + "）");
    }

    {
        std::lock_guard<std::mutex> lock(windowRegistry().table_mutex);
        if (windowRegistry().windows.find(ctx->key) != windowRegistry().windows.end()) {
            if (error != nullptr) {
                *error = "该归属在该站点已有窗口（" + ctx->key + "）";
            }
            return nullptr;
        }
        windowRegistry().table.note_opened(owner, site);
        if (const WindowEntry* entry = windowRegistry().table.find(owner, site); entry != nullptr) {
            ctx->seq = entry->seq; // 诊断序号（收尾顺序由窗口表给出）
        }
        windowRegistry().windows.emplace(ctx->key, ctx);
    }

    ctx->running = true;
    try {
        ctx->thread = std::thread(window_thread_main, ctx, ctx->request);
    }
    catch (const std::exception& ex) {
        ctx->running = false;
        {
            std::lock_guard<std::mutex> lock(windowRegistry().table_mutex);
            windowRegistry().windows.erase(ctx->key);
            windowRegistry().table.note_closed(owner, site);
        }
        if (error != nullptr) {
            *error = std::string("创建登录线程失败（") + ctx->key + "）: " + ex.what();
        }
        return nullptr;
    }

    log::info("[网页版窗口] 开窗 " + ctx->key + "（profile " + ctx->request.profile_dir +
              "；窗口表：" + windowRegistry().table.report() + "）");
    return ctx;
}

// 关**自己的**窗口：请求关闭（可选等线程结束）+ 从窗口表移除
void ctx_close(const std::shared_ptr<WindowCtx>& ctx, int code, bool wait)
{
    if (ctx == nullptr) {
        return;
    }
    ctx->stop = true;
    if (const HWND window = ctx->window.load(); window != nullptr) {
        if (code >= 0) {
            ctx->result = code;
        }
        ::PostMessageW(window, WM_CLOSE, 0, 0);
    }
    if (wait && ctx->thread.joinable()) {
        ctx->thread.join();
    }
    std::lock_guard<std::mutex> lock(windowRegistry().table_mutex);
    windowRegistry().windows.erase(ctx->key);
    windowRegistry().table.note_closed(ctx->owner, ctx->site_key);
}

// 收尾：**逐窗口**关闭（顺序 = 窗口表给出：后开的先关）
void ctx_close_all()
{
    std::vector<std::shared_ptr<WindowCtx>> all;
    {
        std::lock_guard<std::mutex> lock(windowRegistry().table_mutex);
        for (const std::string& key : windowRegistry().table.keys_in_close_order()) {
            const auto it = windowRegistry().windows.find(key);
            if (it != windowRegistry().windows.end()) {
                all.push_back(it->second);
            }
        }
    }
    for (const auto& ctx : all) {
        ctx_close(ctx, -1, /*wait=*/true);
    }
}


} // namespace aiwrite::web
