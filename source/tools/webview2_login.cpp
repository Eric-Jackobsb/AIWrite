// =============================================================================
//  webview2_login —— M1 技术验证工具（M1-04 / V-03）
//
//  功能：
//    1) 用 WebView2 打开 https://chat.deepseek.com/
//    2) 用户手动登录（人工步骤，无法自动完成）
//    3) 提取该站点 Cookie，仅打印「名称 + 脱敏值 + 属性」，不落盘
//    4) 关闭窗口即销毁 WebView2 与提取到的数据
//
//  用法：
//    webview2_login               # 使用 ~/.brain-ai/webview2 作为浏览器 profile（保留登录态）
//    webview2_login --ephemeral   # 使用临时 profile，退出即丢弃登录态
//
//  快捷键：Ctrl+Alt+C 立即提取 Cookie，ESC 关闭窗口
// =============================================================================
#include "utils/log.h"
#include "utils/paths.h"

#include <windows.h>

#include <wrl/client.h> // Microsoft::WRL::ComPtr
#include <wrl/event.h>  // Microsoft::WRL::Callback

#include <WebView2.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"AIwriteWebView2Login";
constexpr wchar_t kWindowTitle[] =
    L"AIwrite · DeepSeek 网页登录（登录后按 Ctrl+Alt+C 提取 Cookie，ESC 退出）";
constexpr wchar_t kStartUrl[]               = L"https://chat.deepseek.com/";
constexpr int     kHotkeyExtractCookies     = 1;
constexpr int     kHotkeyClose              = 2;

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

    if (g_webview == nullptr) {
        aiwrite::log::warn("WebView2 尚未就绪，无法提取 Cookie");
        return;
    }

    ComPtr<ICoreWebView2_2> webview2;
    if (FAILED(g_webview.As(&webview2)) || webview2 == nullptr) {
        aiwrite::log::error("当前 WebView2 运行时过旧，不支持 CookieManager 接口");
        return;
    }

    ComPtr<ICoreWebView2CookieManager> manager;
    if (FAILED(webview2->get_CookieManager(&manager)) || manager == nullptr) {
        aiwrite::log::error("获取 ICoreWebView2CookieManager 失败");
        return;
    }

    const HRESULT hr = manager->GetCookies(
        kStartUrl,
        Callback<ICoreWebView2GetCookiesCompletedHandler>(
            [](HRESULT result, ICoreWebView2CookieList* list) -> HRESULT {
                if (FAILED(result) || list == nullptr) {
                    aiwrite::log::error("Cookie 提取失败（HRESULT 异常）");
                    return S_OK;
                }

                UINT count = 0;
                list->get_Count(&count);

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
                std::printf("说明：Cookie 仅保存在内存与控制台输出，不写入磁盘（设计文档 8.4）。\n\n");
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
                        ::PostMessageW(g_window, WM_CLOSE, 0, 0);
                        return S_OK;
                    }

                    g_environment = environment;
                    aiwrite::log::info("WebView2 环境创建成功");

                    return g_environment->CreateCoreWebView2Controller(
                        g_window,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                                if (FAILED(result) || controller == nullptr) {
                                    aiwrite::log::error("创建 WebView2 控制器失败 HRESULT=" +
                                                        std::to_string(result));
                                    ::PostMessageW(g_window, WM_CLOSE, 0, 0);
                                    return S_OK;
                                }

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
                                            aiwrite::log::info(std::string("[webview2] 导航完成: ") +
                                                               (success ? "成功" : "失败"));
                                            if (success) {
                                                extract_cookies("页面加载完成（若已登录可直接看到 Cookie）");
                                            }
                                            return S_OK;
                                        })
                                        .Get(),
                                    &token);

                                aiwrite::log::info("开始导航: https://chat.deepseek.com/");
                                g_webview->Navigate(kStartUrl);
                                return S_OK;
                            })
                            .Get());
                })
                .Get());

        if (FAILED(hr)) {
            aiwrite::log::error("CreateCoreWebView2EnvironmentWithOptions 调用失败 HRESULT=" +
                                std::to_string(hr));
            ::PostMessageW(g_window, WM_CLOSE, 0, 0);
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
        extract_cookies("窗口关闭前最后一次提取");
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
        ::PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int main(int argc, char** argv)
{
    bool ephemeral = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--ephemeral") {
            ephemeral = true;
        }
        else if (arg == "--help" || arg == "-h") {
            std::printf("webview2_login - AIwrite M1 技术验证工具（M1-04 / V-03）\n");
            std::printf("用法: webview2_login [--ephemeral]\n");
            std::printf("  --ephemeral  使用临时浏览器 profile，退出即丢弃登录态\n");
            return 0;
        }
        else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            return 2;
        }
    }

    aiwrite::log::init(true);
    aiwrite::log::info("webview2_login 启动（M1-04 / V-03：WebView2 登录 + Cookie 提取）");

    if (aiwrite::paths::ensure_data_dirs() != 0) {
        aiwrite::log::warn("部分数据目录创建失败，请检查权限");
    }

    const std::filesystem::path profile =
        ephemeral ? (std::filesystem::temp_directory_path() / "aiwrite-webview2-ephemeral")
                  : aiwrite::paths::webview2_profile();

    std::error_code ec;
    std::filesystem::create_directories(profile, ec);
    aiwrite::log::info(std::string("浏览器 profile: ") + profile.string() +
                       (ephemeral ? "（临时目录）" : ""));

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
    const int x      = (::GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y      = (::GetSystemMetrics(SM_CYSCREEN) - height) / 2;

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

    ::ShowWindow(g_window, SW_SHOW);
    ::UpdateWindow(g_window);

    ::RegisterHotKey(g_window, kHotkeyExtractCookies, MOD_CONTROL | MOD_ALT, 'C');
    ::RegisterHotKey(g_window, kHotkeyClose, 0, VK_ESCAPE);

    aiwrite::log::info("请在弹出的窗口中手动登录 DeepSeek；登录完成后按 Ctrl+Alt+C 提取 Cookie，"
                       "按 ESC 关闭。");

    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    ::UnregisterHotKey(g_window, kHotkeyExtractCookies);
    ::UnregisterHotKey(g_window, kHotkeyClose);
    ::UnregisterClassW(kWindowClass, g_instance);
    ::CoUninitialize();

    aiwrite::log::info("webview2_login 结束");
    aiwrite::log::shutdown();
    return 0;
}
