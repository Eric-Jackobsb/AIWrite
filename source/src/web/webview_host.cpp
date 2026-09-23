#include "web/webview_host.h"

#include "utils/log.h"
#include "utils/paths.h"
#include "web/session_store.h"

#include <windows.h>

#include <wrl/client.h> // Microsoft::WRL::ComPtr
#include <wrl/event.h>  // Microsoft::WRL::Callback

#include <WebView2.h>

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

// ---- 登录线程内的运行时状态（同一时刻只允许一个登录窗口）----
LoginRequest          g_request;   // start() 写入，线程只读
std::atomic<HWND>     g_window{nullptr};
std::atomic<bool>     g_stop{false};
std::atomic<int>      g_result{-1};
std::atomic<unsigned> g_cookie_count{0};
std::atomic<bool>     g_cookies_done{false};
std::atomic<DWORD>    g_first_cookie_tick{0};
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
