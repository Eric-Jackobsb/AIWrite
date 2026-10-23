// ============================================================================
//  webview_host_window.cpp — WebView2 环境·控制器回调·window_proc·窗口线程体·网络回执挂钩
//  * ARCH-Z1b：自 3514 行单文件 `webview_host.cpp` 逐字拆出，**零语义变化**
//  * 模块内契约见 `webview_host_detail.h`；公开 API 见 `webview_host.h`
// ============================================================================

#include "webview_host_detail.h"
#include "log.h"

namespace aiwrite::web {

void hook_resource_capture()
{
    if (st().webview == nullptr) {
        return;
    }
    ComPtr<ICoreWebView2_2> webview2;
    if (FAILED(st().webview->QueryInterface(IID_PPV_ARGS(&webview2))) || webview2 == nullptr) {
        log::warn("[图片上传] 取不到 ICoreWebView2_2 —— 网络回执采集不可用"
                  "（本页只能用页面证据判定）");
        return;
    }
    EventRegistrationToken token{};
    // `PW-04`（`I26`）：回执**带上产生它的归属** —— 取用时会再核一次（`owner_accepts`）
    const WebOwner         window_owner = st().owner;
    const HRESULT          hr = webview2->add_WebResourceResponseReceived(
        Callback<ICoreWebView2WebResourceResponseReceivedEventHandler>(
            [window_owner](ICoreWebView2* /*sender*/,
               ICoreWebView2WebResourceResponseReceivedEventArgs* args) -> HRESULT {
                if (args == nullptr) {
                    return S_OK;
                }
                WebResourceRecord record;
                ComPtr<ICoreWebView2WebResourceRequest> request;
                if (SUCCEEDED(args->get_Request(&request)) && request != nullptr) {
                    LPWSTR uri = nullptr;
                    if (SUCCEEDED(request->get_Uri(&uri)) && uri != nullptr) {
                        record.url = to_utf8(uri);
                        ::CoTaskMemFree(uri);
                    }
                    LPWSTR method = nullptr;
                    if (SUCCEEDED(request->get_Method(&method)) && method != nullptr) {
                        record.method = to_utf8(method);
                        ::CoTaskMemFree(method);
                    }
                }
                ComPtr<ICoreWebView2WebResourceResponseView> response;
                if (SUCCEEDED(args->get_Response(&response)) && response != nullptr) {
                    int status = 0;
                    if (SUCCEEDED(response->get_StatusCode(&status))) {
                        record.status = status;
                    }
                }
                if (record.url.empty()) {
                    return S_OK;
                }
                record.owner = window_owner; // `PW-04` / `I26`：回执绑定归属
                std::lock_guard<std::mutex> lock(st().net_mutex);
                if (st().net_records.size() >= kNetRecordLimit) {
                    st().net_records.erase(st().net_records.begin()); // 有界（R13）
                }
                st().net_records.push_back(std::move(record));
                return S_OK;
            })
            .Get(),
        &token);
    st().net_ready = SUCCEEDED(hr);
    log::info(std::string("[图片上传] 网络回执采集：") +
              (st().net_ready ? "已挂钩（WebResourceResponseReceived）" : "挂钩失败"));
}



HRESULT on_controller_ready(HRESULT result, ICoreWebView2Controller* controller)
{
    if (FAILED(result) || controller == nullptr) {
        log::error("[网页版登录] 创建 WebView2 控制器失败 HRESULT=" + std::to_string(result));
        st().result = 1;
        ::PostMessageW(st().window.load(), WM_CLOSE, 0, 0);
        return S_OK;
    }

    st().controller = controller;
    st().controller->get_CoreWebView2(&st().webview);
    resize_webview();
    st().controller->put_IsVisible(TRUE);

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(st().webview->get_Settings(&settings)) && settings != nullptr) {
        settings->put_IsStatusBarEnabled(FALSE);
    }

    // M8-14：挂钩网络回执采集（上传证据的第二条腿；失败只记日志，不影响登录/生成）
    hook_resource_capture();

    EventRegistrationToken token{};
    st().webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2* /*sender*/,
               ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                if (success) {
                    extract_cookies("页面加载完成");
                    if (st().request.probe_after_load) {
                        st().probe_due_tick = ::GetTickCount() + kProbeDelayMs; // SPA 就绪后再探测
                    }
                }
                else {
                    log::warn("[网页版登录] 页面导航失败（可检查网络后重试）");
                }
                // `PW-08`（D-35③ 修订 2026-10-17）：复位导航的完成信号 —— 只在「有导航在等」时消费
                //（同控制器回新会话页；窗口线程消息循环派发，不锁消息泵）
                {
                    std::lock_guard<std::mutex> lock(st().nav_mutex);
                    if (st().nav_pending) {
                        st().nav_ok      = success ? true : false;
                        st().nav_error   = success
                                               ? std::string()
                                               : std::string("导航完成但页面加载失败（IsSuccess=false）");
                        st().nav_pending = false;
                        st().nav_done    = true;
                    }
                }
                st().nav_cv.notify_all();
                return S_OK;
            })
            .Get(),
        &token);

    log::info("[网页版登录] 开始导航: " + st().request.url);
    st().webview->Navigate(to_wide(st().request.url).c_str());
    return S_OK;
}

// ---- 浏览器环境：**每窗口线程私有**（`D-35④` 修订 2026-10-18）----
//  * WebView2 官方线程模型：环境 / 控制器都必须在其创建线程上使用（「Threading model」文档）。
//    此前「按 profile 缓存、跨窗口线程共享」⇒ 第二个窗口用**别人线程**的环境创建控制器，
//    同步失败 `0x802A000C`（真机批量探测 9/10 站失败根因）。现改为每个窗口线程自建自用。
//  * `envCache().creating` = 「该 profile 正在被某个窗口线程创建」：同 profile 的并发创建**等它建好**
//    （首建时 user data folder 被占用，同时创建会失败）。
//  * 失败码进缓存（`envCache().fail`）：同一 profile 不反复重试空转（如实报错）
void create_controller_for_window(ICoreWebView2Environment* environment)
{
    // `D-35③` 修订 2026-10-17：同步返回失败是**浏览器侧瞬时竞态**的信号
    //（干净 profile 复现 0x802A000C：上一控制器 Close 后浏览器侧未完全释放）⇒ 短退避重试。
    //  * 同步失败时回调不会触发（典型：HWND 无效 → E_INVALIDARG），必须显式报错；
    //    0x802A000C 属瞬时竞态，重试可过；最多 3 次 × 300ms（兜底，窗口线程短暂阻塞可接受）。
    constexpr int   kCreateRetries = 3;
    constexpr DWORD kCreateRetryGapMs = 300;
    for (int attempt = 1; attempt <= kCreateRetries; ++attempt) {
        const HRESULT hr = environment->CreateCoreWebView2Controller(
            st().window.load(),
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(&on_controller_ready)
                .Get());
        if (SUCCEEDED(hr)) {
            return;
        }
        log::warn("[网页版登录] CreateCoreWebView2Controller 同步返回失败 HRESULT=" +
                  std::to_string(hr) + "（窗口 " + st().key + "，第 " + std::to_string(attempt) +
                  "/" + std::to_string(kCreateRetries) + " 次；300ms 后重试）");
        if (attempt < kCreateRetries) {
            ::Sleep(kCreateRetryGapMs);
        }
    }
    log::error("[网页版登录] CreateCoreWebView2Controller 重试耗尽仍失败（窗口 " + st().key + "）");
    st().result = 1;
    ::PostMessageW(st().window.load(), WM_CLOSE, 0, 0);
}

HRESULT on_environment_ready(HRESULT result, ICoreWebView2Environment* environment)
{
    const std::string profile_dir = st().request.profile_dir;
    if (FAILED(result) || environment == nullptr) {
        log::error("[网页版登录] 创建 WebView2 环境失败（请确认已安装 WebView2 Runtime）HRESULT=" +
                   std::to_string(result));
        {
            std::lock_guard<std::mutex> lock(envCache().mutex);
            envCache().fail[profile_dir] = result;
            if (envCache().creating == profile_dir) {
                envCache().creating.clear();
            }
        }
        envCache().cv.notify_all();
        st().result = 1;
        ::PostMessageW(st().window.load(), WM_CLOSE, 0, 0);
        return S_OK;
    }

    // ① 环境归**本窗口线程**私有（`D-35④` 修订 2026-10-18）：**不进**进程级 `g_envs` 缓存。
    //    跨线程复用环境 ⇒ 第二个窗口 `CreateCoreWebView2Controller` 同步失败 0x802A000C
    //    （线程亲和性违反，真机 2026-10-17 批量探测实证）。环境随窗口关闭一并释放。
    {
        std::lock_guard<std::mutex> lock(envCache().mutex);
        envCache().fail.erase(profile_dir);
        if (envCache().creating == profile_dir) {
            envCache().creating.clear();
        }
    }
    envCache().cv.notify_all();

    // ② 本窗口**自己**的 controller（创建线程 = 环境线程 ⇒ 满足线程亲和）
    st().environment = environment;
    create_controller_for_window(environment);
    return S_OK;
}

// 取（必要时建）**本窗口**要用的环境；失败 → false + 可操作 `error`
//  * `D-35④`（2026-10-18）：**每个窗口线程都自己创建环境**（本线程用，不跨线程复用）——
//    跨线程复用会撞 0x802A000C（线程亲和）。同 profile 的并发创建由 `envCache().creating` 串行化。
bool ensure_environment(HWND window, std::string* error)
{
    const std::string profile_dir = st().request.profile_dir;
    constexpr DWORD   kEnvWaitMs  = 30000; // 等「同 profile 的创建者」完成（首次可能较慢）
    const DWORD       deadline    = ::GetTickCount() + kEnvWaitMs;
    bool              create_now  = false;
    bool              env_timeout = false;
    HRESULT           fail_code   = S_OK;

    {
        std::unique_lock<std::mutex> lock(envCache().mutex);
        for (;;) {
            if (const auto it = envCache().fail.find(profile_dir); it != envCache().fail.end()) {
                fail_code = it->second;
                break;
            }
            if (envCache().creating.empty()) {
                envCache().creating = profile_dir; // 本线程来建（同 profile 同时只允许一个创建者）
                create_now     = true;
                break;
            }
            if (::GetTickCount() >= deadline) {
                env_timeout = true;
                break;
            }
            envCache().cv.wait_for(lock, std::chrono::milliseconds(200));
        }
    }

    if (create_now) {
        const std::wstring profile = to_wide(profile_dir);
        log::info("[网页版会话] 创建 WebView2 环境（profile " + profile_dir + "；窗口 " +
                  st().key + "）");
        const HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions(
            nullptr, profile.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                &on_environment_ready)
                .Get());
        if (FAILED(hr)) {
            log::error("[网页版登录] CreateCoreWebView2EnvironmentWithOptions 调用失败 HRESULT=" +
                       std::to_string(hr));
            {
                std::lock_guard<std::mutex> lock(envCache().mutex);
                envCache().fail[profile_dir] = hr;
                if (envCache().creating == profile_dir) {
                    envCache().creating.clear();
                }
            }
            envCache().cv.notify_all();
            if (error != nullptr) {
                *error = "创建 WebView2 环境失败（HRESULT " + std::to_string(hr) +
                         "）：请确认已安装 WebView2 Runtime";
            }
            ::PostMessageW(window, WM_CLOSE, 0, 0);
            return false;
        }
        return true; // 环境回调里会创建**本窗口**的 controller
    }

    if (error != nullptr) {
        *error = env_timeout ? ("等待 WebView2 环境超时（profile " + profile_dir +
                                "；另一个窗口正在创建同一 profile 的环境）")
                             : ("WebView2 环境此前创建失败（HRESULT " + std::to_string(fail_code) +
                                "；profile " + profile_dir + "）");
    }
    ::PostMessageW(window, WM_CLOSE, 0, 0);
    return false;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)lparam;
    switch (message) {
    case WM_CREATE: {
        // 【关键】异步回调可能在 CreateWindowExW 返回前（嵌套消息泵）触发，必须先赋值 st().window
        st().window = window;

        // 环境**每窗口线程私有**（`D-35④` 修订 2026-10-18：WebView2 线程亲和，跨线程复用
        // 会 0x802A000C）——本线程建、本线程用，窗口关闭即释放
        std::string env_error;
        if (!ensure_environment(window, &env_error)) {
            log::error("[网页版登录] " + st().key + " 环境不可用：" + env_error);
            st().result = 1;
            ::PostMessageW(window, WM_CLOSE, 0, 0);
        }

        st().start_tick = ::GetTickCount();
        st().last_poll  = st().start_tick;
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
        if (st().stop.load()) {
            log::info("[网页版登录] 收到关闭请求，正在关闭登录窗口");
            close_window(-1);
            return 0;
        }
        if (st().request.timeout_seconds > 0 &&
            now - st().start_tick > static_cast<DWORD>(st().request.timeout_seconds) * 1000) {
            log::warn("[网页版登录] 到达超时 " + std::to_string(st().request.timeout_seconds) +
                      " 秒，自动关闭窗口");
            close_window(st().cookieCount.load() > 0 ? 0 : 2);
            return 0;
        }
        if (now - st().last_poll >= kCookiePollMs && st().webview != nullptr) {
            st().last_poll = now;
            extract_cookies("定时刷新（登录后 Cookie 会变化）");
        }
        if (st().probe_stage == 1 && now - st().probe_last_poll >= 500) {
            st().probe_last_poll = now;
            poll_protocol_probe();
        }
        if (st().pow_stage == 1 && now - st().pow_last_poll >= 500) {
            st().pow_last_poll = now;
            poll_pow_solve();
        }
        if (st().probe_due_tick != 0 && now >= st().probe_due_tick) {
            st().probe_due_tick = 0;
            start_protocol_probe();
        }
        if (st().request.auto_close_after_cookies && st().cookies_done.load()) {
            const DWORD first = st().first_cookie_tick.load();
            if (first != 0 && now - first >= kAutoCloseGraceMs) {
                log::info("[网页版登录] 已取得 Cookie 且稳定 " +
                          std::to_string(kAutoCloseGraceMs / 1000) + " 秒，自动关闭登录窗口");
                close_window(0);
            }
        }
        return 0;
    }

    case kRunScriptMessage: // L3：DOM 适配器 / 选择器探测
        run_script_in_window();
        return 0;

    case kNavigateMessage: // PW-08（D-35③ 修订 2026-10-17）：同控制器「导航回新会话页」
        run_navigate_in_window();
        return 0;

    case kSetFilesMessage: // M8-14：把本地文件交给页面（CDP 三步）
        run_set_files_in_window();
        return 0;

    case kCdpInputMessage: // PW-07f：CDP 真实输入 / 发送（insertText + Enter）
        run_cdp_input_in_window();
        return 0;

    case kProbeMessage:
        start_protocol_probe();
        return 0;

    case kSolvePowMessage:
        run_pow_solve_in_page();
        return 0;

    case kLogoutMessage:
        run_logout_in_page();
        return 0;

    case WM_CLOSE:
        extract_cookies("窗口关闭前最后一次提取"); // 异步；紧接着销毁窗口
        ::DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        if (st().controller != nullptr) {
            st().controller->Close();
            st().controller = nullptr;
        }
        st().webview     = nullptr;
        st().environment = nullptr;
        st().window      = nullptr;
        forget_ready(); // M_patchC PW-03：窗口关闭 ⇒ 就绪缓存作废
        if (st().result.load() < 0) {
            st().result = (st().cookieCount.load() > 0) ? 0 : 1;
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

    // ---- `PW-02`：**窗口类名按窗口隔离**（多窗口并存时，同名 `RegisterClassExW` 会失败）----
    st().class_name = std::wstring(kWindowClass) + L"_" + std::to_wstring(st().seq);

    WNDCLASSEXW window_class{};
    window_class.cbSize        = sizeof(window_class);
    window_class.style         = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc   = window_proc;
    window_class.hInstance     = ::GetModuleHandleW(nullptr);
    window_class.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = st().class_name.c_str();

    if (::RegisterClassExW(&window_class) == 0) {
        log::error("[网页版登录] 注册窗口类失败（" + st().key + "）");
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

    HWND window = ::CreateWindowExW(0, st().class_name.c_str(), to_wide(request.window_title).c_str(),
                                    WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr, nullptr,
                                    window_class.hInstance, nullptr);
    if (window == nullptr) {
        log::error("[网页版登录] 创建登录窗口失败（" + st().key + "）");
        ::UnregisterClassW(st().class_name.c_str(), window_class.hInstance);
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
    ::UnregisterClassW(st().class_name.c_str(), window_class.hInstance);
    ::CoUninitialize();
    return st().result.load() < 0 ? 1 : st().result.load();
}


} // namespace aiwrite::web
