#include "web/webview_host.h"

#include "utils/log.h"
#include "utils/paths.h"
#include "web/session_store.h"

#include <windows.h>

#include <wrl/client.h> // Microsoft::WRL::ComPtr
#include <wrl/event.h>  // Microsoft::WRL::Callback

#include <WebView2.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <utility>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace aiwrite::web {
namespace {

constexpr const wchar_t* kWindowClass   = L"AIwriteWebLoginWindow";
constexpr UINT_PTR       kTimerId       = 1;
constexpr UINT           kTimerTickMs   = 500;   // 定时器节拍
constexpr DWORD          kCookiePollMs  = 3000;  // 登录期间重取 Cookie 的间隔
constexpr DWORD          kAutoCloseGraceMs = 3000; // 自检：取到 Cookie 后静置多久自动收尾
constexpr int            kHotkeyExtract = 1;     // Ctrl+Alt+C：立即重新提取
constexpr int            kHotkeyClose   = 2;     // ESC：关闭窗口
constexpr UINT           kProbeMessage  = WM_APP + 1; // 请求协议探测（可在任意线程 Post）
constexpr DWORD          kProbeDelayMs  = 1500;  // 页面加载完成后再等一会儿（SPA 就绪）才探测

// 协议探测脚本（在**已登录页面内**执行：同源 fetch，可绕过跨域/反爬）
//  两段式：kickoff 注入异步任务并把结果写到 window.__aiwriteProbe；poll 轮询取回
//  （不用 ExecuteScript 的 Promise 等待：部分运行时不会 await，会拿到空的 {}）
constexpr const char* kProbeKickoffScript = R"JS(
(function () {
  window.__aiwriteProbe = { done: false, data: '' };
  const mask = (t) => (t && t.length > 8) ? (t.slice(0, 4) + '****' + t.slice(-4) + '(len=' + t.length + ')') : (t ? '****' : '');
  const scrub = (t) => (t || '').replace(/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/g, '***@***').slice(0, 400);
  (async () => {
    const out = { keys: '', hasUserToken: false, userTokenMasked: '', settingsJwtMasked: '', tokenRaw: '', challenge: '', endpoints: '', error: '' };
    let token = '';
    try {
      out.keys = Object.keys(localStorage).join(',');
      const raw = localStorage.getItem('userToken') || '';
      token = raw;
      try {                              // 新版前端把 Token 包成 JSON：{"value":"...","__version":"0"}
        const parsed = JSON.parse(raw);
        token = parsed.value || parsed.token || raw;
      } catch (e) { /* 旧版直接是字符串 */ }
      out.hasUserToken = token.length > 0;
      out.userTokenMasked = mask(token);
      out.tokenRaw = token;              // 真 Token：只回传给宿主进程内存，不写日志/文件
      out.settingsJwtMasked = mask(localStorage.getItem('settingsJwt') || '');
    } catch (e) { out.error += 'localStorage:' + e + ' | '; }
    const authHeaders = { 'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json' };
    try {
      const resp = await fetch('/api/v0/chat/create_pow_challenge', {
        method: 'POST',
        headers: authHeaders,
        body: JSON.stringify({ target_path: '/api/v0/chat/completion' })
      });
      out.challenge = (await resp.text()).slice(0, 2000);
    } catch (e) { out.error += 'challenge:' + e + ' | '; }
    const candidates = ['/api/v0/users/current', '/api/v0/chat_session/fetch_page'];
    const lines = [];
    for (const path of candidates) {
      try {
        const r = await fetch(path, { headers: { 'Authorization': 'Bearer ' + token } });
        lines.push(path + ' -> ' + r.status + ' ' + scrub(await r.text()));
      } catch (e) { lines.push(path + ' -> error ' + e); }
    }
    out.endpoints = lines.join(' || ');

    // 说明：RE 期的重量级步骤（抓取页面 JS 片段 / 调用官方 PoW worker 求 ground truth /
    //       哈希指纹）已移除，结论记录在 docs/ai_writer_nodes.md；此处保持轻量（~3 秒）。
    window.__aiwriteProbe.data = JSON.stringify(out);
    window.__aiwriteProbe.done = true;
  })();
  return 'started';
})();
)JS";

constexpr const char* kProbePollScript = R"JS(
(function () {
  const p = window.__aiwriteProbe;
  if (!p || !p.done) { return ''; }
  return p.data || '';
})();
)JS";

// ---- 登录线程内的运行时状态（同一时刻只允许一个登录窗口）----
LoginRequest          g_request;   // start() 写入，线程只读
std::atomic<HWND>     g_window{nullptr};
std::atomic<bool>     g_stop{false};
std::atomic<int>      g_result{-1};
std::atomic<unsigned> g_cookie_count{0};
std::atomic<bool>     g_cookies_done{false};
std::atomic<DWORD>    g_first_cookie_tick{0};
// 协议探测状态：0=空闲 / 1=已注入（轮询等待结果）/ 2=完成
int                   g_probe_stage   = 0;
int                   g_probe_attempt = 0;
DWORD                 g_probe_due_tick  = 0;
DWORD                 g_probe_last_poll = 0;
constexpr int         kProbeMaxAttempts = 24; // 24 × 500ms ≈ 12 秒
DWORD                 g_start_tick = 0;
DWORD                 g_last_poll  = 0;

ComPtr<ICoreWebView2Environment> g_environment;
ComPtr<ICoreWebView2Controller>  g_controller;
ComPtr<ICoreWebView2>            g_webview;

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
    HWND window = g_window.load();
    if (g_controller == nullptr || window == nullptr) {
        return;
    }
    RECT bounds{};
    ::GetClientRect(window, &bounds);
    g_controller->put_Bounds(bounds);
}

void write_session(const std::vector<Cookie>& cookies)
{
    Session session;
    session.url     = g_request.url;
    session.cookies = cookies;
    SessionStore::instance().set(std::move(session));
}

void extract_cookies(const char* reason)
{
    if (g_webview == nullptr) {
        return;
    }

    ComPtr<ICoreWebView2_2> webview2;
    if (FAILED(g_webview.As(&webview2)) || webview2 == nullptr) {
        log::error("[网页版登录] 当前 WebView2 运行时过旧，不支持 CookieManager（请更新 Runtime）");
        return;
    }

    ComPtr<ICoreWebView2CookieManager> manager;
    if (FAILED(webview2->get_CookieManager(&manager)) || manager == nullptr) {
        log::error("[网页版登录] 获取 ICoreWebView2CookieManager 失败");
        return;
    }

    const std::wstring target_url = to_wide(g_request.url);
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
                    summary += cookies.back().name + "=" + mask_value(cookies.back().value);

                    if (raw_name != nullptr) {
                        ::CoTaskMemFree(raw_name);
                    }
                    if (raw_value != nullptr) {
                        ::CoTaskMemFree(raw_value);
                    }
                }

                g_cookie_count = static_cast<unsigned>(cookies.size());
                g_cookies_done = true;
                if (g_first_cookie_tick.load() == 0) {
                    g_first_cookie_tick = ::GetTickCount();
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
    g_result = (code >= 0) ? code : ((g_cookie_count.load() > 0) ? 0 : 1);
    HWND window = g_window.load();
    if (window != nullptr) {
        ::KillTimer(window, kTimerId);
        ::DestroyWindow(window);
    }
}

// 在登录窗口内执行协议探测（必须运行在窗口线程）
//  两段式：start 注入脚本 → WM_TIMER 调 poll 轮询结果 → finish 落库（+ 可选自动关窗）
void finish_protocol_probe(ProbeResult probe);
void start_protocol_probe();
void poll_protocol_probe();

void finish_protocol_probe(ProbeResult probe)
{
    g_probe_stage = 2;

    log::info("[网页版探测] localStorage 键: " +
              (probe.local_storage_keys.empty() ? std::string("(空)") : probe.local_storage_keys));
    log::info("[网页版探测] userToken: " +
              (probe.has_user_token ? probe.user_token_masked
                                    : std::string("未获取（可能未登录）")));
    log::info("[网页版探测] settingsJwt: " +
              (probe.settings_jwt_masked.empty() ? std::string("(空)") : probe.settings_jwt_masked));
    log::info("[网页版探测] challenge: " +
              (probe.challenge_json.empty() ? std::string("(空)") : probe.challenge_json));
    log::info("[网页版探测] 端点探测: " +
              (probe.endpoints_report.empty() ? std::string("(空)") : probe.endpoints_report));
    if (!probe.error.empty()) {
        log::warn("[网页版探测] 错误: " + probe.error);
    }

    const bool ok = probe.ok;
    SessionStore::instance().set_probe(std::move(probe));

    if (g_request.auto_close_after_probe) {
        log::info("[网页版探测] 已自动收尾（关闭登录窗口）");
        close_window(ok ? 0 : 1);
    }
}

void start_protocol_probe()
{
    if (g_webview == nullptr) {
        ProbeResult probe;
        probe.error = "WebView2 尚未就绪";
        finish_protocol_probe(std::move(probe));
        return;
    }

    log::info("[网页版探测] 注入探测脚本（localStorage/userToken + create_pow_challenge）");
    const std::wstring script = to_wide(kProbeKickoffScript);
    const HRESULT      hr     = g_webview->ExecuteScript(
        script.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT result, LPCWSTR /*json_result*/) -> HRESULT {
                if (FAILED(result)) {
                    ProbeResult probe;
                    probe.error = "注入探测脚本失败 HRESULT=" + std::to_string(result);
                    finish_protocol_probe(std::move(probe));
                    return S_OK;
                }
                g_probe_stage     = 1; // 等待异步任务完成，交给 WM_TIMER 轮询
                g_probe_attempt   = 0;
                g_probe_last_poll = ::GetTickCount();
                return S_OK;
            })
            .Get());

    if (FAILED(hr)) {
        ProbeResult probe;
        probe.error = "调用 ExecuteScript 失败";
        finish_protocol_probe(std::move(probe));
    }
}

void poll_protocol_probe()
{
    if (g_probe_stage != 1 || g_webview == nullptr) {
        return;
    }
    if (++g_probe_attempt > kProbeMaxAttempts) {
        ProbeResult probe;
        probe.error = "探测超时（结果未在 " + std::to_string(kProbeMaxAttempts * 500 / 1000) +
                      " 秒内返回）";
        finish_protocol_probe(std::move(probe));
        return;
    }

    const std::wstring script = to_wide(kProbePollScript);
    const HRESULT      hr     = g_webview->ExecuteScript(
        script.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT result, LPCWSTR json_result) -> HRESULT {
                if (FAILED(result) || json_result == nullptr) {
                    return S_OK; // 下一轮继续
                }

                // ExecuteScript 把返回值编码为 JSON：未完成时是空字符串 "\"\""
                std::string inner;
                try {
                    const nlohmann::json outer = nlohmann::json::parse(to_utf8(json_result));
                    inner = outer.is_string() ? outer.get<std::string>() : std::string();
                }
                catch (const std::exception&) {
                    return S_OK;
                }
                if (inner.empty()) {
                    return S_OK; // 尚未完成
                }

                ProbeResult probe;
                try {
                    const nlohmann::json data = nlohmann::json::parse(inner);
                    probe.local_storage_keys = data.value("keys", std::string());
                    probe.has_user_token     = data.value("hasUserToken", false);
                    probe.user_token_masked  = data.value("userTokenMasked", std::string());
                    probe.settings_jwt_masked = data.value("settingsJwtMasked", std::string());
                    probe.token_raw          = data.value("tokenRaw", std::string()); // 仅内存
                    probe.challenge_json     = data.value("challenge", std::string());
                    probe.endpoints_report   = data.value("endpoints", std::string());
                    probe.error              = data.value("error", std::string());
                    probe.ok                 = true;
                }
                catch (const std::exception& ex) {
                    probe.error = std::string("探测结果解析失败: ") + ex.what();
                }
                finish_protocol_probe(std::move(probe));
                return S_OK;
            })
            .Get());

    if (FAILED(hr)) {
        ProbeResult probe;
        probe.error = "轮询探测结果失败";
        finish_protocol_probe(std::move(probe));
    }
}

HRESULT on_controller_ready(HRESULT result, ICoreWebView2Controller* controller)
{
    if (FAILED(result) || controller == nullptr) {
        log::error("[网页版登录] 创建 WebView2 控制器失败 HRESULT=" + std::to_string(result));
        g_result = 1;
        ::PostMessageW(g_window.load(), WM_CLOSE, 0, 0);
        return S_OK;
    }

    g_controller = controller;
    g_controller->get_CoreWebView2(&g_webview);
    resize_webview();
    g_controller->put_IsVisible(TRUE);

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings != nullptr) {
        settings->put_IsStatusBarEnabled(FALSE);
    }

    EventRegistrationToken token{};
    g_webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2* /*sender*/,
               ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                if (success) {
                    extract_cookies("页面加载完成");
                    if (g_request.probe_after_load) {
                        g_probe_due_tick = ::GetTickCount() + kProbeDelayMs; // SPA 就绪后再探测
                    }
                }
                else {
                    log::warn("[网页版登录] 页面导航失败（可检查网络后重试）");
                }
                return S_OK;
            })
            .Get(),
        &token);

    log::info("[网页版登录] 开始导航: " + g_request.url);
    g_webview->Navigate(to_wide(g_request.url).c_str());
    return S_OK;
}

HRESULT on_environment_ready(HRESULT result, ICoreWebView2Environment* environment)
{
    if (FAILED(result) || environment == nullptr) {
        log::error("[网页版登录] 创建 WebView2 环境失败（请确认已安装 WebView2 Runtime）HRESULT=" +
                   std::to_string(result));
        g_result = 1;
        ::PostMessageW(g_window.load(), WM_CLOSE, 0, 0);
        return S_OK;
    }

    g_environment = environment;
    const HRESULT hr = g_environment->CreateCoreWebView2Controller(
        g_window.load(),
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(&on_controller_ready)
            .Get());
    if (FAILED(hr)) {
        // 同步返回失败时回调不会触发（典型：HWND 无效 → E_INVALIDARG），必须显式报错
        log::error("[网页版登录] CreateCoreWebView2Controller 同步返回失败 HRESULT=" +
                   std::to_string(hr));
        g_result = 1;
        ::PostMessageW(g_window.load(), WM_CLOSE, 0, 0);
    }
    return S_OK;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)lparam;
    switch (message) {
    case WM_CREATE: {
        // 【关键】异步回调可能在 CreateWindowExW 返回前（嵌套消息泵）触发，必须先赋值 g_window
        g_window = window;

        const std::wstring profile = to_wide(g_request.profile_dir);
        const HRESULT      hr      = ::CreateCoreWebView2EnvironmentWithOptions(
            nullptr, profile.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                &on_environment_ready)
                .Get());
        if (FAILED(hr)) {
            log::error("[网页版登录] CreateCoreWebView2EnvironmentWithOptions 调用失败 HRESULT=" +
                       std::to_string(hr));
            g_result = 1;
            ::PostMessageW(window, WM_CLOSE, 0, 0);
        }

        g_start_tick = ::GetTickCount();
        g_last_poll  = g_start_tick;
        ::SetTimer(window, kTimerId, kTimerTickMs, nullptr);
        return 0;
    }

    case WM_SIZE:
        resize_webview();
        return 0;

    case WM_HOTKEY:
        if (wparam == kHotkeyExtract) {
            extract_cookies("用户按下 Ctrl+Alt+C");
        }
        else if (wparam == kHotkeyClose) {
            ::PostMessageW(window, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_TIMER: {
        const DWORD now = ::GetTickCount();
        if (g_stop.load()) {
            log::info("[网页版登录] 收到关闭请求，正在关闭登录窗口");
            close_window(-1);
            return 0;
        }
        if (g_request.timeout_seconds > 0 &&
            now - g_start_tick > static_cast<DWORD>(g_request.timeout_seconds) * 1000) {
            log::warn("[网页版登录] 到达超时 " + std::to_string(g_request.timeout_seconds) +
                      " 秒，自动关闭窗口");
            close_window(g_cookie_count.load() > 0 ? 0 : 2);
            return 0;
        }
        if (now - g_last_poll >= kCookiePollMs && g_webview != nullptr) {
            g_last_poll = now;
            extract_cookies("定时刷新（登录后 Cookie 会变化）");
        }
        if (g_probe_stage == 1 && now - g_probe_last_poll >= 500) {
            g_probe_last_poll = now;
            poll_protocol_probe();
        }
        if (g_probe_due_tick != 0 && now >= g_probe_due_tick) {
            g_probe_due_tick = 0;
            start_protocol_probe();
        }
        if (g_request.auto_close_after_cookies && g_cookies_done.load()) {
            const DWORD first = g_first_cookie_tick.load();
            if (first != 0 && now - first >= kAutoCloseGraceMs) {
                log::info("[网页版登录] 已取得 Cookie 且稳定 " +
                          std::to_string(kAutoCloseGraceMs / 1000) + " 秒，自动关闭登录窗口");
                close_window(0);
            }
        }
        return 0;
    }

    case kProbeMessage:
        start_protocol_probe();
        return 0;

    case WM_CLOSE:
        extract_cookies("窗口关闭前最后一次提取"); // 异步；紧接着销毁窗口
        ::DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        if (g_controller != nullptr) {
            g_controller->Close();
            g_controller = nullptr;
        }
        g_webview     = nullptr;
        g_environment = nullptr;
        g_window      = nullptr;
        if (g_result.load() < 0) {
            g_result = (g_cookie_count.load() > 0) ? 0 : 1;
        }
        ::PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

// ---------------------------------------------------------------- 线程体 ----
int run_login_window(const LoginRequest& request)
{
    const HRESULT com_hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_hr)) {
        log::error("[网页版登录] COM 初始化失败（WebView2 需要 STA）");
        return 1;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize        = sizeof(window_class);
    window_class.style         = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc   = window_proc;
    window_class.hInstance     = ::GetModuleHandleW(nullptr);
    window_class.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;

    if (::RegisterClassExW(&window_class) == 0) {
        log::error("[网页版登录] 注册窗口类失败");
        ::CoUninitialize();
        return 1;
    }

    RECT rect{0, 0, 1000, 760};
    ::AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    const int width  = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    int       x      = (::GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int       y      = (::GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    if (request.offscreen) {
        // 离屏可见窗口：完全隐藏（SW_HIDE）时 WebView2 不会创建控制器
        x = ::GetSystemMetrics(SM_CXSCREEN) + 160;
        y = 120;
    }

    HWND window = ::CreateWindowExW(0, kWindowClass, to_wide(request.window_title).c_str(),
                                    WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr, nullptr,
                                    window_class.hInstance, nullptr);
    if (window == nullptr) {
        log::error("[网页版登录] 创建登录窗口失败");
        ::UnregisterClassW(kWindowClass, window_class.hInstance);
        ::CoUninitialize();
        return 1;
    }

    ::ShowWindow(window, request.offscreen ? SW_SHOWNOACTIVATE : SW_SHOW);
    ::UpdateWindow(window);
    ::RegisterHotKey(window, kHotkeyExtract, MOD_CONTROL | MOD_ALT, 'C');
    ::RegisterHotKey(window, kHotkeyClose, 0, VK_ESCAPE);

    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    ::UnregisterHotKey(window, kHotkeyExtract);
    ::UnregisterHotKey(window, kHotkeyClose);
    ::UnregisterClassW(kWindowClass, window_class.hInstance);
    ::CoUninitialize();
    return g_result.load() < 0 ? 1 : g_result.load();
}

} // namespace

// -------------------------------------------------------- LoginWindow -------
LoginWindow::~LoginWindow()
{
    request_close();
    join();
}

bool LoginWindow::start(const LoginRequest& request, std::string* error)
{
    if (running_.load()) {
        if (error != nullptr) {
            *error = "已有登录窗口在运行";
        }
        return false;
    }
    join(); // 回收上一次的线程

    g_request = request;
    if (g_request.profile_dir.empty()) {
        g_request.profile_dir = paths::webview2_profile().string();
    }
    g_stop             = false;
    g_result           = -1;
    g_cookies_done     = false;
    g_cookie_count     = 0;
    g_first_cookie_tick = 0;
    g_probe_due_tick   = 0;
    g_probe_stage      = 0;
    g_probe_attempt    = 0;
    g_window           = nullptr;

    std::error_code ec;
    std::filesystem::create_directories(g_request.profile_dir, ec);
    if (ec) {
        log::warn("[网页版登录] profile 目录创建失败: " + g_request.profile_dir + "（" +
                  ec.message() + "）");
    }

    set_status("正在打开登录窗口…");
    running_ = true;
    try {
        thread_ = std::thread(&LoginWindow::run_thread, this, g_request);
    }
    catch (const std::exception& ex) {
        running_ = false;
        if (error != nullptr) {
            *error = std::string("创建登录线程失败: ") + ex.what();
        }
        return false;
    }

    log::info("[网页版登录] 登录窗口已启动（" + g_request.url + "，profile " +
              g_request.profile_dir + "）");
    return true;
}

void LoginWindow::request_close()
{
    g_stop      = true;
    HWND window = g_window.load();
    if (window != nullptr) {
        ::PostMessageW(window, WM_CLOSE, 0, 0);
    }
}

void LoginWindow::join()
{
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::string LoginWindow::status() const
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

void LoginWindow::set_status(const std::string& text)
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_ = text;
}

void LoginWindow::run_thread(LoginRequest request)
{
    const int code = run_login_window(request);
    result_        = code;
    running_       = false;
    switch (code) {
    case 0:
        set_status("登录窗口已关闭（会话 Cookie 已保存在内存）");
        break;
    case 2:
        set_status("登录窗口已关闭（超时，未取得 Cookie）");
        break;
    default:
        set_status("登录窗口已关闭（未取得有效会话）");
        break;
    }
    log::info("[网页版登录] 登录窗口线程结束，退出码 " + std::to_string(code));
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

    LoginWindow window;
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
                    mask_value(cookie.value).c_str(), cookie.http_only ? 1 : 0,
                    cookie.session ? 1 : 0);
    }
    std::printf("====================================\n");

    const int   code    = window.result();
    const bool  pass    = (code == 0) && session.cookie_count() > 0;
    const char* verdict = pass ? "PASS" : (code == 2 ? "TIMEOUT" : "FAIL");
    std::printf("[网页版登录] 自检结束：%s（退出码 %d，Cookie %zu 条，url=%s）\n", verdict, code,
                session.cookie_count(), request.url.c_str());
    log::info(std::string("[网页版登录] 自检结束：") + verdict + "（Cookie " +
              std::to_string(session.cookie_count()) + " 条，url=" + request.url + "）");

    SessionStore::instance().clear(); // 自检不留会话
    return pass ? 0 : (code == 2 ? 2 : 1);
}

LoginWindow& login_window()
{
    // 故意用裸 new：不在退出期析构，避免静态析构顺序导致的线程/日志竞态。
    // 生命周期由 stop_login_window() 显式收尾。
    static LoginWindow* instance = new LoginWindow();
    return *instance;
}

void request_protocol_probe()
{
    HWND window = g_window.load();
    if (window == nullptr) {
        log::warn("[网页版探测] 登录窗口未打开：请先点「打开登录窗口（WebView2）」并完成登录");
        return;
    }
    log::info("[网页版探测] 已请求（在登录窗口内执行）");
    ::PostMessageW(window, kProbeMessage, 0, 0);
}

int protocol_probe(int timeout_seconds)
{
    attach_parent_console();

    const int timeout = timeout_seconds > 0 ? timeout_seconds : 30;
    log::info("[网页版探测] 自检开始（离屏窗口，超时 " + std::to_string(timeout) + " 秒）");

    LoginRequest request;
    request.offscreen              = true;
    request.timeout_seconds        = timeout;
    request.probe_after_load       = true; // 页面加载完成后自动探测
    request.auto_close_after_probe = true; // 探测完成即关窗退出

    LoginWindow window;
    std::string error;
    if (!window.start(request, &error)) {
        std::printf("[网页版探测] 启动失败: %s\n", error.c_str());
        return 1;
    }
    window.join();

    const Session      session = SessionStore::instance().snapshot();
    const ProbeResult& probe   = session.probe;

    std::printf("\n===== 网页版协议探测（脱敏）=====\n");
    std::printf("localStorage 键 : %s\n", probe.local_storage_keys.c_str());
    std::printf("userToken      : %s\n",
                probe.has_user_token ? probe.user_token_masked.c_str() : "未获取（可能未登录）");
    std::printf("settingsJwt    : %s\n", probe.settings_jwt_masked.c_str());
    std::printf("challenge      : %s\n", probe.challenge_json.c_str());
    std::printf("端点探测       : %s\n", probe.endpoints_report.c_str());
    std::printf("错误           : %s\n", probe.error.c_str());
    std::printf("================================\n");

    const bool pass = probe.ok && probe.has_user_token;
    if (probe.ok && !probe.has_user_token) {
        std::printf("[网页版探测] 提示：未取到 userToken → 请先在登录窗口完成一次登录"
                    "（`aiwrite.exe --login-selftest` 或界面参数面板的「打开登录窗口」）\n");
    }
    std::printf("[网页版探测] 结果：%s（退出码 %d）\n", pass ? "PASS" : "FAIL", pass ? 0 : 1);
    log::info("[网页版探测] 自检结束：" + probe.summary());

    SessionStore::instance().clear(); // 自检不留会话
    return pass ? 0 : 1;
}

void stop_login_window()
{
    LoginWindow& window = login_window();
    if (!window.running()) {
        return;
    }
    window.request_close();
    window.join();
}

} // namespace aiwrite::web
