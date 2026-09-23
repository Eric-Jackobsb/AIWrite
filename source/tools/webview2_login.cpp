// =============================================================================
//  webview2_login —— M1 技术验证工具（M1-04 / V-03）
//
//  功能：
//    1) 用 WebView2 打开目标站点（默认 https://chat.deepseek.com/）
//    2) 用户手动登录（人工步骤，无法自动完成）
//    3) 提取该站点 Cookie，仅打印「名称 + 脱敏值 + 属性」，不落盘
//    4) 关闭窗口即销毁 WebView2 与提取到的数据
//
//  用法：
//    webview2_login                     # 交互：使用 ~/.brain-ai/webview2 profile（保留登录态）
//    webview2_login --ephemeral         # 交互：使用临时 profile，退出即丢弃登录态
//    webview2_login --selftest          # 自检：隐藏窗口自动跑「导航 → 提取 Cookie」全链路
//    webview2_login --selftest --url https://www.bing.com/ --timeout 45
//    webview2_login --hidden            # 交互模式但不显示窗口（配合 --timeout 便于自动化）
//
//  选项：
//    --selftest        自检模式：退出码 0=通过（Cookie≥1）/ 1=失败 / 2=超时；结果写入 app.log
//    --url <url>       目标站点（默认 https://chat.deepseek.com/）
//    --timeout <秒>    自动退出超时（自检默认 30 秒；0 = 不自动退出）
//    --hidden          窗口隐藏（不抢焦点）
//    --ephemeral       使用临时浏览器 profile，退出即丢弃登录态
//
//  快捷键（交互模式）：Ctrl+Alt+C 立即提取 Cookie，ESC 关闭窗口
// =============================================================================
#include "utils/log.h"
#include "utils/paths.h"

#include <windows.h>

#include <wrl/client.h> // Microsoft::WRL::ComPtr
#include <wrl/event.h>  // Microsoft::WRL::Callback

#include <WebView2.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"AIwriteWebView2Login";
constexpr wchar_t kWindowTitle[] =
    L"AIwrite · DeepSeek 网页登录（登录后按 Ctrl+Alt+C 提取 Cookie，ESC 退出）";
constexpr int     kHotkeyExtractCookies = 1;
constexpr int     kHotkeyClose          = 2;
constexpr UINT_PTR kTimerId             = 1;

// ---------------------------------------------------------------- 运行选项 ----
struct Options {
    bool        ephemeral       = false;
    bool        selftest        = false;   // 自动自检（隐藏窗口 + 超时 + 退出码）
    bool        hidden          = false;   // 不显示窗口
    int         timeout_seconds = 0;       // 0 = 不自动退出
    std::string url             = "https://chat.deepseek.com/";
};

Options g_options;

// ---------------------------------------------------------------- 自检状态 ----
int      g_exit_code       = 0;   // 自检/超时设置的进程退出码（WM_DESTROY 使用）
bool     g_navigation_done = false;
bool     g_navigation_ok   = false;
bool     g_cookies_done    = false;
unsigned g_cookie_count    = 0;
DWORD    g_start_tick      = 0;

HINSTANCE g_instance = nullptr;
HWND      g_window   = nullptr;

ComPtr<ICoreWebView2Environment> g_environment;
ComPtr<ICoreWebView2Controller>  g_controller;
ComPtr<ICoreWebView2>            g_webview;

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

// 脱敏：只保留前 4 位与后 4 位
std::string mask_value(const std::string& value)
{
    const std::size_t length = value.size();
    if (length == 0) {
        return "(空)";
    }
    if (length <= 8) {
        return std::string(length, '*') + "(len=" + std::to_string(length) + ")";
    }
    return value.substr(0, 4) + "****" + value.substr(length - 4) +
           "(len=" + std::to_string(length) + ")";
}

void print_usage()
{
    std::printf("webview2_login - AIwrite M1 技术验证工具（M1-04 / V-03）\n");
    std::printf("用法:\n");
    std::printf("  webview2_login                              交互：打开站点并手动登录\n");
    std::printf("  webview2_login --ephemeral                  交互：临时 profile，退出即丢弃登录态\n");
    std::printf("  webview2_login --selftest [--url <url>] [--timeout <秒>]\n");
    std::printf("                                              自检：隐藏窗口自动跑「导航 → 提取 Cookie」\n");
    std::printf("                                              退出码 0=通过(Cookie>=1) / 1=失败 / 2=超时\n");
    std::printf("  webview2_login --hidden [--timeout <秒>]    交互但不显示窗口（便于自动化）\n");
    std::printf("选项:\n");
    std::printf("  --url <url>      目标站点（默认 https://chat.deepseek.com/）\n");
    std::printf("  --timeout <秒>   自动退出超时（自检默认 30；0 = 不自动退出）\n");
    std::printf("快捷键（交互模式）: Ctrl+Alt+C 提取 Cookie，ESC 退出\n");
    std::printf("说明: Cookie 仅内存 + 控制台脱敏输出，不落盘（设计文档 8.4）\n");
}

void resize_webview()
{
    if (g_controller == nullptr || g_window == nullptr) {
        return;
    }
    RECT bounds{};
    ::GetClientRect(g_window, &bounds);
    g_controller->put_Bounds(bounds);
}

// 提取 Cookie（仅内存 + 控制台输出）
void extract_cookies(const char* reason);

void extract_cookies_impl(const char* reason)
{
    std::printf("\n[webview2] 提取 Cookie（触发原因: %s）...\n", reason);

    // 自检模式下：失败要立即判定（g_cookies_done=true, count=0），避免干等到超时
    const auto fail_fast = [](const std::string& message) {
        aiwrite::log::error(message);
        if (g_options.selftest) {
            g_cookies_done = true;
            g_cookie_count = 0;
        }
    };

    if (g_webview == nullptr) {
        fail_fast("WebView2 尚未就绪，无法提取 Cookie");
        return;
    }

    ComPtr<ICoreWebView2_2> webview2;
    if (FAILED(g_webview.As(&webview2)) || webview2 == nullptr) {
        fail_fast("当前 WebView2 运行时过旧，不支持 CookieManager 接口（请更新 WebView2 Runtime）");
        return;
    }

    ComPtr<ICoreWebView2CookieManager> manager;
    if (FAILED(webview2->get_CookieManager(&manager)) || manager == nullptr) {
        fail_fast("获取 ICoreWebView2CookieManager 失败");
        return;
    }

    const std::wstring target_url = to_wide(g_options.url);
    const HRESULT hr = manager->GetCookies(
        target_url.c_str(),
        Callback<ICoreWebView2GetCookiesCompletedHandler>(
            [](HRESULT result, ICoreWebView2CookieList* list) -> HRESULT {
                if (FAILED(result) || list == nullptr) {
                    aiwrite::log::error("Cookie 提取失败（HRESULT 异常）");
                    g_cookies_done = true;
                    g_cookie_count = 0;
                    return S_OK;
                }

                UINT count = 0;
                list->get_Count(&count);
                g_cookie_count = count;   // 自检判定依据（≥1 为通过）
                g_cookies_done = true;
                aiwrite::log::info("[V-03] Cookie 提取完成：共 " + std::to_string(count) + " 条（值已脱敏）");

                std::printf("\n===== Cookie 提取结果（共 %u 条，值已脱敏）=====\n", count);
                for (UINT i = 0; i < count; ++i) {
                    ComPtr<ICoreWebView2Cookie> cookie;
                    if (FAILED(list->GetValueAtIndex(i, &cookie)) || cookie == nullptr) {
                        continue;
                    }

                    LPWSTR raw_name  = nullptr;
                    LPWSTR raw_value = nullptr;
                    cookie->get_Name(&raw_name);
                    cookie->get_Value(&raw_value);

                    BOOL   http_only = FALSE;
                    BOOL   session   = FALSE;
                    double expires   = 0.0;
                    cookie->get_IsHttpOnly(&http_only);
                    cookie->get_IsSession(&session);
                    cookie->get_Expires(&expires);

                    const std::string name = to_utf8(raw_name != nullptr ? raw_name : L"");
                    const std::string value =
                        mask_value(to_utf8(raw_value != nullptr ? raw_value : L""));

                    std::printf("  %-34s %-34s httpOnly=%d session=%d\n", name.c_str(), value.c_str(),
                                http_only ? 1 : 0, session ? 1 : 0);

                    if (raw_name != nullptr) {
                        ::CoTaskMemFree(raw_name);
                    }
                    if (raw_value != nullptr) {
                        ::CoTaskMemFree(raw_value);
                    }
                }
                std::printf("==============================================\n");
                std::printf("说明：Cookie 仅保存在内存与控制台输出，不写入磁盘（设计文档 8.4）。\n");
                if (g_options.selftest) {
                    std::printf("自检结论：%s（Cookie %u 条，url=%s）\n\n",
                                count >= 1 ? "PASS" : "FAIL", count, g_options.url.c_str());
                }
                else {
                    std::printf("\n");
                }
                return S_OK;
            })
            .Get());

    if (FAILED(hr)) {
        aiwrite::log::error("调用 GetCookies 失败: HRESULT=" + std::to_string(hr));
    }
}

void extract_cookies(const char* reason)
{
    extract_cookies_impl(reason);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        // 【关键】WebView2 的异步回调可能在 CreateWindowExW 返回前（嵌套消息泵）就被触发，
        // 因此必须先用 WM_CREATE 传入的 hwnd 初始化 g_window —— 否则会以 HWND=null 调用
        // CreateCoreWebView2Controller，同步失败 E_INVALIDARG(0x80070057)，
        // 表现为"环境创建成功后就没有下文"（导航/控制器回调永不触发）。
        g_window = window;

        static std::wstring profile_path;
        profile_path = static_cast<const wchar_t*>(
            reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);

        const HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions(
            nullptr, profile_path.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                    if (FAILED(result) || environment == nullptr) {
                        aiwrite::log::error(
                            "创建 WebView2 环境失败（请确认已安装 WebView2 Runtime）HRESULT=" +
                            std::to_string(result));
                        if (g_options.selftest) {
                            g_exit_code = 1; // 自检：环境创建失败 = FAIL
                        }
                        ::PostMessageW(g_window, WM_CLOSE, 0, 0);
                        return S_OK;
                    }

                    g_environment = environment;
                    aiwrite::log::info("WebView2 环境创建成功");

                    const HRESULT controller_hr = g_environment->CreateCoreWebView2Controller(
                        g_window,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                                if (FAILED(result) || controller == nullptr) {
                                    aiwrite::log::error("创建 WebView2 控制器失败 HRESULT=" +
                                                        std::to_string(result));
                                    if (g_options.selftest) {
                                        g_exit_code = 1; // 自检：控制器创建失败 = FAIL
                                    }
                                    ::PostMessageW(g_window, WM_CLOSE, 0, 0);
                                    return S_OK;
                                }

                                aiwrite::log::info("[webview2] 控制器创建成功，准备导航");

                                g_controller = controller;
                                g_controller->get_CoreWebView2(&g_webview);
                                resize_webview();
                                g_controller->put_IsVisible(TRUE);

                                ComPtr<ICoreWebView2Settings> settings;
                                if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
                                    settings->put_IsStatusBarEnabled(FALSE);
                                }

                                EventRegistrationToken token{};
                                g_webview->add_NavigationCompleted(
                                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                        [](ICoreWebView2* /*sender*/,
                                           ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                            BOOL success = FALSE;
                                            args->get_IsSuccess(&success);
                                            g_navigation_done = true;
                                            g_navigation_ok   = (success != FALSE);
                                            aiwrite::log::info(std::string("[webview2] 导航完成: ") +
                                                               (success ? "成功" : "失败") + "（" +
                                                               g_options.url + "）");
                                            if (success) {
                                                extract_cookies("页面加载完成（若已登录可直接看到 Cookie）");
                                            }
                                            else {
                                                // 自检：导航失败直接判定失败，不必等到超时
                                                g_cookies_done = true;
                                                g_cookie_count = 0;
                                            }
                                            return S_OK;
                                        })
                                        .Get(),
                                    &token);

                                aiwrite::log::info("开始导航: " + g_options.url);
                                const std::wstring target_url = to_wide(g_options.url);
                                g_webview->Navigate(target_url.c_str());
                                return S_OK;
                            })
                            .Get());

                    // 关键诊断：CreateCoreWebView2Controller **同步返回**失败时回调不会触发，
                    // 必须把 HRESULT 打出来（否则表现为"环境创建成功后再无下文"）
                    if (FAILED(controller_hr)) {
                        char buffer[32] = {};
                        std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                                      static_cast<unsigned long>(controller_hr));
                        aiwrite::log::error(std::string("CreateCoreWebView2Controller 同步返回失败 HRESULT=") +
                                            buffer + "（窗口 " + std::to_string(reinterpret_cast<std::uintptr_t>(g_window)) + "）");
                        if (g_options.selftest) {
                            g_exit_code = 1;
                        }
                        ::PostMessageW(g_window, WM_CLOSE, 0, 0);
                    }
                    return S_OK;
                })
                .Get());

        if (FAILED(hr)) {
            aiwrite::log::error("CreateCoreWebView2EnvironmentWithOptions 调用失败 HRESULT=" +
                                std::to_string(hr));
            if (g_options.selftest) {
                g_exit_code = 1; // 自检：环境创建失败 = FAIL
            }
            ::PostMessageW(g_window, WM_CLOSE, 0, 0);
        }

        g_start_tick = ::GetTickCount();
        if (g_options.selftest || g_options.timeout_seconds > 0) {
            ::SetTimer(window, kTimerId, 200, nullptr);
        }
        return 0;
    }

    case WM_TIMER: {
        const DWORD elapsed = ::GetTickCount() - g_start_tick;

        if (!g_options.selftest) {
            // 交互模式 + --timeout：到点自动关闭（便于自动化冒烟；不带 --timeout 则永不自动退出）
            if (g_options.timeout_seconds > 0 &&
                elapsed > static_cast<DWORD>(g_options.timeout_seconds) * 1000) {
                aiwrite::log::info("[webview2] 到达 --timeout 设定的 " +
                                   std::to_string(g_options.timeout_seconds) + " 秒，自动关闭窗口");
                ::KillTimer(window, kTimerId);
                ::DestroyWindow(window);
            }
            return 0;
        }

        if (g_cookies_done) {
            g_exit_code = (g_cookie_count >= 1) ? 0 : 1;
            aiwrite::log::info(std::string("[V-03] WebView2 自检：Cookie ") +
                               std::to_string(g_cookie_count) + " 条 → " +
                               (g_exit_code == 0 ? "PASS" : "FAIL") + "（url=" + g_options.url + "）");
            ::KillTimer(window, kTimerId);
            ::DestroyWindow(window);
        }
        else if (g_options.timeout_seconds > 0 &&
                 elapsed > static_cast<DWORD>(g_options.timeout_seconds) * 1000) {
            g_exit_code = 2;
            aiwrite::log::error("[V-03] WebView2 自检超时 → TIMEOUT（" +
                                std::to_string(elapsed / 1000) + " 秒，url=" + g_options.url + "）");
            ::KillTimer(window, kTimerId);
            ::DestroyWindow(window);
        }
        return 0;
    }

    case WM_SIZE:
        resize_webview();
        return 0;

    case WM_HOTKEY:
        if (wparam == kHotkeyExtractCookies) {
            extract_cookies("用户按下 Ctrl+Alt+C");
        }
        else if (wparam == kHotkeyClose) {
            ::PostMessageW(g_window, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_CLOSE:
        if (!g_options.selftest) {
            extract_cookies("窗口关闭前最后一次提取");
        }
        ::DestroyWindow(g_window);
        return 0;

    case WM_DESTROY:
        aiwrite::log::info("正在销毁 WebView2 ...");
        if (g_controller != nullptr) {
            g_controller->Close();
            g_controller = nullptr;
        }
        g_webview     = nullptr;
        g_environment = nullptr;
        ::PostQuitMessage(g_exit_code);
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 %s 缺少取值\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--ephemeral") {
            g_options.ephemeral = true;
        }
        else if (arg == "--selftest") {
            g_options.selftest = true;
            g_options.hidden   = true; // 自检不弹窗（避免抢焦点、便于后台运行）
        }
        else if (arg == "--hidden") {
            g_options.hidden = true;
        }
        else if (arg == "--url") {
            g_options.url = next("--url");
        }
        else if (arg == "--timeout") {
            g_options.timeout_seconds = std::atoi(next("--timeout").c_str());
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

    if (g_options.selftest && g_options.timeout_seconds <= 0) {
        g_options.timeout_seconds = 30; // 自检默认 30 秒超时，避免自动化挂住
    }

    aiwrite::log::init(true);
    aiwrite::log::info("webview2_login 启动（M1-04 / V-03：WebView2 登录 + Cookie 提取）");
    aiwrite::log::info("运行模式: " + std::string(g_options.selftest ? "自检(--selftest)"
                                                                     : (g_options.hidden ? "交互(隐藏窗口)" : "交互")) +
                       "，目标站点: " + g_options.url + "，超时: " +
                       (g_options.timeout_seconds > 0 ? std::to_string(g_options.timeout_seconds) + " 秒"
                                                      : std::string("不自动退出")));

    if (aiwrite::paths::ensure_data_dirs() != 0) {
        aiwrite::log::warn("部分数据目录创建失败，请检查权限");
    }

    const std::filesystem::path profile =
        g_options.ephemeral ? (std::filesystem::temp_directory_path() / "aiwrite-webview2-ephemeral")
                            : aiwrite::paths::webview2_profile();

    std::error_code ec;
    std::filesystem::create_directories(profile, ec);
    aiwrite::log::info(std::string("浏览器 profile: ") + profile.string() +
                       (g_options.ephemeral ? "（临时目录）" : ""));

    const std::wstring profile_wide = profile.wstring();

    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        aiwrite::log::error("COM 初始化失败");
        aiwrite::log::shutdown();
        return 1;
    }

    g_instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW window_class{};
    window_class.cbSize        = sizeof(window_class);
    window_class.style         = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc   = window_proc;
    window_class.hInstance     = g_instance;
    window_class.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;

    if (::RegisterClassExW(&window_class) == 0) {
        aiwrite::log::error("注册窗口类失败");
        ::CoUninitialize();
        aiwrite::log::shutdown();
        return 1;
    }

    RECT rect{0, 0, 1100, 820};
    ::AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    const int width  = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    int       x      = (::GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int       y      = (::GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    if (g_options.hidden) {
        // 「隐藏」用**离屏可见窗口**实现：WebView2 不会为完全隐藏（SW_HIDE）的父窗口创建控制器，
        // 因此把窗口放到桌面可见区域之外（仍 Show），既不打扰用户、又能正常加载页面。
        x = ::GetSystemMetrics(SM_CXSCREEN) + 160;
        y = 120;
    }

    g_window = ::CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, x, y, width,
                                 height, nullptr, nullptr, g_instance,
                                 const_cast<wchar_t*>(profile_wide.c_str()));
    if (g_window == nullptr) {
        aiwrite::log::error("创建窗口失败");
        ::UnregisterClassW(kWindowClass, g_instance);
        ::CoUninitialize();
        aiwrite::log::shutdown();
        return 1;
    }

    // 离屏模式：显示但不抢焦点（SW_SHOWNOACTIVATE），保证消息循环与 WebView2 正常工作
    ::ShowWindow(g_window, g_options.hidden ? SW_SHOWNOACTIVATE : SW_SHOW);
    ::UpdateWindow(g_window);

    if (!g_options.selftest) {
        ::RegisterHotKey(g_window, kHotkeyExtractCookies, MOD_CONTROL | MOD_ALT, 'C');
        ::RegisterHotKey(g_window, kHotkeyClose, 0, VK_ESCAPE);
        aiwrite::log::info("请在弹出的窗口中登录站点；登录完成后按 Ctrl+Alt+C 提取 Cookie，按 ESC 关闭。");
    }
    else {
        aiwrite::log::info("[V-03] 自检模式：等待「导航完成 → 提取 Cookie」，超时 " +
                           std::to_string(g_options.timeout_seconds) + " 秒");
    }

    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    if (!g_options.selftest) {
        ::UnregisterHotKey(g_window, kHotkeyExtractCookies);
        ::UnregisterHotKey(g_window, kHotkeyClose);
    }
    ::UnregisterClassW(kWindowClass, g_instance);
    ::CoUninitialize();

    if (g_options.selftest) {
        const char* verdict = (g_exit_code == 0) ? "PASS" : (g_exit_code == 1 ? "FAIL" : "TIMEOUT");
        std::printf("\n[V-03] 自检结束：%s（退出码 %d，Cookie %u 条，url=%s）\n", verdict, g_exit_code,
                    g_cookie_count, g_options.url.c_str());
        aiwrite::log::info(std::string("[V-03] 自检结束：") + verdict + "（退出码 " +
                           std::to_string(g_exit_code) + "，Cookie " +
                           std::to_string(g_cookie_count) + " 条，url=" + g_options.url + "）");
    }

    aiwrite::log::info("webview2_login 结束");
    aiwrite::log::shutdown();
    return g_exit_code;
}
