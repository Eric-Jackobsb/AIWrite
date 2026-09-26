#include "web/webview_host.h"

#include "utils/log.h"
#include "utils/paths.h"
#include "web/session_store.h"

#include <windows.h>

#include <wrl/client.h> // Microsoft::WRL::ComPtr
#include <wrl/event.h>  // Microsoft::WRL::Callback

#include <WebView2.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
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
constexpr UINT           kSolvePowMessage = WM_APP + 2; // 请求在页面内求解 PoW（任意线程 Post）
constexpr UINT           kLogoutMessage   = WM_APP + 3; // 请求按站点注销（删该 origin 的 Cookie + 清 localStorage）
constexpr UINT           kRunScriptMessage = WM_APP + 4; // L3：请求在页面内执行一段脚本并回传原始 JSON（任意线程 Post）
// 页面加载完成后到开始协议探测的等待（给 SPA 一点就绪时间）
constexpr DWORD kProbeDelayMs = 1500;

// 页面内求解 PoW：分两段执行（注入→轮询取回），避免部分运行时 ExecuteScript 不等待 Promise
//  1) 找 main.*.js（页面已加载的主包）
//  2) 正则取 `n.u(<id>)`（new Worker 处）与 `<id>:"<hash>"`（chunk 表）
//  3) 用「主包 URL 换文件名」拼出 worker URL，取回源码 → Blob → Worker（绕同源限制）
constexpr const char* kSolvePowScript = R"JS(
(function () {
  window.__aiwritePowResult = '';
  const finish = (obj) => { window.__aiwritePowResult = JSON.stringify(obj); };
  const runWorker = (urlText) => new Promise((resolve, reject) => {
    const worker = new Worker(urlText);
    const timer = setTimeout(() => { worker.terminate(); reject(new Error('worker timeout')); }, 60000);
    worker.onmessage = (ev) => {
      clearTimeout(timer); worker.terminate();
      if (ev.data && ev.data.type === 'pow-answer') { resolve(ev.data.answer); }
      else { reject(new Error('pow-error')); }
    };
    worker.onerror = () => { clearTimeout(timer); worker.terminate(); reject(new Error('worker error')); };
    worker.postMessage(window.__aiwritePowPayload);
  });

  (async () => {
    const out = { ok: false, answer: -1, error: '' };
    try {
      let raw = window.__aiwritePowChallenge;
      if (typeof raw === 'string') { raw = raw.trim(); }
      if (!raw) { throw new Error('empty challenge'); }
      const parsed = (typeof raw === 'string') ? JSON.parse(raw) : raw;
      const ch = parsed.data.biz_data.challenge;
      window.__aiwritePowPayload = {
        type: 'pow-challenge',
        challenge: { ...ch, expireAt: ch.expire_at, expireAfter: ch.expire_after }
      };

      const candidates = [];
      try {
        const mainUrl = Array.from(document.querySelectorAll('script[src]'))
          .map((s) => s.src)
          .find((u) => /\/main\.[0-9a-f]+\.js$/i.test(u));
        if (mainUrl) {
          const text = await (await fetch(mainUrl)).text();
          const ids = new Set();
          const workerRe = /new Worker\(new URL\([^,]*?\.u\((\d+)\)/g;
          let m;
          while ((m = workerRe.exec(text)) !== null) { ids.add(m[1]); }
          for (const id of ids) {
            const hashRe = new RegExp('(?:^|[,{])"?' + id + '"?:"([0-9a-f]{8,})"');
            const hm = hashRe.exec(text);
            if (hm) { candidates.push(mainUrl.replace(/\/[^/]+\.js$/, '/' + id + '.' + hm[1] + '.js')); }
          }
        }
      } catch (e) { /* 主包解析失败则走兜底 */ }
      candidates.push('https://fe-static.deepseek.com/chat/static/76608.8f2a9fa413.js');

      let lastError = '';
      for (const url of candidates) {
        try {
          const src = await (await fetch(url)).text();
          const blobUrl = URL.createObjectURL(new Blob([src], { type: 'text/javascript' }));
          const answer = await runWorker(blobUrl);
          if (answer && typeof answer.answer === 'number') {
            out.ok = true;
            out.answer = answer.answer;
            finish(out);
            return;
          }
        } catch (e) { lastError = String(e); }
      }
      out.error = lastError || 'no worker candidate succeeded';
    } catch (e) { out.error = String(e); }
    finish(out);
  })();
  return 'started';
})();
)JS";

// 轮询取回求解结果
constexpr const char* kSolvePowPollScript = R"JS(
(function () {
  const r = window.__aiwritePowResult;
  return r ? r : '';
})();
)JS";

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

// ---- M_patchB L1（PB2-05）：把站点路径注入探测脚本（不传参数 = 改造前常量，逐字一致）----
void replace_all_token(std::string* text, const std::string& from, const std::string& to)
{
    for (std::size_t pos = text->find(from); pos != std::string::npos;
         pos = text->find(from, pos + to.size())) {
        text->replace(pos, from.size(), to);
    }
}


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

// 页面内 PoW 求解：跨线程同步（调用方等待，窗口线程执行）
std::mutex              g_pow_mutex;
std::condition_variable g_pow_cv;
bool                    g_pow_done = false;
long long               g_pow_answer = -1;
std::string             g_pow_error;
std::string             g_pow_challenge_json;
int                     g_pow_stage      = 0;   // 0=空闲 / 1=已注入（轮询等待）/ 2=完成
int                     g_pow_attempt    = 0;
DWORD                   g_pow_last_poll  = 0;
constexpr int           kPowMaxAttempts  = 240; // 240 × 500ms ≈ 120 秒

// ---- L3（PB2-13 / PB2-15）：通用「窗口内同步执行脚本」的状态（一次只跑一段，串行复用）----
std::mutex              g_script_mutex;
std::condition_variable g_script_cv;
bool                    g_script_done = false;
std::string             g_script_js;
std::string             g_script_result;
std::string             g_script_error;

// 按站点注销（PB2-19）：跨线程同步（调用方等待，窗口线程删 Cookie + 清 localStorage）
std::mutex              g_logout_mutex;
std::condition_variable g_logout_cv;
bool                    g_logout_done  = false;
bool                    g_logout_ok    = false;
std::string             g_logout_error;
unsigned                g_logout_deleted = 0;
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

// 写会话：按**站点**（origin）归档 —— 多站点互不覆盖（PB2-18）
void write_session(const std::vector<Cookie>& cookies)
{
    Session session;
    session.url         = g_request.url;
    session.cookies     = cookies;
    session.site        = site_key_of(g_request.url);
    session.provider_id = g_request.provider_id;
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
    SessionStore::instance().set_probe(std::move(probe), site_key_of(g_request.url)); // 按站点归档（PB2-18）

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
    const std::wstring script = to_wide(probe_kickoff_script(g_request));
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

// PoW：把结果交回等待中的调用线程
// L3：结束一次「窗口内同步执行脚本」（窗口线程调用；结果 = ExecuteScript 的原始 JSON）
void finish_run_script(std::string result, std::string error)
{
    {
        std::lock_guard<std::mutex> lock(g_script_mutex);
        g_script_result = std::move(result);
        g_script_error  = std::move(error);
        g_script_done   = true;
    }
    g_script_cv.notify_all();
}

// L3：在窗口线程执行 g_script_js（不做解释，原样回传 JSON 结果）
void run_script_in_window()
{
    if (g_webview == nullptr) {
        finish_run_script({}, "WebView2 未就绪");
        return;
    }
    std::string script_js;
    {
        std::lock_guard<std::mutex> lock(g_script_mutex);
        script_js = g_script_js;
    }
    if (script_js.empty()) {
        finish_run_script({}, "脚本为空");
        return;
    }
    const std::wstring script = to_wide(script_js);
    const HRESULT      hr     = g_webview->ExecuteScript(
        script.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT result, LPCWSTR json_result) -> HRESULT {
                if (FAILED(result)) {
                    finish_run_script({}, "脚本执行失败（HRESULT " + std::to_string(result) + "）");
                }
                else {
                    finish_run_script(json_result == nullptr ? std::string("null")
                                                             : to_utf8(json_result),
                                      {});
                }
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) {
        finish_run_script({}, "调用 ExecuteScript 失败");
    }
}

void finish_pow_solve(long long answer, std::string error)
{
    {
        std::lock_guard<std::mutex> lock(g_pow_mutex);
        g_pow_answer = answer;
        g_pow_error  = std::move(error);
        g_pow_done   = true;
    }
    g_pow_cv.notify_all();
}

// PoW：在窗口线程执行（注入挑战 → 运行页面内求解脚本）
void run_pow_solve_in_page()
{
    if (g_webview == nullptr) {
        finish_pow_solve(-1, "WebView2 未就绪");
        return;
    }

    std::string challenge_json;
    {
        std::lock_guard<std::mutex> lock(g_pow_mutex);
        challenge_json = g_pow_challenge_json;
    }
    if (challenge_json.empty()) {
        finish_pow_solve(-1, "挑战为空");
        return;
    }

    // 1) 挑战 JSON 写入页面全局（内容是合法 JSON，可安全内联）
    const std::wstring inject =
        to_wide("window.__aiwritePowChallenge = " + challenge_json + "; 'ok';");
    const HRESULT inject_hr = g_webview->ExecuteScript(
        inject.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT result, LPCWSTR /*json_result*/) -> HRESULT {
                if (FAILED(result)) {
                    finish_pow_solve(-1, "注入挑战失败");
                }
                return S_OK;
            })
            .Get());
    if (FAILED(inject_hr)) {
        finish_pow_solve(-1, "调用注入脚本失败");
        return;
    }

    // 2) 注入求解脚本（结果写入 window.__aiwritePowResult，由 WM_TIMER 轮询取回）
    const std::wstring script = to_wide(kSolvePowScript);
    const HRESULT      hr     = g_webview->ExecuteScript(
        script.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT result, LPCWSTR /*json_result*/) -> HRESULT {
                if (FAILED(result)) {
                    finish_pow_solve(-1, "注入求解脚本失败");
                }
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) {
        finish_pow_solve(-1, "调用求解脚本失败");
        return;
    }
    g_pow_stage     = 1;
    g_pow_attempt   = 0;
    g_pow_last_poll = ::GetTickCount();
}

// PoW：轮询页面里的求解结果（WM_TIMER 驱动）
void poll_pow_solve()
{
    if (g_pow_stage != 1 || g_webview == nullptr) {
        return;
    }
    if (++g_pow_attempt > kPowMaxAttempts) {
        g_pow_stage = 2;
        finish_pow_solve(-1, "页面内 PoW 求解超时");
        return;
    }

    const std::wstring script = to_wide(kSolvePowPollScript);
    const HRESULT      hr     = g_webview->ExecuteScript(
        script.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT result, LPCWSTR json_result) -> HRESULT {
                if (FAILED(result) || json_result == nullptr) {
                    return S_OK; // 下一轮继续
                }
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

                g_pow_stage = 2;
                try {
                    const nlohmann::json data   = nlohmann::json::parse(inner);
                    const bool           ok     = data.value("ok", false);
                    const long long      answer = data.value("answer", -1LL);
                    const std::string    why    = data.value("error", std::string());
                    log::info("[网页版 PoW] 页面内求解：" +
                              (ok ? "answer=" + std::to_string(answer)
                                  : std::string("失败（") + why + "）"));
                    finish_pow_solve(ok ? answer : -1, ok ? std::string() : why);
                }
                catch (const std::exception& ex) {
                    finish_pow_solve(-1, std::string("解析求解结果失败: ") + ex.what());
                }
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) {
        g_pow_stage = 2;
        finish_pow_solve(-1, "轮询求解结果失败");
    }
}

// ------------------------------------------------------------ 按站点注销 -----
// PB2-19：在**窗口线程内**删除该站点 origin 的 Cookie 并清同源 localStorage / sessionStorage。
//  * 不触碰其他站点（profile 虽共享，但 cookie / localStorage 按 origin 隔离 —— 决策 D-19 / R14）
//  * 完成后由 logout_site() 关闭窗口并已提前清掉该站点的内存会话
void finish_logout(bool ok, std::string error)
{
    {
        std::lock_guard<std::mutex> lock(g_logout_mutex);
        g_logout_ok    = ok;
        g_logout_error = std::move(error);
        g_logout_done  = true;
    }
    g_logout_cv.notify_all();
}

void run_logout_in_page()
{
    if (g_webview == nullptr) {
        finish_logout(false, "WebView2 尚未就绪");
        return;
    }

    ComPtr<ICoreWebView2_2> webview2;
    if (FAILED(g_webview.As(&webview2)) || webview2 == nullptr) {
        finish_logout(false, "当前 WebView2 运行时过旧，不支持 CookieManager（请更新 Runtime）");
        return;
    }
    ComPtr<ICoreWebView2CookieManager> manager;
    if (FAILED(webview2->get_CookieManager(&manager)) || manager == nullptr) {
        finish_logout(false, "获取 ICoreWebView2CookieManager 失败");
        return;
    }

    const std::wstring target_url = to_wide(g_request.url);
    const HRESULT      hr         = manager->GetCookies(
        target_url.c_str(),
        Callback<ICoreWebView2GetCookiesCompletedHandler>(
            [manager](HRESULT result, ICoreWebView2CookieList* list) -> HRESULT {
                UINT     count   = 0;
                unsigned deleted = 0;
                if (SUCCEEDED(result) && list != nullptr) {
                    list->get_Count(&count);
                    for (UINT i = 0; i < count; ++i) {
                        ComPtr<ICoreWebView2Cookie> cookie;
                        if (SUCCEEDED(list->GetValueAtIndex(i, &cookie)) && cookie != nullptr &&
                            SUCCEEDED(manager->DeleteCookie(cookie.Get()))) {
                            ++deleted;
                        }
                    }
                }
                g_logout_deleted = deleted;
                log::info("[网页版注销] 站点 Cookie 删除：" + std::to_string(deleted) + " / " +
                          std::to_string(count) + " 条（" + g_request.url + "）");

                if (g_webview == nullptr) {
                    finish_logout(true, std::string());
                    return S_OK;
                }
                // 同源 localStorage / sessionStorage 一并清掉（网页版 token 就存在 localStorage）
                const HRESULT script_hr = g_webview->ExecuteScript(
                    L"try { localStorage.clear(); sessionStorage.clear(); } catch (e) { }",
                    Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                        [](HRESULT, LPCWSTR /*json*/) -> HRESULT {
                            finish_logout(true, std::string());
                            return S_OK;
                        })
                        .Get());
                if (FAILED(script_hr)) {
                    finish_logout(true, std::string()); // Cookie 已删；localStorage 清理失败不致命
                }
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) {
        finish_logout(false, "调用 CookieManager->GetCookies 失败");
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
        if (g_pow_stage == 1 && now - g_pow_last_poll >= 500) {
            g_pow_last_poll = now;
            poll_pow_solve();
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

    case kRunScriptMessage: // L3：DOM 适配器 / 选择器探测
        run_script_in_window();
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

// ---- M_patchB L1（PB2-05）：站点参数化 —— 渲染探测脚本（外部可见，供离线断言）----
//  * 不传站点参数时，渲染结果与改造前的常量脚本逐字一致（行为不变）
std::string probe_kickoff_script(const LoginRequest& request)
{
    std::string       script(kProbeKickoffScript);
    const std::string challenge =
        request.challenge_path.empty() ? std::string("/api/v0/chat/create_pow_challenge")
                                       : request.challenge_path;
    const std::string completion = request.completion_path.empty()
                                       ? std::string("/api/v0/chat/completion")
                                       : request.completion_path;
    replace_all_token(&script, "'/api/v0/chat/create_pow_challenge'", "'" + challenge + "'");
    replace_all_token(&script, "'/api/v0/chat/completion'", "'" + completion + "'");
    if (!request.probe_paths.empty()) {
        std::string list;
        for (const std::string& path : request.probe_paths) {
            if (!list.empty()) {
                list += ", ";
            }
            list += "'" + path + "'";
        }
        replace_all_token(&script, "['/api/v0/users/current', '/api/v0/chat_session/fetch_page']",
                          "[" + list + "]");
    }
    // M_patchB L1 续（PB2-17）：取 token 表达式按站点（空 = 内置脚本原样 → 逐字一致）
    if (!request.token_expr.empty()) {
        replace_all_token(&script, "localStorage.getItem('userToken')", request.token_expr);
    }
    return script;
}

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
    {
        std::lock_guard<std::mutex> lock(g_logout_mutex);
        g_logout_done    = false;
        g_logout_ok      = false;
        g_logout_error.clear();
        g_logout_deleted = 0;
    }

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

// 当前登录窗口所属站点（origin；无窗口 = 空）
std::string current_window_site()
{
    if (g_window.load() == nullptr) {
        return {};
    }
    return site_key_of(g_request.url);
}

bool window_on_site(const std::string& site)
{
    return g_window.load() != nullptr && !site.empty() && current_window_site() == site;
}

// 兼容路径（不指定站点）：改造前语义 —— 当前会话 / 内置默认站点，行为逐字不变
bool ensure_session(int timeout_ms, std::string* error)
{
    // 已有凭证 → 幂等返回（默认槽 = 最近写入的站点）
    if (!SessionStore::instance().snapshot().user_token.empty()) {
        return true;
    }

    LoginRequest request; // 默认 = 内置默认站点（DeepSeek）
    if (g_window.load() != nullptr && !g_request.url.empty()) {
        // 窗口已开：沿用其站点与站点参数（原地补探测，不另开窗）
        request.url             = g_request.url;
        request.window_title    = g_request.window_title;
        request.profile_dir     = g_request.profile_dir;
        request.provider_id     = g_request.provider_id;
        request.probe_paths     = g_request.probe_paths;
        request.challenge_path  = g_request.challenge_path;
        request.completion_path = g_request.completion_path;
        request.token_expr      = g_request.token_expr;
    }
    return ensure_session(request, timeout_ms, error);
}

// M_patchB L1 续（PB2-17/18/19）：按**站点**确保凭证 + 窗口对齐
//  * 窗口必须开在**目标站点**上：页面内 PoW 求解依赖该站点页面（否则会拿错站点的页面）
//  * 站点与当前窗口不一致 → **串行复用**同一个窗口（先关旧窗再按目标站点开窗；决策 D-20）
//  * 因此多个网页版条目可「各自登录一次、运行时各用各的凭证」（内存会话按站点键控）
bool ensure_session(const LoginRequest& site_request, int timeout_ms, std::string* error)
{
    const std::string site = site_key_of(site_request.url);
    const DWORD       deadline =
        ::GetTickCount() + static_cast<DWORD>(timeout_ms > 0 ? timeout_ms : 30000);

    bool window_ready = window_on_site(site);
    if (!window_ready) {
        // ---- 串行复用：目标站点 ≠ 当前窗口站点（或无窗口）→ 关旧窗 + 按目标站点开窗 ----
        if (g_window.load() != nullptr) {
            log::info("[网页版会话] 切换站点：" + current_window_site() + " → " + site +
                      "（串行复用登录窗口：先关闭旧窗口）");
            login_window().request_close();
            login_window().join();
        }
        // 起（或重建）离屏登录窗口；probe_after_load 会刷新 Cookie 与 userToken
        LoginRequest request     = site_request;
        request.offscreen        = true;   // 离屏可见窗口（完全隐藏时 WebView2 不创建控制器）
        request.timeout_seconds  = 0;
        request.probe_after_load = true;
        std::string start_error;
        if (!login_window().start(request, &start_error)) {
            if (error != nullptr) {
                *error = "启动登录窗口失败" + (site.empty() ? std::string() : "（站点 " + site + "）") +
                         ": " + start_error;
            }
            return false;
        }
        log::info("[网页版会话] 站点 " + (site.empty() ? std::string("(默认)") : site) +
                  (SessionStore::instance().has_token(site)
                       ? " 已有凭证但窗口不在该站点：已按串行策略重新打开（页面内 PoW 求解依赖该站点页面）"
                       : " 内存无凭证：已离屏启动登录窗口（页面加载后自动探测）"));
    }
    else if (SessionStore::instance().has_token(site)) {
        return true; // 幂等：窗口与凭证都已就绪
    }
    else {
        // 窗口已在该站点但没有凭证（用户手动开的窗口不会自动探测 / 上次探测失败）→ 显式补一次探测
        request_protocol_probe();
        log::info("[网页版会话] 站点 " + (site.empty() ? std::string("(默认)") : site) +
                  " 内存无凭证：复用已打开的登录窗口，已请求一次协议探测");
    }

    // ---- 等该站点的凭证（页面加载后自动探测 / 每 6 秒补一次）----
    const DWORD retry_ms   = 6000; // 每 6 秒补一次探测（登录动作可能发生在页面加载之后）
    int         probes     = window_ready ? 1 : 0;
    DWORD       next_retry = ::GetTickCount() + retry_ms;

    while (::GetTickCount() < deadline) {
        if (SessionStore::instance().has_token(site)) {
            log::info("[网页版会话] 站点 " + (site.empty() ? std::string("(默认)") : site) +
                      " 已取得内存凭证（userToken 长度 " +
                      std::to_string(SessionStore::instance().snapshot(site).user_token.size()) +
                      "，仅内存；路径=" + (window_ready ? "复用已开窗口补探测" : "离屏开窗探测") + "）");
            return true;
        }
        const DWORD now = ::GetTickCount();
        if (probes < 3 && now >= next_retry) {
            ++probes;
            next_retry = now + retry_ms;
            request_protocol_probe();
            log::info("[网页版会话] 站点 " + (site.empty() ? std::string("(默认)") : site) +
                      " 仍未取得凭证，补一次协议探测（第 " + std::to_string(probes) +
                      " 次；窗口" + (window_ready ? "已打开" : "本次新开") + "）");
        }
        ::Sleep(100);
    }

    if (error != nullptr) {
        *error = (site.empty() ? std::string() : "站点 " + site + " ") +
                 "未取得网页版凭证（内存里没有 userToken）"
                 "：请在已登录的登录窗口内点「探测网页版协议（dev）」后重试，"
                 "或运行 aiwrite.exe --web-probe 确认登录态（退出码 0 = 可用）";
    }
    return false;
}

// M_patchB L1 续（PB2-19）：按**站点**注销
//  * 清该站点的**内存会话**（立即生效，其他站点不受影响）
//  * 删除该站点 origin 的 **Cookie** 与 **localStorage / sessionStorage**（在窗口线程内完成）
//  * **不**删除登录 profile：删除整个 profile（= 清掉所有站点）属高级操作，由参数面板单独入口提供
bool logout_site(const LoginRequest& site_request, int timeout_ms, std::string* error)
{
    const std::string site = site_key_of(site_request.url);

    // 1) 先清内存会话：UI 立刻看到「未登录」，且不影响其他站点
    SessionStore::instance().clear(site);

    // 2) 窗口不在目标站点 → 按串行策略切过去（离屏，仅用于删 Cookie / localStorage）
    if (g_window.load() != nullptr && !window_on_site(site)) {
        log::info("[网页版会话] 按站点注销：窗口从 " + current_window_site() + " 切到 " + site);
        login_window().request_close();
        login_window().join();
    }
    if (g_window.load() == nullptr) {
        LoginRequest request     = site_request;
        request.offscreen        = true;
        request.timeout_seconds  = 0;
        request.probe_after_load = false; // 注销不需要探测
        std::string start_error;
        if (!login_window().start(request, &start_error)) {
            if (error != nullptr) {
                *error = "按站点注销需要打开该站点窗口（站点 " + site + "）：" + start_error;
            }
            return false;
        }
        // 等控制器就绪：CookieManager 需要 ICoreWebView2_2
        const DWORD ready_deadline = ::GetTickCount() + 10000;
        while (g_webview == nullptr && ::GetTickCount() < ready_deadline) {
            ::Sleep(100);
        }
        if (g_webview == nullptr) {
            login_window().request_close();
            login_window().join();
            if (error != nullptr) {
                *error = "按站点注销：WebView2 在 10 秒内未就绪（站点 " + site + "）";
            }
            return false;
        }
    }

    // 3) 窗口线程执行：删该 origin 的 Cookie + 清同源 localStorage
    {
        std::lock_guard<std::mutex> lock(g_logout_mutex);
        g_logout_done    = false;
        g_logout_ok      = false;
        g_logout_error.clear();
        g_logout_deleted = 0;
    }
    if (HWND window = g_window.load(); window != nullptr) {
        ::PostMessageW(window, kLogoutMessage, 0, 0);
    }

    bool        ok  = false;
    std::string why;
    {
        std::unique_lock<std::mutex> lock(g_logout_mutex);
        const auto wait_ms = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 15000);
        if (g_logout_cv.wait_for(lock, wait_ms, [] { return g_logout_done; })) {
            ok  = g_logout_ok;
            why = g_logout_error;
        }
        else {
            why = "按站点注销超时（Cookie 删除未在时限内完成）";
        }
    }

    // 4) 收尾：关闭窗口（该站点已登出）
    login_window().request_close();
    login_window().join();

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
    const HWND window = g_window.load();
    if (window == nullptr || g_webview == nullptr) {
        if (error != nullptr) {
            *error = "登录窗口未就绪（请先在界面「打开登录窗口」登录一次）";
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_script_mutex);
        g_script_js = script_js;
        g_script_result.clear();
        g_script_error.clear();
        g_script_done = false;
    }
    ::PostMessageW(window, kRunScriptMessage, 0, 0);

    std::unique_lock<std::mutex> lock(g_script_mutex);
    const auto wait_ms = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 15000);
    if (!g_script_cv.wait_for(lock, wait_ms, [] { return g_script_done; })) {
        if (error != nullptr) {
            *error = "脚本执行超时（窗口可能未就绪或页面无响应）";
        }
        return false;
    }
    if (json_result != nullptr) {
        *json_result = g_script_result;
    }
    if (error != nullptr) {
        *error = g_script_error;
    }
    return g_script_error.empty();
}

// L3：等页面就绪（readyState = complete；最多 wait_ms）—— DOM 站点/选择器探测用
bool wait_page_ready(int wait_ms)
{
    const DWORD deadline = ::GetTickCount() + static_cast<DWORD>(std::max(0, wait_ms));
    while (::GetTickCount() <= deadline) {
        std::string raw;
        std::string err;
        if (run_script_now("document.readyState", 5000, &raw, &err) &&
            raw.find("complete") != std::string::npos) {
            return true;
        }
        ::Sleep(400);
    }
    return false;
}

// L3（PB2-13 / PB2-15）：按站点在页面内执行一段脚本（同步等待结果；由调用方解析 JSON）
//  * 前置（三级，逐级放宽，**不把「内存 userToken」当通用条件** —— 通用 DOM 站点的登录态在浏览器 profile 里）：
//    ① 窗口已在该站点 → 直接执行
//    ② `ensure_session`（会取凭证 / 复核窗口）成功 → 执行
//    ③ 兜底：按站点**离屏开窗** + 等页面就绪 → 执行（DOM 生成与选择器探测只需页面）
bool run_script_sync(const LoginRequest& site_request, const std::string& script_js, int timeout_ms,
                     std::string* json_result, std::string* error)
{
    if (script_js.empty()) {
        if (error != nullptr) {
            *error = "脚本为空";
        }
        return false;
    }
    const std::string site_key = site_key_of(site_request.url);
    if (!window_on_site(site_key)) {
        std::string boot_error;
        if (!ensure_session(site_request, 15000, &boot_error) && !window_on_site(site_key)) {
            LoginWindow& window = login_window();
            if (window.running()) {
                window.request_close(); // 串行复用（决策 D-20）
                window.join();
            }
            LoginRequest request     = site_request;
            request.offscreen        = true;  // 离屏：不抢焦点（用户可在参数面板打开可见窗口登录）
            request.probe_after_load = false; // 不额外触发协议探测（DOM 路径不依赖 userToken）
            std::string start_error;
            if (!window.start(request, &start_error)) {
                if (error != nullptr) {
                    *error = "打开登录窗口失败：" + start_error +
                             (boot_error.empty() ? std::string() : ("（" + boot_error + "）"));
                }
                return false;
            }
            wait_page_ready(std::min(timeout_ms > 0 ? timeout_ms : 15000, 15000));
        }
    }
    return run_script_now(script_js, timeout_ms, json_result, error);
}

long long solve_pow_via_page(const std::string& site_url, const std::string& challenge_json,
                             int timeout_ms, std::string* error)
{
    if (challenge_json.empty()) {
        if (error != nullptr) {
            *error = "挑战为空";
        }
        return -1;
    }

    // 确保有可用登录窗口（**按目标站点**；PB2-23：不再用无参 ensure_session → 内置默认站点）
    LoginRequest site_request;
    if (!site_url.empty()) {
        site_request.url = site_url;
    }
    site_request.offscreen        = true;
    site_request.probe_after_load = true;
    std::string boot_error;
    if (!ensure_session(site_request, 15000, &boot_error)) {
        if (error != nullptr) {
            *error = boot_error;
        }
        return -1;
    }

    const HWND window = g_window.load();
    if (window == nullptr || g_webview == nullptr) {
        if (error != nullptr) {
            *error = "登录窗口未就绪（请先登录一次：界面「打开登录窗口」或 --login-selftest）";
        }
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(g_pow_mutex);
        g_pow_challenge_json = challenge_json;
        g_pow_done           = false;
        g_pow_answer         = -1;
        g_pow_error.clear();
    }
    ::PostMessageW(window, kSolvePowMessage, 0, 0);

    std::unique_lock<std::mutex> lock(g_pow_mutex);
    const auto                   wait_ms = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 60000);
    if (!g_pow_cv.wait_for(lock, wait_ms, [] { return g_pow_done; })) {
        if (error != nullptr) {
            *error = "页面内 PoW 求解超时";
        }
        return -1;
    }
    if (error != nullptr) {
        *error = g_pow_error;
    }
    return g_pow_answer;
}

// PB2-23：探测主体抽出，供「默认站点」与「按条目」两个入口共用（行为逐字一致）
static int protocol_probe_with(const LoginRequest& base_request, const std::string& site_label,
                               int timeout_seconds)
{
    attach_parent_console();

    const int timeout = timeout_seconds > 0 ? timeout_seconds : 30;
    log::info("[网页版探测] 自检开始（离屏窗口，超时 " + std::to_string(timeout) + " 秒）");
    if (!site_label.empty()) {
        std::printf("[网页版探测] 站点：%s\n", site_label.c_str());
    }

    LoginRequest request            = base_request;
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

// PB2-23：「默认站点」入口（旧行为，逐字不变 —— 守 I2）
int protocol_probe(int timeout_seconds)
{
    return protocol_probe_with(LoginRequest{}, std::string(), timeout_seconds);
}

// PB2-23：「按条目」入口 —— 严格解析站点（**不**回落）；不可用 → 打印原因 + 返回 2（不开窗、不发请求）
int protocol_probe_for_provider(const std::string& provider_id, int timeout_seconds)
{
    attach_parent_console();

    const ai::ProviderSpec* spec = ai::provider_specs().find(provider_id);
    const std::string       site_error = ai::web_site_error(spec);
    if (!site_error.empty()) {
        std::printf("[网页版探测] 站点不可用（条目 %s）：%s\n", provider_id.c_str(),
                    site_error.c_str());
        return 2;
    }

    const ai::ProviderWebSpec site    = ai::strict_web_spec_for(spec);
    const std::string         site_id = ai::strict_web_provider_id_for(spec);
    const LoginRequest        request = probe_login_request(site, site_id);
    std::printf("[网页版探测] 条目 %s → 站点 %s（适配器 %s）\n", provider_id.c_str(),
                request.url.c_str(), site.adapter.empty() ? "builtin" : site.adapter.c_str());
    if (!ai::web_adapter_implemented(site.adapter)) {
        std::printf("[网页版探测] 注意：适配器 %s 本版本尚未实现（已实现：builtin:deepseek）"
                    "—— 登录 / 探测可用，**生成**会明确报错（L3 PB2-13…16）\n",
                    site.adapter.c_str());
    }
    return protocol_probe_with(request, request.url, timeout_seconds);
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
