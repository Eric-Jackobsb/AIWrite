#include "webview_host.h"

#include "log.h"
#include "paths.h"
#include "session_store.h"

#include <windows.h>

#include <wrl/client.h> // Microsoft::WRL::ComPtr
#include <wrl/event.h>  // Microsoft::WRL::Callback

#include <WebView2.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <exception>
#include <filesystem>
#include <utility>
#include <vector>

#include "web_error.h"      // M_patchC（PW-05）：三类归因文案（纯函数）
#include "probe_verdict.h"  // M_patchAB §3.3（PB2-25）：探测判据收紧（纯函数：结论 / 判据 / 下一步）
#include "window_table.h"   // M_patchC（PW-02 / PW-06 / PW-08）：窗口表 + 复位计划（纯函数）

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

// ---- M_patchB L4（PB2-28 / 不变量 I16）：**协议探测不适用**站点的只读诊断脚本 ----
//  * 用于 `adapter=dom` 等**不产生 DeepSeek 协议**的站点：探测按钮 / `--web-probe` 改走本脚本
//  * **绝不**请求任何站点端点（脚本内不出现 `/api/v0/`）、**绝不**读取 localStorage 的**值**
//    （只读键名，故不出现 `localStorage.getItem('userToken')`）
//  * 输出结构与内置探测脚本**完全一致**（`window.__aiwriteProbe.data` 的 JSON 字段同名），
//    故 `poll_protocol_probe()` 的解析逻辑**无需改动**
constexpr const char* kProbeKickoffScriptReadOnly = R"JS(
(function () {
  window.__aiwriteProbe = { done: false, data: '' };
  const out = { keys: '', hasUserToken: false, userTokenMasked: '', settingsJwtMasked: '', tokenRaw: '', challenge: '', endpoints: '', error: '' };
  try {
    const lines = [];
    lines.push('URL: ' + location.href);
    lines.push('标题: ' + (document.title || '(空)'));
    const lsKeys = Object.keys(localStorage);          // 只读**键名**，不读取任何值
    out.keys = lsKeys.join(',');
    lines.push('localStorage 键数: ' + lsKeys.length + '（只读键名，不读值）');
    const cookieNames = (document.cookie || '').split(';')
        .map(function (p) { return p.split('=')[0].trim(); })
        .filter(function (n) { return n.length > 0; });
    lines.push('Cookie 名数: ' + cookieNames.length + '（' + cookieNames.slice(0, 12).join(',') + '）');
    lines.push('输入框候选数: ' + document.querySelectorAll("input, textarea, [contenteditable='true']").length +
               '（input / textarea / contenteditable）');
    lines.push('按钮候选数: ' + document.querySelectorAll('button, [role="button"]').length);
    out.endpoints = lines.join(' || ');
  } catch (e) { out.error = 'readonly:' + e; }
  window.__aiwriteProbe.data = JSON.stringify(out);
  window.__aiwriteProbe.done = true;
  return 'started';
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

} // namespace（**暂时收口**：`WindowCtx` 必须定义在 `aiwrite::web` 名字空间 —— 头文件按此名前向声明）

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
    std::atomic<unsigned> cookie_count{0};
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

namespace { // 重新打开匿名名字空间（窗口表 / 环境缓存 / 全部实现细节）

constexpr int kProbeMaxAttempts = 24;  // 24 × 500ms ≈ 12 秒
constexpr int kPowMaxAttempts   = 240; // 240 × 500ms ≈ 120 秒

constexpr std::size_t kNetRecordLimit = 400; // 网络回执有界缓冲（R13：不能无限增长）

// ---- `PW-02`：**窗口表**（唯一真相 = 键与顺序；`g_windows` 存对应运行时对象）----
//  * `g_table` = 纯函数决策模型（`web/window_table.h`）：复用 / 新建 / 收尾顺序 / 诊断行
//  * `g_windows` = 窗口键 → 窗口运行时（`WindowCtx`）；**只在 `ctx_acquire` / `ctx_close` 里变更**
//    （两处同时改，保证与 `g_table` 不发散）
std::mutex                                        g_table_mutex;
WindowTable                                       g_table;
std::map<std::string, std::shared_ptr<WindowCtx>> g_windows;

// ---- 浏览器**环境**（`D-35④` 修订 2026-10-18：**每窗口线程各建一个**，不再跨线程共享）----
//  * WebView2 对象有**线程亲和性**（必须在创建它的线程上使用，见官方「Threading model」文档）。
//    此前把第一个窗口线程创建的环境缓存进 `g_envs`、交给**别的**窗口线程
//    `CreateCoreWebView2Controller` ⇒ 跨线程调用同步失败 `0x802A000C`
//    （`--web-adapter-selftest --provider all` 9/10 站失败的根因，真机 2026-10-17 日志实证）。
//  * 现在每个窗口线程在自己的线程上创建并独占自己的环境：本线程建、本线程用、窗口关闭即释放。
//    同 profile 的**并发创建**仍由 `g_env_creating` 串行化（首建时 user data folder 被占用）。
//  * `g_env_fail` = 创建失败码缓存：同一 profile 不反复重试空转（如实报错）。
std::mutex                      g_env_mutex;
std::condition_variable         g_env_cv;
std::map<std::string, HRESULT>  g_env_fail;  // profile 目录 → 创建失败码
std::string                     g_env_creating; // 正在创建该 profile

// ---- 「当前窗口」上下文（`thread_local`）----
//  * 窗口线程：线程启动时绑定**自己**的窗口 ⇒ 窗口线程内所有 `st()` 都指自己（含 COM 回调）
//  * 调用方线程：进入公开 API 时用 `CtxScope` 绑定**目标**窗口（由 `ctx_find` 得到）
//  * 这一层替换掉了改造前的「进程级单例」：**同一份代码**现在服务 N 个窗口
thread_local WindowCtx* t_cur = nullptr;

WindowCtx& st() { return *t_cur; }

// 绑定 / 还原「当前窗口」（作用域守卫；支持嵌套）
class CtxScope {
public:
    explicit CtxScope(WindowCtx* ctx) : prev_(t_cur) { t_cur = ctx; }
    ~CtxScope() { t_cur = prev_; }
    CtxScope(const CtxScope&)            = delete;
    CtxScope& operator=(const CtxScope&) = delete;

private:
    WindowCtx* prev_;
};

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
    session.site        = site_key_of(st().request.url);
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
                    summary += cookies.back().name + "=" + mask_value(cookies.back().value);

                    if (raw_name != nullptr) {
                        ::CoTaskMemFree(raw_name);
                    }
                    if (raw_value != nullptr) {
                        ::CoTaskMemFree(raw_value);
                    }
                }

                st().cookie_count = static_cast<unsigned>(cookies.size());
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
    st().result = (code >= 0) ? code : ((st().cookie_count.load() > 0) ? 0 : 1);
    HWND window = st().window.load();
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
    std::lock_guard<std::mutex> lock(g_table_mutex);
    const auto                  it = g_windows.find(key);
    return it == g_windows.end() ? nullptr : it->second;
}

bool ctx_alive(const std::shared_ptr<WindowCtx>& ctx)
{
    return ctx != nullptr && ctx->running.load();
}

// 窗口线程体（绑定 `t_cur` → 跑 `run_login_window` → 回写结果 / `running`）
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
    const std::string site = site_key_of(request.url);
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
        std::lock_guard<std::mutex> lock(g_table_mutex);
        if (g_windows.find(ctx->key) != g_windows.end()) {
            if (error != nullptr) {
                *error = "该归属在该站点已有窗口（" + ctx->key + "）";
            }
            return nullptr;
        }
        g_table.note_opened(owner, site);
        if (const WindowEntry* entry = g_table.find(owner, site); entry != nullptr) {
            ctx->seq = entry->seq; // 诊断序号（收尾顺序由窗口表给出）
        }
        g_windows.emplace(ctx->key, ctx);
    }

    ctx->running = true;
    try {
        ctx->thread = std::thread(window_thread_main, ctx, ctx->request);
    }
    catch (const std::exception& ex) {
        ctx->running = false;
        {
            std::lock_guard<std::mutex> lock(g_table_mutex);
            g_windows.erase(ctx->key);
            g_table.note_closed(owner, site);
        }
        if (error != nullptr) {
            *error = std::string("创建登录线程失败（") + ctx->key + "）: " + ex.what();
        }
        return nullptr;
    }

    log::info("[网页版窗口] 开窗 " + ctx->key + "（profile " + ctx->request.profile_dir +
              "；窗口表：" + g_table.report() + "）");
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
    std::lock_guard<std::mutex> lock(g_table_mutex);
    g_windows.erase(ctx->key);
    g_table.note_closed(ctx->owner, ctx->site_key);
}

// 收尾：**逐窗口**关闭（顺序 = 窗口表给出：后开的先关）
void ctx_close_all()
{
    std::vector<std::shared_ptr<WindowCtx>> all;
    {
        std::lock_guard<std::mutex> lock(g_table_mutex);
        for (const std::string& key : g_table.keys_in_close_order()) {
            const auto it = g_windows.find(key);
            if (it != g_windows.end()) {
                all.push_back(it->second);
            }
        }
    }
    for (const auto& ctx : all) {
        ctx_close(ctx, -1, /*wait=*/true);
    }
}

void finish_protocol_probe(ProbeResult probe)
{
    st().probe_stage = 2;

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
    SessionStore::instance().set_probe(std::move(probe), site_key_of(st().request.url)); // 按站点归档（PB2-18）

    if (st().request.auto_close_after_probe) {
        log::info("[网页版探测] 已自动收尾（关闭登录窗口）");
        close_window(ok ? 0 : 1);
    }
}

void start_protocol_probe()
{
    if (st().webview == nullptr) {
        ProbeResult probe;
        probe.error = "WebView2 尚未就绪";
        finish_protocol_probe(std::move(probe));
        return;
    }

    log::info("[网页版探测] 注入探测脚本（localStorage/userToken + create_pow_challenge）");
    const std::wstring script = to_wide(probe_kickoff_script(st().request));
    const HRESULT      hr     = st().webview->ExecuteScript(
        script.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT result, LPCWSTR /*json_result*/) -> HRESULT {
                if (FAILED(result)) {
                    ProbeResult probe;
                    probe.error = "注入探测脚本失败 HRESULT=" + std::to_string(result);
                    finish_protocol_probe(std::move(probe));
                    return S_OK;
                }
                st().probe_stage     = 1; // 等待异步任务完成，交给 WM_TIMER 轮询
                st().probe_attempt   = 0;
                st().probe_last_poll = ::GetTickCount();
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
    if (st().probe_stage != 1 || st().webview == nullptr) {
        return;
    }
    if (++st().probe_attempt > kProbeMaxAttempts) {
        ProbeResult probe;
        probe.error = "探测超时（结果未在 " + std::to_string(kProbeMaxAttempts * 500 / 1000) +
                      " 秒内返回）";
        finish_protocol_probe(std::move(probe));
        return;
    }

    const std::wstring script = to_wide(kProbePollScript);
    const HRESULT      hr     = st().webview->ExecuteScript(
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
        std::lock_guard<std::mutex> lock(st().script_mutex);
        st().script_result = std::move(result);
        st().script_error  = std::move(error);
        st().script_done   = true;
    }
    st().script_cv.notify_all();
}

// L3：在窗口线程执行 st().script_js（不做解释，原样回传 JSON 结果）
void run_script_in_window()
{
    if (st().webview == nullptr) {
        finish_run_script({}, "WebView2 未就绪");
        return;
    }
    std::string script_js;
    {
        std::lock_guard<std::mutex> lock(st().script_mutex);
        script_js = st().script_js;
    }
    if (script_js.empty()) {
        finish_run_script({}, "脚本为空");
        return;
    }
    const std::wstring script = to_wide(script_js);
    const HRESULT      hr     = st().webview->ExecuteScript(
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

// ---- `PW-08`（`D-35③` 修订 2026-10-17）：复位 = 同一控制器「导航回新会话页」----
//  * 跨线程协议同「脚本槽」：调用方线程 `navigate_now`（PostMessage + 等 nav_cv）；
//    窗口线程 `run_navigate_in_window` 执行 Navigate；`NavigationCompleted` 回调
//    （见 `on_controller_ready`）置完成信号 —— 回调经窗口线程消息循环派发，不锁消息泵。

// 结束一次「复位导航」（窗口线程调用；ok = NavigationCompleted 成功）
void finish_navigate(bool ok, std::string error)
{
    {
        std::lock_guard<std::mutex> lock(st().nav_mutex);
        st().nav_ok      = ok;
        st().nav_error   = std::move(error);
        st().nav_pending = false;
        st().nav_done    = true;
    }
    st().nav_cv.notify_all();
}

// 窗口线程：执行 `st().nav_url` 的导航（login_url 即新会话页；完成 = NavigationCompleted）
void run_navigate_in_window()
{
    if (st().webview == nullptr) {
        finish_navigate(false, "WebView2 未就绪（无法导航复位）");
        return;
    }
    std::string url;
    {
        std::lock_guard<std::mutex> lock(st().nav_mutex);
        url = st().nav_url;
    }
    if (url.empty()) {
        finish_navigate(false, "导航目标为空");
        return;
    }
    log::info("[网页版窗口] 复位导航发起: " + st().key + " → " + url);
    const HRESULT hr = st().webview->Navigate(to_wide(url).c_str());
    if (FAILED(hr)) {
        finish_navigate(false, "Navigate 调用失败（HRESULT " + std::to_string(hr) + "）");
    }
    // 成功 ⇒ 等 NavigationCompleted（现有回调会置完成信号 + 提取 Cookie）
}

// 调用方线程：请窗口线程导航回新会话页并**同步等待**完成（镜像 `run_script_now`）
//  * 用 `ctx` 而非 `st()`：`prepare_window` 在 `CtxScope` 绑定前调用，st() 不可用
bool navigate_now(WindowCtx* ctx, const std::string& url, int timeout_ms, std::string* error)
{
    if (ctx == nullptr) {
        if (error != nullptr) {
            *error = "窗口上下文为空";
        }
        return false;
    }
    if (url.empty()) {
        if (error != nullptr) {
            *error = "导航目标为空";
        }
        return false;
    }
    std::lock_guard<std::mutex> call_lock(ctx->nav_call_mutex); // 同窗口并发调用方串行
    const HWND window = ctx->window.load();
    if (window == nullptr || ctx->webview == nullptr) {
        if (error != nullptr) {
            *error = "窗口 / 页面尚未创建完成（" + ctx->key + "）";
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(ctx->nav_mutex);
        ctx->nav_url     = url;
        ctx->nav_pending = true;
        ctx->nav_done    = false;
        ctx->nav_ok      = false;
        ctx->nav_error.clear();
    }
    ::PostMessageW(window, kNavigateMessage, 0, 0);

    std::unique_lock<std::mutex> lock(ctx->nav_mutex);
    const auto wait_ms = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 15000);
    if (!ctx->nav_cv.wait_for(lock, wait_ms, [ctx] { return !ctx->nav_pending && ctx->nav_done; })) {
        if (error != nullptr) {
            *error = "复位导航超时（页面未完成加载：" + ctx->key + "）";
        }
        return false;
    }
    if (error != nullptr) {
        *error = ctx->nav_error;
    }
    return ctx->nav_ok;
}

// ---- M8-14：把本地文件交给页面（CDP）--------------------------------------------------

// 结束一次「注入文件」（窗口线程调用）
void finish_set_files(bool ok, std::string detail, std::string error)
{
    {
        std::lock_guard<std::mutex> lock(st().files_mutex);
        st().files_ok     = ok;
        st().files_detail = std::move(detail);
        st().files_error  = std::move(error);
        st().files_done   = true;
    }
    st().files_cv.notify_all();
}

// 调用一次 CDP 方法（回调在窗口线程；`params_json` 为 UTF-8 JSON 文本）
void call_cdp(const wchar_t* method, const std::string& params_json,
              std::function<void(HRESULT, const std::string&)> on_done)
{
    if (st().webview == nullptr) {
        on_done(E_FAIL, {});
        return;
    }
    const std::wstring method_name = method;
    const std::wstring params      = to_wide(params_json);
    const HRESULT      hr          = st().webview->CallDevToolsProtocolMethod(
        method_name.c_str(), params.c_str(),
        Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [on_done](HRESULT result, LPCWSTR json_result) -> HRESULT {
                on_done(result, json_result == nullptr ? std::string() : to_utf8(json_result));
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) {
        // 同步失败时回调不会触发 → 必须显式收尾，否则调用方等到超时
        log::warn("[图片上传] CallDevToolsProtocolMethod 同步失败 HRESULT=" + std::to_string(hr));
        on_done(hr, {});
    }
}

// CDP 三步：DOM.getDocument → DOM.querySelector → DOM.setFileInputFiles
void run_set_files_in_window()
{
    if (st().webview == nullptr) {
        finish_set_files(false, {}, "WebView2 未就绪");
        return;
    }
    std::string              selector;
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(st().files_mutex);
        selector = st().files_selector;
        paths    = st().files_paths;
    }
    if (selector.empty()) {
        finish_set_files(false, {}, "选择器为空（站点单元未给 attach_selector）");
        return;
    }
    if (paths.empty()) {
        finish_set_files(false, {}, "文件列表为空");
        return;
    }

    call_cdp(L"DOM.getDocument", "{\"depth\":1}",
             [selector, paths](HRESULT result, const std::string& json_text) {
                 if (FAILED(result)) {
                     finish_set_files(false, {}, "CDP DOM.getDocument 失败（HRESULT " +
                                                     std::to_string(result) + "）");
                     return;
                 }
                 int root_node_id = 0;
                 try {
                     root_node_id = nlohmann::json::parse(json_text).at("root").value("nodeId", 0);
                 }
                 catch (const std::exception&) {
                     root_node_id = 0;
                 }
                 if (root_node_id <= 0) {
                     finish_set_files(false, {},
                                      "CDP DOM.getDocument 未返回根节点（原始：" +
                                          json_text.substr(0, 160) + "）");
                     return;
                 }
                 nlohmann::json query;
                 query["nodeId"]   = root_node_id;
                 query["selector"] = selector;
                 call_cdp(L"DOM.querySelector", query.dump(),
                          [selector, paths](HRESULT query_result, const std::string& query_text) {
                              if (FAILED(query_result)) {
                                  finish_set_files(false, {},
                                                   "CDP DOM.querySelector 失败（HRESULT " +
                                                       std::to_string(query_result) + "）");
                                  return;
                              }
                              int node_id = 0;
                              try {
                                  node_id = nlohmann::json::parse(query_text).value("nodeId", 0);
                              }
                              catch (const std::exception&) {
                                  node_id = 0;
                              }
                              if (node_id <= 0) {
                                  finish_set_files(false, {},
                                                   "选择器未命中页面元素：" + selector +
                                                       "（请跑 --upload-selftest 复核 "
                                                       "web.attach_selector）");
                                  return;
                              }
                              nlohmann::json set_params;
                              set_params["nodeId"] = node_id;
                              set_params["files"]  = paths;
                              call_cdp(L"DOM.setFileInputFiles", set_params.dump(),
                                       [selector, node_id, count = paths.size()](
                                           HRESULT set_result, const std::string& /*json*/) {
                                           if (FAILED(set_result)) {
                                               finish_set_files(
                                                   false, {},
                                                   "CDP DOM.setFileInputFiles 失败（HRESULT " +
                                                       std::to_string(set_result) + "，选择器 " +
                                                       selector + "，nodeId " +
                                                       std::to_string(node_id) + "）");
                                               return;
                                           }
                                           finish_set_files(
                                               true,
                                               "CDP DOM.setFileInputFiles 完成（nodeId=" +
                                                   std::to_string(node_id) + "，文件 " +
                                                   std::to_string(count) + " 个）",
                                               {});
                                       });
                          });
             });
}

// ---- M_patchC（`PW-07f` · 2026-10-08 · **自 `wip/pw07n-20261008` 移植回主分支**）-----------------
//  CDP「真实输入 / 发送」（窗口线程执行；与 `run_set_files_in_window` 同形）
//  为什么必须走 CDP：页面脚本写 contenteditable（`execCommand` / `textContent =`）再发
//  `new KeyboardEvent('keydown', {key:'Enter'})` 时 `isTrusted=false` —— 豆包（tiptap /
//  ProseMirror）用自己的 state 重渲染 ⇒ 直接改 DOM 的文本**被抹掉**、合成回车被忽略
//  ⇒ **消息根本没发出去**（真机 2026-10-08 连续三次 `候选池 0`，页面 60 秒内一条消息都没渲染）。
//  图片路径之所以「能用」：图片走 CDP `DOM.setFileInputFiles`（可信）先落进 composer。

// 结束一次「CDP 真实输入」（窗口线程调用）
void finish_cdp_input(bool ok, std::string detail, std::string error)
{
    {
        std::lock_guard<std::mutex> lock(st().cdp_mutex);
        st().cdp_ok     = ok;
        st().cdp_detail = std::move(detail);
        st().cdp_error  = std::move(error);
        st().cdp_done   = true;
    }
    st().cdp_cv.notify_all();
}

// 一步 CDP 调用（成功后把 `label` 追加进诊断串；任一步失败 → 立即如实收尾）
struct CdpInputStep {
    const wchar_t* method   = L"";
    std::string    params;    // UTF-8 JSON 文本
    std::string    label;     // 成功后写入 detail（空 = 中间步骤，不写）
    std::string    fail_note; // 失败时写入 error 的前缀
};

void run_cdp_input_steps(const std::shared_ptr<std::vector<CdpInputStep>>& steps,
                         const std::shared_ptr<std::size_t>&                index,
                         const std::shared_ptr<std::string>&                detail)
{
    if (*index >= steps->size()) {
        finish_cdp_input(true, *detail, {});
        return;
    }
    const CdpInputStep step = (*steps)[*index];
    call_cdp(step.method, step.params,
             [steps, index, detail, label = step.label, fail_note = step.fail_note](
                 HRESULT result, const std::string& /*json*/) {
                 if (FAILED(result)) {
                     finish_cdp_input(false, *detail,
                                      fail_note + "（HRESULT " + std::to_string(result) + "）");
                     return;
                 }
                 if (!label.empty()) {
                     if (!detail->empty()) {
                         *detail += "｜";
                     }
                     *detail += label;
                 }
                 *index += 1;
                 run_cdp_input_steps(steps, index, detail);
             });
}

void run_cdp_input_in_window()
{
    std::string text;
    bool        press_enter = false;
    {
        std::lock_guard<std::mutex> lock(st().cdp_mutex);
        text        = st().cdp_text;
        press_enter = st().cdp_enter;
    }
    if (st().webview == nullptr) {
        finish_cdp_input(false, {}, "CDP 真实输入：WebView 不可用（窗口已关闭？）");
        return;
    }

    auto steps  = std::make_shared<std::vector<CdpInputStep>>();
    auto detail = std::make_shared<std::string>();

    if (!text.empty()) {
        // `Input.insertText` = **组合输入**语义（等同输入法提交）：站点拿到 isTrusted=true 的
        // 文本输入 ⇒ 富文本编辑器走自己的插入事务（DOM 与内部 state 同步，后续不会被抹掉）
        nlohmann::json params;
        params["text"] = text;
        steps->push_back({L"Input.insertText", params.dump(),
                          "CDP Input.insertText OK（" + std::to_string(text.size()) + " 字节）",
                          "CDP Input.insertText 失败"});
    }
    if (press_enter) {
        // 真实回车 = `keyDown`（带 `text="\r"`，与 Puppeteer `keyboard.press('Enter')` 等效）
        //   + `keyUp`；`windowsVirtualKeyCode=13` ⇒ 站点按真实 Enter 处理（不是合成事件）
        const auto enter_params = [](const char* type, const char* text_field) {
            nlohmann::json p;
            p["type"]                  = type;
            p["key"]                   = "Enter";
            p["code"]                  = "Enter";
            p["windowsVirtualKeyCode"] = 13;
            p["nativeVirtualKeyCode"]  = 13;
            p["text"]                  = text_field;
            p["unmodifiedText"]        = text_field;
            return p.dump();
        };
        steps->push_back({L"Input.dispatchKeyEvent", enter_params("keyDown", "\r"), {},
                          "CDP Input.dispatchKeyEvent(Enter keyDown) 失败"});
        steps->push_back({L"Input.dispatchKeyEvent", enter_params("keyUp", "\r"),
                          "CDP Input.dispatchKeyEvent Enter OK",
                          "CDP Input.dispatchKeyEvent(Enter keyUp) 失败"});
    }
    if (steps->empty()) {
        finish_cdp_input(false, {}, "CDP 真实输入：text 与 press_enter 均为空");
        return;
    }

    const auto index = std::make_shared<std::size_t>(0);
    run_cdp_input_steps(steps, index, detail);
}

// M8-14：网络回执挂钩（窗口线程；控制器就绪后调用一次）
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



void finish_pow_solve(long long answer, std::string error)
{
    {
        std::lock_guard<std::mutex> lock(st().pow_mutex);
        st().pow_answer = answer;
        st().pow_error  = std::move(error);
        st().pow_done   = true;
    }
    st().pow_cv.notify_all();
}

// PoW：在窗口线程执行（注入挑战 → 运行页面内求解脚本）
void run_pow_solve_in_page()
{
    if (st().webview == nullptr) {
        finish_pow_solve(-1, "WebView2 未就绪");
        return;
    }

    std::string challenge_json;
    {
        std::lock_guard<std::mutex> lock(st().pow_mutex);
        challenge_json = st().pow_challenge_json;
    }
    if (challenge_json.empty()) {
        finish_pow_solve(-1, "挑战为空");
        return;
    }

    // 1) 挑战 JSON 写入页面全局（内容是合法 JSON，可安全内联）
    const std::wstring inject =
        to_wide("window.__aiwritePowChallenge = " + challenge_json + "; 'ok';");
    const HRESULT inject_hr = st().webview->ExecuteScript(
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
    const HRESULT      hr     = st().webview->ExecuteScript(
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
    st().pow_stage     = 1;
    st().pow_attempt   = 0;
    st().pow_last_poll = ::GetTickCount();
}

// PoW：轮询页面里的求解结果（WM_TIMER 驱动）
void poll_pow_solve()
{
    if (st().pow_stage != 1 || st().webview == nullptr) {
        return;
    }
    if (++st().pow_attempt > kPowMaxAttempts) {
        st().pow_stage = 2;
        finish_pow_solve(-1, "页面内 PoW 求解超时");
        return;
    }

    const std::wstring script = to_wide(kSolvePowPollScript);
    const HRESULT      hr     = st().webview->ExecuteScript(
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

                st().pow_stage = 2;
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
        st().pow_stage = 2;
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
        std::lock_guard<std::mutex> lock(st().logout_mutex);
        st().logout_ok    = ok;
        st().logout_error = std::move(error);
        st().logout_done  = true;
    }
    st().logout_cv.notify_all();
}

void run_logout_in_page()
{
    if (st().webview == nullptr) {
        finish_logout(false, "WebView2 尚未就绪");
        return;
    }

    ComPtr<ICoreWebView2_2> webview2;
    if (FAILED(st().webview.As(&webview2)) || webview2 == nullptr) {
        finish_logout(false, "当前 WebView2 运行时过旧，不支持 CookieManager（请更新 Runtime）");
        return;
    }
    ComPtr<ICoreWebView2CookieManager> manager;
    if (FAILED(webview2->get_CookieManager(&manager)) || manager == nullptr) {
        finish_logout(false, "获取 ICoreWebView2CookieManager 失败");
        return;
    }

    const std::wstring target_url = to_wide(st().request.url);
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
                st().logout_deleted = deleted;
                log::info("[网页版注销] 站点 Cookie 删除：" + std::to_string(deleted) + " / " +
                          std::to_string(count) + " 条（" + st().request.url + "）");

                if (st().webview == nullptr) {
                    finish_logout(true, std::string());
                    return S_OK;
                }
                // 同源 localStorage / sessionStorage 一并清掉（网页版 token 就存在 localStorage）
                const HRESULT script_hr = st().webview->ExecuteScript(
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
//  * `g_env_creating` = 「该 profile 正在被某个窗口线程创建」：同 profile 的并发创建**等它建好**
//    （首建时 user data folder 被占用，同时创建会失败）。
//  * 失败码进缓存（`g_env_fail`）：同一 profile 不反复重试空转（如实报错）
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
            std::lock_guard<std::mutex> lock(g_env_mutex);
            g_env_fail[profile_dir] = result;
            if (g_env_creating == profile_dir) {
                g_env_creating.clear();
            }
        }
        g_env_cv.notify_all();
        st().result = 1;
        ::PostMessageW(st().window.load(), WM_CLOSE, 0, 0);
        return S_OK;
    }

    // ① 环境归**本窗口线程**私有（`D-35④` 修订 2026-10-18）：**不进**进程级 `g_envs` 缓存。
    //    跨线程复用环境 ⇒ 第二个窗口 `CreateCoreWebView2Controller` 同步失败 0x802A000C
    //    （线程亲和性违反，真机 2026-10-17 批量探测实证）。环境随窗口关闭一并释放。
    {
        std::lock_guard<std::mutex> lock(g_env_mutex);
        g_env_fail.erase(profile_dir);
        if (g_env_creating == profile_dir) {
            g_env_creating.clear();
        }
    }
    g_env_cv.notify_all();

    // ② 本窗口**自己**的 controller（创建线程 = 环境线程 ⇒ 满足线程亲和）
    st().environment = environment;
    create_controller_for_window(environment);
    return S_OK;
}

// 取（必要时建）**本窗口**要用的环境；失败 → false + 可操作 `error`
//  * `D-35④`（2026-10-18）：**每个窗口线程都自己创建环境**（本线程用，不跨线程复用）——
//    跨线程复用会撞 0x802A000C（线程亲和）。同 profile 的并发创建由 `g_env_creating` 串行化。
bool ensure_environment(HWND window, std::string* error)
{
    const std::string profile_dir = st().request.profile_dir;
    constexpr DWORD   kEnvWaitMs  = 30000; // 等「同 profile 的创建者」完成（首次可能较慢）
    const DWORD       deadline    = ::GetTickCount() + kEnvWaitMs;
    bool              create_now  = false;
    bool              env_timeout = false;
    HRESULT           fail_code   = S_OK;

    {
        std::unique_lock<std::mutex> lock(g_env_mutex);
        for (;;) {
            if (const auto it = g_env_fail.find(profile_dir); it != g_env_fail.end()) {
                fail_code = it->second;
                break;
            }
            if (g_env_creating.empty()) {
                g_env_creating = profile_dir; // 本线程来建（同 profile 同时只允许一个创建者）
                create_now     = true;
                break;
            }
            if (::GetTickCount() >= deadline) {
                env_timeout = true;
                break;
            }
            g_env_cv.wait_for(lock, std::chrono::milliseconds(200));
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
                std::lock_guard<std::mutex> lock(g_env_mutex);
                g_env_fail[profile_dir] = hr;
                if (g_env_creating == profile_dir) {
                    g_env_creating.clear();
                }
            }
            g_env_cv.notify_all();
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
            close_window(st().cookie_count.load() > 0 ? 0 : 2);
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
            st().result = (st().cookie_count.load() > 0) ? 0 : 1;
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

} // namespace

// ---- M_patchB L1（PB2-05）：站点参数化 —— 渲染探测脚本（外部可见，供离线断言）----
//  * 不传站点参数时，渲染结果与改造前的常量脚本逐字一致（行为不变）
std::string probe_kickoff_script(const LoginRequest& request)
{
    // ---- M_patchB L4（PB2-28 / 不变量 I16）：**不适用**协议探测的站点 → 只读诊断脚本 ----
    //  * 不请求任何站点端点（无 `/api/v0/*`）、不读取 localStorage 的值（含 `userToken`）
    //  * `probe_applicable` 默认 true → 无参 / CLI（`--web-probe`、`--web-chat`、`--login-selftest`）
    //    与内置站点路径**逐字不变**（守 I2）
    if (!request.probe_applicable) {
        return std::string(kProbeKickoffScriptReadOnly);
    }
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
//  `PW-02`：`LoginWindow` 从「进程内单例窗口」改为**窗口句柄**（绑定一个 `owner`）——
//   * 界面用 `login_window()`（owner = `界面#登录窗口`）：它也是**独立**窗口，
//     不再被节点的 `ensure_session` 抢走（真机错配的另一半）
//   * `start()` 把句柄指向 `(owner, 该请求站点)` 的窗口；**只关同一个 owner 的旧窗口**
//     （界面上切换站点仍是「一个可见窗口」⇒ 与今天一致；节点 owner 之间互不影响 · `I24`）
LoginWindow::LoginWindow(WebOwner owner) : owner_(std::move(owner)) {}

LoginWindow::~LoginWindow()
{
    request_close();
    join();
}

bool LoginWindow::start(const LoginRequest& request, std::string* error)
{
    const std::string site = site_key_of(request.url);

    // ① 同一个 owner 在其他站点还有窗口 ⇒ 先关它（换站点 = 换这一个可见窗口；`I24` 允许：同一个 owner）
    const std::vector<std::string> owned = [this] {
        std::lock_guard<std::mutex> lock(g_table_mutex);
        return g_table.keys_of_owner(owner_);
    }();
    for (const std::string& key : owned) {
        if (key == window_key(owner_, site)) {
            continue;
        }
        std::shared_ptr<WindowCtx> other;
        {
            std::lock_guard<std::mutex> lock(g_table_mutex);
            const auto                  it = g_windows.find(key);
            if (it != g_windows.end()) {
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

void LoginWindow::request_close()
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
        std::lock_guard<std::mutex> lock(g_table_mutex);
        order = g_table.keys_in_close_order(); // 后开的在前
    }
    for (const std::string& key : order) {
        std::shared_ptr<WindowCtx> ctx;
        {
            std::lock_guard<std::mutex> lock(g_table_mutex);
            const auto                  it = g_windows.find(key);
            if (it != g_windows.end()) {
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
    const std::string          site = site_key_of(site_request.url);
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
            std::lock_guard<std::mutex> lock(g_table_mutex);
            return g_table.report();
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

void request_protocol_probe()
{
    const std::shared_ptr<WindowCtx> ctx = ctx_latest_alive();
    if (ctx == nullptr) {
        log::warn("[网页版探测] 没有已打开的登录窗口：请先点「打开登录窗口（WebView2）」并完成登录");
        return;
    }
    ctx_post_probe(ctx);
}

// 当前窗口所属站点（origin；无窗口 = 空）—— **按归属**（`PW-02`：不再是「进程里那一个窗口」）
std::string current_window_site(const WebOwner& owner, const std::string& site_key)
{
    const std::shared_ptr<WindowCtx> ctx = ctx_find(owner, site_key);
    if (!ctx_alive(ctx)) {
        return {};
    }
    return ctx->site_key;
}

// 该归属在**指定站点**是否有活着的窗口（界面判定 / 调用前置）
bool window_on_site(const WebOwner& owner, const std::string& site)
{
    if (site.empty()) {
        return false;
    }
    const std::shared_ptr<WindowCtx> ctx = ctx_find(owner, site);
    return ctx_alive(ctx) && ctx->site_key == site;
}

// 窗口表诊断行（`PW-02`：谁开着哪些窗口 —— 日志 / 断言 / 界面可读）
std::string windows_report()
{
    std::lock_guard<std::mutex> lock(g_table_mutex);
    return g_table.report();
}

// 关闭**某归属**的窗口（显式收尾 / 换站点用；不碰别的归属）
void close_owner_window(const WebOwner& owner, const std::string& site_key)
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
bool ensure_session(int timeout_ms, std::string* error)
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
    return ensure_session(request, timeout_ms, error);
}

// M_patchB L1 续（PB2-17/18/19）：按**站点**确保凭证 + 取用**本归属自己的**窗口
//  * `PW-02`（`I24`）**核心修正**：站点不同 ⇒ **不再关别人的窗口**，而是**开/取该站点自己的**窗口
//    （真机 2026-10-06 19:39:12 那条「串行复用：先关闭旧窗口」由此消失）
//  * `PW-05`：失败文案带 `owner` 标签（谁要的）+ 站点 + 可操作下一步
bool ensure_session(const LoginRequest& site_request, int timeout_ms, std::string* error)
{
    const std::string site = site_key_of(site_request.url);
    // ---- M_patchB L4（PB2-27 / 决策 D-27 / 不变量 I15）：**站点无关**的就绪判据 ----
    //  * 内置协议站点（probe_applicable=true，如 deepseek-web）：仍需内存 userToken
    //    （页面内 PoW / 端点调用依赖它 → 逐字保持改造前语义，守 I2）
    //  * DOM 站点（probe_applicable=false）：登录态由浏览器 profile 维持 →
    //    **只看该 origin 有没有 Cookie**，绝不因「内存里没有 userToken」空等到超时（AB2-20）
    const bool needs_token = site_request.probe_applicable;
    const auto ready       = [&site, needs_token]() {
        if (needs_token) {
            return SessionStore::instance().has_token(site);
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
bool logout_site(const LoginRequest& site_request, int timeout_ms, std::string* error)
{
    const std::string site = site_key_of(site_request.url);

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

// ---- M_patchC（`PW-03` · 不变量 `I25`）：就绪三级 —— **实现** ----
//  * L1 窗口级：窗口在该站点 + controller / webview 已创建（复用 logout 里的既有写法）
//  * L2 页面级：`document.readyState == complete` **且实时 URL 的站点键 == 目标站点**
//  * L3 交互态级：可见输入候选 > 0（+ 上传场景：`input[type=file]` > 0）
//  * **只读**：只读页面状态；不点击 / 不写入 / 不改导航（复位归 `PW-08` / `D-35③`）

namespace {

constexpr DWORD kReadyCacheMs = 1500; // 就绪缓存有效期（轮询期间的免探针窗口）

void remember_ready(const std::string& site, int mask)
{
    std::lock_guard<std::mutex> lock(st().ready_mutex);
    st().ready_site = site;
    st().ready_mask = mask;
    st().ready_tick = ::GetTickCount();
}

int cached_ready(const std::string& site, int need)
{
    std::lock_guard<std::mutex> lock(st().ready_mutex);
    if (st().ready_site != site || st().ready_mask == 0) {
        return 0;
    }
    if (::GetTickCount() - st().ready_tick > kReadyCacheMs) {
        return 0;
    }
    return ready_satisfied(st().ready_mask, need) ? st().ready_mask : 0;
}

// L1：等 controller / webview 创建完成（50 ms 粒度；与 logout_site 既有写法同源）
bool wait_controller_ready(int timeout_ms, std::string* error)
{
    const DWORD deadline = ::GetTickCount() + static_cast<DWORD>(timeout_ms > 0 ? timeout_ms : 0);
    while (::GetTickCount() < deadline) {
        if (st().window.load() != nullptr && st().webview != nullptr) {
            return true;
        }
        ::Sleep(50);
    }
    if (error != nullptr) {
        *error = "WebView2 控制器在时限内未就绪";
    }
    return false;
}

// 页面状态探针（**只读**）：URL / readyState / 可见输入候选 / `input[type=file]` 计数
constexpr const char* kPageStateScript = R"JS(
(function () {
  const out = { url: '', ready_state: '', composer_hits: 0, composer_total: 0, file_inputs: 0 };
  try {
    out.url = location.href || '';
    out.ready_state = document.readyState || '';
    const pool = 'textarea, input[type="text"], input[type="search"], input:not([type]), ' +
                 '[contenteditable="true"], [contenteditable=""], [role="textbox"]';
    const nodes = document.querySelectorAll(pool);
    out.composer_total = nodes.length;
    let visible = 0;
    for (let i = 0; i < nodes.length; i++) {
      const r = nodes[i].getBoundingClientRect();
      if (r.width > 1 && r.height > 1) { visible += 1; }
    }
    out.composer_hits = visible;
    out.file_inputs = document.querySelectorAll('input[type=file]').length;
  } catch (e) {
    out.ready_state = 'error:' + ((e && e.message) ? e.message : String(e));
  }
  return out;
})();
)JS";

} // namespace

bool peek_page_state(const LoginRequest& site_request, WebPageState* out, std::string* error)
{
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "输出参数为空";
        }
        return false;
    }
    *out = WebPageState{};

    // `PW-02`：**只读**探针只作用于「本归属在该站点的窗口」——没有窗口就如实报（不另开窗）
    const std::shared_ptr<WindowCtx> ctx = ctx_find(site_request.owner, site_key_of(site_request.url));
    if (!ctx_alive(ctx)) {
        if (error != nullptr) {
            *error = web_error_text(WebErrorKind::NotReady, site_request.owner,
                                    site_key_of(site_request.url), "该归属在该站点暂无窗口");
        }
        return false;
    }
    CtxScope scope(ctx.get());

    std::string raw;
    std::string why;
    if (!run_script_now(kPageStateScript, 8000, &raw, &why)) {
        if (error != nullptr) {
            *error = "读取页面状态失败：" + why;
        }
        return false;
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(raw);
        out->url                  = parsed.value("url", std::string());
        out->ready_state          = parsed.value("ready_state", std::string());
        out->ready_state_complete = out->ready_state == "complete";
        out->composer_hits        = parsed.value("composer_hits", 0);
        out->file_inputs          = parsed.value("file_inputs", 0);
        return true;
    }
    catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = std::string("页面状态解析失败：") + ex.what() + "；原始结果：" + raw.substr(0, 160);
        }
        return false;
    }
}

int wait_ready(const LoginRequest& site_request, int need, int timeout_ms, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (need == kReadyNone) {
        return kReadyNone;
    }
    need |= kReadyWindow; // L2 / L3 都隐含「窗口存在」

    const std::string site     = site_key_of(site_request.url);
    const std::string tag      = owner_tag(site_request.owner);
    const int         budget   = timeout_ms > 0 ? timeout_ms : 15000;
    const DWORD       deadline = ::GetTickCount() + static_cast<DWORD>(budget);

    // ---- `PW-02` / `PW-08`：取用**本归属自己的**该站点窗口（复用 / 新建 / 首次复位）----
    //  * **不再**「关掉别人的窗口」；`prepare_window` 里由**窗口表**决策落点
    std::shared_ptr<WindowCtx> ctx;
    if (!prepare_window(site_request, &ctx, error)) {
        return kReadyNone;
    }
    CtxScope scope(ctx.get());

    const int cached = cached_ready(site, need);
    if (cached != 0) {
        return cached;
    }

    int reached = kReadyNone;

    // 观测缓存（2026-10-10 · 循环开发 `T-L06` 诊断补充）：失败时把**实时页面事实**写进 error
    //  * 动机（真机）：`gemini-web` / `spark-web` 只报「页面未就绪（缺 页面级）」——
    //    而「readyState 未完成」与「实时 URL 已不在目标站点（登录墙 / 反爬跳转）」是**两种病**，
    //    没有实时事实 ⇒ 只能记「环境级 · 未定」，无法定性（[loop_dev](../../docs/loop_dev.md) §5.1 第③行）
    WebPageState last_state;
    bool         have_state       = false;
    std::string  last_probe_error;

    // ---- L1 窗口级（窗口已由 `prepare_window` 备好；这里只等 controller / webview 创建完成）----
    {
        const DWORD now  = ::GetTickCount();
        const int   left = now < deadline ? static_cast<int>(deadline - now) : 0;
        std::string why;
        if (!wait_controller_ready(left, &why)) {
            if (error != nullptr) {
                *error = web_error_text(WebErrorKind::NotReady, site_request.owner, site,
                                        ready_missing_text(need, reached) + "（" + why + "）");
            }
            ctx->failed_before = true; // `PW-08`：下一次调用会复位一次（关窗重开）
            return reached;
        }
        reached |= kReadyWindow;
    }

    // ---- L2 / L3：轮询页面状态（逐级达成 / 逐级日志）----
    while (::GetTickCount() < deadline) {
        WebPageState state;
        std::string  why;
        if (peek_page_state(site_request, &state, &why)) {
            last_state = state;   // 诊断缓存：无论达标与否都留最新事实
            have_state = true;
            if ((reached & kReadyPage) == 0 && state.ready_state_complete &&
                site_key_of(state.url) == site) {
                reached |= kReadyPage;
                log::info(tag + " 就绪 L2 页面级：readyState=complete，URL=" + state.url);
            }
            if ((reached & kReadyPage) != 0) {
                if ((reached & kReadyComposer) == 0 && state.composer_hits > 0) {
                    reached |= kReadyComposer;
                    log::info(tag + " 就绪 L3 交互态：可见输入候选 " +
                              std::to_string(state.composer_hits) + " 个");
                }
                if ((reached & kReadyFileInput) == 0 && state.file_inputs > 0) {
                    reached |= kReadyFileInput;
                    log::info(tag + " 就绪 L3 交互态：input[type=file] 命中 " +
                              std::to_string(state.file_inputs) + " 个");
                }
            }
            if (ready_satisfied(reached, need)) {
                remember_ready(site, reached);
                ctx->failed_before = false; // `PW-08`：成功 ⇒ 清掉「失败一次」的记账
                return reached;
            }
        }
        else {
            last_probe_error = why; // 连状态都读不到（脚本未执行 / 页面无文档）⇒ 也是事实
        }
        ::Sleep(200);
    }

    // 失败：**归因分离**（`PW-05`：只说「哪一级没过 + 下一步」；**不得**说成「形态不支持 / 拖拽入口」）
    std::string detail = ready_missing_text(need, reached);
    if (have_state) {
        detail += ready_detail_text(last_state.url, last_state.ready_state, last_state.composer_hits);
    }
    else {
        detail += "｜实时页面：**取不到页面状态**" +
                  (last_probe_error.empty() ? std::string() : ("（" + last_probe_error + "）"));
    }
    ctx->failed_before = true; // `PW-08`：下一次调用会复位一次（关窗重开）
    if (error != nullptr) {
        *error = web_error_text(WebErrorKind::NotReady, site_request.owner, site, detail);
    }
    log::warn(tag + " 就绪失败（" + std::to_string(need) + " 位需求 / 已达 " +
              std::to_string(reached) + "）：" + detail);
    return reached;
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

    // ---- `PW-02` / `PW-08`：取用**本归属自己的**该站点窗口（复用 / 新建 / 首次复位）----
    //  * **不再**「关掉别人的窗口」（`I24`）；同站点不同 owner 各开各的
    std::shared_ptr<WindowCtx> ctx;
    if (!prepare_window(site_request, &ctx, error)) {
        return false;
    }
    CtxScope scope(ctx.get());

    // `PW-05`：同窗口**串行执行**守卫（同一线程重入放行）—— 并发时给「可分辨」的原因，不静默串槽
    bool        busy_ok = false;
    BusyGuard   busy(*ctx, site_key, &busy_ok, error);
    if (!busy_ok) {
        return false;
    }

    // ---- M_patchC PW-03（不变量 I25）：脚本前置 = L1 窗口级 + L2 页面级 ----
    //  * **不再**把「窗口尚在创建 / 页面还在导航」当成「脚本失败」—— 先逐级等就绪（消除 113 ms 假失败）
    //  * 交互态（L3）由调用方按需另行要求（上传路径 / 文字生成），以免影响只读诊断（dump / probe）
    const int   need_ready = kReadyWindow | kReadyPage;
    std::string ready_error;
    const int   reached = wait_ready(site_request, need_ready,
                                     std::min(timeout_ms > 0 ? timeout_ms : 15000, 15000),
                                     &ready_error);
    if (!ready_satisfied(reached, need_ready)) {
        if (error != nullptr) {
            *error = ready_error.empty()
                         ? web_error_text(WebErrorKind::NotReady, site_request.owner, site_key,
                                          "脚本未执行（就绪未达成）")
                         : ready_error;
        }
        return false;
    }
    const bool ok = run_script_now(script_js, timeout_ms, json_result, error);
    ctx->failed_before = !ok; // `PW-08`：失败 ⇒ 下一次调用复位一次
    return ok;
}

// ---- M8-14 续（`PW-04` · `I26`）：网络回执**按归属** ----
//  * 回执存在**该归属自己的窗口**里 ⇒ 结构上不可能跨 owner 串（旧实现 = 一个全进程缓冲）
//  * 取用时再用 `owner_accepts` 核一次「回执归属 == 要它的归属」：
//    未绑定 / 跨归属 ⇒ **丢弃并告警**（`I26`：未绑定或跨 owner 的证据**不得**用于 `I18`）
void begin_resource_capture(const WebOwner& owner, const std::string& site_key)
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

bool resource_capture_ready(const WebOwner& owner, const std::string& site_key)
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
    const std::string site_key = site_key_of(site_request.url);
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
    const int   reached = wait_ready(site_request, need_ready,
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
    const std::string site_key = site_key_of(site_request.url);
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
    const int   reached = wait_ready(site_request, need_ready,
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


namespace {

// M8-37：**只读**枚举页面里的 `input[type=file]` —— 供 attach_selector 未回填 / 未命中时兜底
//  * 与 `--upload-selftest` 的「选择器未命中」诊断同源：枚举 → 挑最优 → 给出可回填清单
//  * 只读：不点击、不写入、不读取任何值；失败一律如实报错（绝不静默换选择器）
constexpr const char* kFileInputProbeScript = R"JS(
(function () {
  const out = { count: 0, items: [] };
  try {
    const nodes = document.querySelectorAll('input[type=file]');
    out.count = nodes.length;
    for (let i = 0; i < nodes.length && i < 8; i++) {
      const el = nodes[i];
      const cls = (typeof el.className === 'string') ? el.className : '';
      const r = el.getBoundingClientRect();
      const first = cls.replace(/\s+/g, ' ').trim().split(' ')[0];
      out.items.push({ accept: (el.getAttribute('accept') || '').slice(0, 120),
                       multiple: el.multiple === true,
                       visible: r.width > 1 && r.height > 1,
                       selector: el.id ? ('#' + el.id) : (first ? ('.' + first) : 'input[type=file]') });
    }
  } catch (e) { out.count = -1; }
  return out;
})();
)JS";

} // namespace

bool resolve_file_input_selector(const LoginRequest& site_request, int timeout_ms,
                                 std::string* selector, std::string* detail, std::string* error)
{
    if (selector != nullptr) {
        selector->clear();
    }
    if (detail != nullptr) {
        detail->clear();
    }

    // ---- M_patchC PW-03（不变量 I25）：先等**附件入口级**就绪 ----
    //  * 真机 41 ms 假失败 = 附件区懒加载未挂载时枚举到 0 个，却被归因为「形态不支持」（`G6`）
    //  * 这里先等到 L3（含附件入口）；未就绪 → 如实报「页面未就绪」，**不**报形态结论
    const int   need_ready = kReadyUpload;
    std::string ready_error;
    const int   reached = wait_ready(site_request, need_ready,
                                     std::min(timeout_ms > 0 ? timeout_ms : 10000, 15000),
                                     &ready_error);
    if (!ready_satisfied(reached, need_ready)) {
        if (error != nullptr) {
            *error = ready_error.empty()
                         ? web_error_text(WebErrorKind::NotReady, site_request.owner,
                                          site_key_of(site_request.url), "未枚举附件入口")
                         : ready_error;
        }
        return false;
    }

    std::string raw;
    std::string why;
    if (!run_script_sync(site_request, kFileInputProbeScript, timeout_ms > 0 ? timeout_ms : 10000, &raw,
                         &why)) {
        if (error != nullptr) {
            *error = web_error_text(WebErrorKind::NotReady, site_request.owner,
                                    site_key_of(site_request.url),
                                    "读取页面 `input[type=file]` 失败：" + why);
        }
        return false;
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(raw);
        const int            count  = parsed.value("count", 0);
        if (count < 0) {
            if (error != nullptr) {
                *error = "页面脚本异常：无法枚举 `input[type=file]`";
            }
            return false;
        }
        const nlohmann::json items = parsed.value("items", nlohmann::json::array());
        std::string list = "页面里 `input[type=file]` 共 " + std::to_string(count) + " 个";
        if (count == 0) {
            // 归因分离（`G6`）：就绪三级**已通过**（上面刚等到附件入口）⇒ 这里是**竞态**
            // （懒加载卸载 / 换帧），不再归因为「该站点用拖拽 / 粘贴入口」
            list += "（就绪检查已通过，但枚举瞬间没有 `input[type=file]` —— 多为懒加载竞态；请重试）";
        }
        std::string with_image;   // accept 含 image（最可能是图片入口）
        std::string hidden_input; // 隐藏且带 accept（附件入口通常隐藏）
        std::string first_input;  // 文档顺序首个
        for (const auto& item : items) {
            const std::string sel    = item.value("selector", std::string("?"));
            const std::string accept = item.value("accept", std::string());
            const bool        visible = item.value("visible", false);
            list += "\n    · " + sel + "（accept=" + (accept.empty() ? std::string("(无)") : accept) +
                    "，multiple=" + (item.value("multiple", false) ? "true" : "false") + "，可见=" +
                    (visible ? "是" : "否") + "）";
            if (first_input.empty()) {
                first_input = sel;
            }
            if (with_image.empty() && accept.find("image") != std::string::npos) {
                with_image = sel;
            }
            if (hidden_input.empty() && !visible && !accept.empty()) {
                hidden_input = sel;
            }
        }
        // 取值策略（确定性，口径写在这里）：accept 含 image → 隐藏且带 accept → 文档顺序首个
        const std::string picked = !with_image.empty()    ? with_image
                                   : !hidden_input.empty() ? hidden_input
                                                           : first_input;
        if (picked.empty()) {
            if (error != nullptr) {
                // **归因分离**（`PW-05` · `W-D6`）：L3（含附件入口）**已通过**却枚举不到 ⇒
                // 这是「无入口 / 竞态」，**不得**再写成「页面未就绪」，也不得写「形态不支持」以外的猜测
                *error = web_error_text(
                    WebErrorKind::NoEntry, site_request.owner, site_key_of(site_request.url),
                    "枚举到 " + std::to_string(count) + " 个 `input[type=file]`（就绪已达：" +
                        ready_reached_text(reached) + "；就绪检查此前已通过 ⇒ 多为懒加载竞态）");
            }
            if (detail != nullptr) {
                *detail = list;
            }
            return false;
        }
        if (selector != nullptr) {
            *selector = picked;
        }
        if (detail != nullptr) {
            *detail = list + "\n    ⇒ 自动识别选择器：" + picked;
        }
        return true;
    }
    catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = std::string("枚举结果解析失败：") + ex.what() + "；原始结果：" + raw.substr(0, 160);
        }
        return false;
    }
}

long long solve_pow_via_page(const std::string& site_url, const std::string& challenge_json,
                             int timeout_ms, std::string* error, const WebOwner& owner)
{
    if (challenge_json.empty()) {
        if (error != nullptr) {
            *error = "挑战为空";
        }
        return -1;
    }

    // 确保有可用登录窗口（**按目标站点**；PB2-23：不再用无参 ensure_session → 内置默认站点）
    //  * `PW-02`：归属显式 —— 默认为**未标注**（内置协议路径 `web_chat` 尚未把节点 owner 穿下来，
    //    故它使用「未标注」归属在该站点的那一个窗口；节点级 DOM 路径不受影响）
    LoginRequest site_request;
    if (!site_url.empty()) {
        site_request.url = site_url;
    }
    site_request.owner            = owner;
    site_request.offscreen        = true;
    site_request.probe_after_load = true;
    std::string boot_error;
    if (!ensure_session(site_request, 15000, &boot_error)) {
        if (error != nullptr) {
            *error = boot_error;
        }
        return -1;
    }

    const std::string          site = site_key_of(site_request.url);
    std::shared_ptr<WindowCtx> ctx  = ctx_find(site_request.owner, site);
    if (!ctx_alive(ctx) || ctx->webview == nullptr) {
        if (error != nullptr) {
            *error = web_error_text(WebErrorKind::NotReady, site_request.owner, site,
                                    "窗口 / 页面尚未创建完成（PoW 求解需要该站点页面）");
        }
        return -1;
    }
    CtxScope scope(ctx.get());

    const HWND window = st().window.load();
    {
        std::lock_guard<std::mutex> lock(st().pow_mutex);
        st().pow_challenge_json = challenge_json;
        st().pow_done           = false;
        st().pow_answer         = -1;
        st().pow_error.clear();
    }
    ::PostMessageW(window, kSolvePowMessage, 0, 0);

    std::unique_lock<std::mutex> lock(st().pow_mutex);
    const auto                   wait_ms = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 60000);
    if (!st().pow_cv.wait_for(lock, wait_ms, [] { return st().pow_done; })) {
        if (error != nullptr) {
            *error = web_error_text(WebErrorKind::NotReady, site_request.owner, site,
                                    "页面内 PoW 求解超时");
        }
        return -1;
    }
    if (error != nullptr) {
        *error = st().pow_error;
    }
    return st().pow_answer;
}

// PB2-23：探测主体抽出，供「默认站点」与「按条目」两个入口共用（行为逐字一致）
static int protocol_probe_with(const LoginRequest& base_request, const ai::ProviderSpec* spec,
                               const std::string& site_label, int timeout_seconds)
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
    request.probe_after_load       = true; // 页面加载完成后自动执行（适用 → 协议探测；不适用 → 只读诊断）
    request.auto_close_after_probe = true; // 探测完成即关窗退出

    // L4（PB2-28 / 不变量 I16）：**不适用**协议探测的站点（如 DOM）→ 说明清楚，不再打印「未取得凭证」
    if (!request.probe_applicable) {
        std::printf("[网页版探测] 协议探测：**不适用**（该站点不是内置协议站点）—— 改跑**只读诊断**：\n"
                    "             URL / 标题 / localStorage 键名与个数 / Cookie 名与个数 / 输入框与按钮候选数；\n"
                    "             **不**请求任何站点端点，**不**读取 localStorage 的值（含 userToken）\n");
    }

    LoginWindow window(selftest_owner()); // `PW-02`：探测自检用独立归属（不与节点 / 界面窗口互抢）
    std::string error;
    if (!window.start(request, &error)) {
        std::printf("[网页版探测] 启动失败: %s\n", error.c_str());
        return 1;
    }
    window.join();

    const Session      session = SessionStore::instance().snapshot();
    const Session      site_session = SessionStore::instance().snapshot(site_key_of(request.url));
    const ProbeResult& probe   = session.probe;

    // ---- L4（PB2-28 / I16）：不适用站点 → 只读诊断输出 + **站点无关**登录态结论 ----
    if (!request.probe_applicable) {
        std::printf("\n===== 网页版只读诊断（脱敏；该站点不适用协议探测）=====\n");
        std::printf("诊断            : %s\n", probe.endpoints_report.c_str());
        std::printf("localStorage 键 : %s\n", probe.local_storage_keys.c_str());
        std::printf("错误            : %s\n", probe.error.c_str());

        const ai::WebSessionVerdict verdict =
            ai::web_session_state(spec, web_session_evidence(site_session));
        std::printf("站点            : %s\n", request.url.c_str());
        std::printf("登录态          : %s（Cookie %zu 条）\n",
                    ai::web_session_state_label(verdict.state).c_str(), site_session.cookies.size());
        std::printf("判据            : %s\n", verdict.reason.c_str());
        std::printf("================================================\n");
        if (verdict.state != ai::WebSessionState::logged_in) {
            std::printf("[网页版探测] 提示：该站点未登录 → 请点界面参数面板的「打开登录窗口」**手动登录**；\n"
                        "             该站点不适用协议探测，登录后**无需**再点「探测网页版协议」\n");
        }
        log::info("[网页版探测] 只读诊断结束（不适用协议探测）：" + verdict.reason);

        SessionStore::instance().clear(site_key_of(request.url)); // 自检不留会话
        if (!probe.ok) {
            return 1; // 诊断脚本本身失败（页面没加载 / WebView2 异常）
        }
        return verdict.state == ai::WebSessionState::logged_in ? 0 : 2; // 2 = 未登录 / 未确认
    }

    std::printf("\n===== 网页版协议探测（脱敏）=====\n");
    std::printf("localStorage 键 : %s\n", probe.local_storage_keys.c_str());
    std::printf("userToken      : %s\n",
                probe.has_user_token ? probe.user_token_masked.c_str() : "未获取（可能未登录）");
    std::printf("settingsJwt    : %s\n", probe.settings_jwt_masked.c_str());
    std::printf("challenge      : %s\n", probe.challenge_json.c_str());
    std::printf("端点探测       : %s\n", probe.endpoints_report.c_str());
    std::printf("错误           : %s\n", probe.error.c_str());
    std::printf("================================\n");

    // M_patchAB §3.3（PB2-25）：判据**收紧** —— 「有凭证」不再等于 PASS：
    //  * 还需**判据端点** HTTP 200 且 `code=0`（旧判据在端点 `code=40003` 时会**假 PASS**）
    //  * 结论四分类与可操作下一步由纯函数 `judge_probe_result()` 给出（离线可断言）
    const std::string required_path = request.probe_paths.empty()
                                          ? std::string("/api/v0/users/current")
                                          : request.probe_paths.front();
    const ProbeJudgement judgement =
        judge_probe_result(probe, required_path, /*probe_applicable=*/true);
    std::printf("结论           : %s\n", probe_verdict_name(judgement.verdict).c_str());
    std::printf("判据           : %s\n", judgement.reason.c_str());
    std::printf("下一步         : %s\n", judgement.next_step.c_str());
    std::printf("================================\n");
    if (judgement.needs_relogin) {
        std::printf("[网页版探测] 提示：**会话已失效** → 请在界面参数面板点「打开登录窗口」重新登录；\n"
                    "             界面错误条同样会给出「重新登录」入口（PB-07）\n");
    }
    std::printf("[网页版探测] 结果：%s（退出码 %d）\n", judgement.pass ? "PASS" : "FAIL",
                judgement.pass ? 0 : 1);
    log::info("[网页版探测] 自检结束：" + probe.summary() + " ｜ 结论=" +
              probe_verdict_name(judgement.verdict) + "：" + judgement.reason);

    SessionStore::instance().clear(); // 自检不留会话
    return judgement.pass ? 0 : 1;
}

// PB2-23：「默认站点」入口（旧行为，逐字不变 —— 守 I2）
int protocol_probe(int timeout_seconds)
{
    return protocol_probe_with(LoginRequest{}, nullptr, std::string(), timeout_seconds);
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
    // L4（PB2-28）：不适用协议探测的站点 → 只读诊断 + 登录态结论（不再打印「未取得凭证」）
    return protocol_probe_with(request, spec, request.url, timeout_seconds);
}

void stop_login_window()
{
    // `PW-02`：收尾 = **逐窗口**（顺序由窗口表给出：后开的先关）—— 不再只关「那一个窗口」
    ctx_close_all();
    // 失败缓存一并释放（同进程内再开窗会重新尝试创建环境）
    {
        std::lock_guard<std::mutex> lock(g_env_mutex);
        g_env_fail.clear();
        g_env_creating.clear();
    }
}

} // namespace aiwrite::web
