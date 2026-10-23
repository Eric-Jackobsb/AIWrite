// ============================================================================
//  webview_host_probe.cpp — 协议探测（两段式）/ 就绪三级 / 页面状态探针 / wait_ready
//  * ARCH-Z1b：自 3514 行单文件 `webview_host.cpp` 逐字拆出，**零语义变化**
//  * 模块内契约见 `webview_host_detail.h`；公开 API 见 `webview_host.h`
// ============================================================================

#include "webview_host_detail.h"
#include "log.h"
#include "probe_verdict.h"
#include "ready_level.h"

namespace aiwrite::web {

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

// 在登录窗口内执行协议探测（必须运行在窗口线程）
//  两段式：start 注入脚本 → WM_TIMER 调 poll 轮询结果 → finish 落库（+ 可选自动关窗）
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
    SessionStore::instance().setProbe(std::move(probe), siteKeyOf(st().request.url)); // 按站点归档（PB2-18）

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
    const std::wstring script = to_wide(probeKickoffScript(st().request));
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
// ---- M_patchB L1（PB2-05）：站点参数化 —— 渲染探测脚本（外部可见，供离线断言）----
//  * 不传站点参数时，渲染结果与改造前的常量脚本逐字一致（行为不变）
std::string probeKickoffScript(const LoginRequest& request)
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

// ---- M_patchC（`PW-03` · 不变量 `I25`）：就绪三级 —— **实现** ----
//  * L1 窗口级：窗口在该站点 + controller / webview 已创建（复用 logout 里的既有写法）
//  * L2 页面级：`document.readyState == complete` **且实时 URL 的站点键 == 目标站点**
//  * L3 交互态级：可见输入候选 > 0（+ 上传场景：`input[type=file]` > 0）
//  * **只读**：只读页面状态；不点击 / 不写入 / 不改导航（复位归 `PW-08` / `D-35③`）


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


bool peekPageState(const LoginRequest& site_request, WebPageState* out, std::string* error)
{
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "输出参数为空";
        }
        return false;
    }
    *out = WebPageState{};

    // `PW-02`：**只读**探针只作用于「本归属在该站点的窗口」——没有窗口就如实报（不另开窗）
    const std::shared_ptr<WindowCtx> ctx = ctx_find(site_request.owner, siteKeyOf(site_request.url));
    if (!ctx_alive(ctx)) {
        if (error != nullptr) {
            *error = web_error_text(WebErrorKind::NotReady, site_request.owner,
                                    siteKeyOf(site_request.url), "该归属在该站点暂无窗口");
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

int waitReady(const LoginRequest& site_request, int need, int timeout_ms, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (need == kReadyNone) {
        return kReadyNone;
    }
    need |= kReadyWindow; // L2 / L3 都隐含「窗口存在」

    const std::string site     = siteKeyOf(site_request.url);
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
        if (peekPageState(site_request, &state, &why)) {
            last_state = state;   // 诊断缓存：无论达标与否都留最新事实
            have_state = true;
            if ((reached & kReadyPage) == 0 && state.ready_state_complete &&
                siteKeyOf(state.url) == site) {
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
    const std::string site_key = siteKeyOf(site_request.url);

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
    const int   reached = waitReady(site_request, need_ready,
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

int protocolProbe(int timeout_seconds)
{
    return protocol_probe_with(LoginRequest{}, nullptr, std::string(), timeout_seconds);
}

// PB2-23：「按条目」入口 —— 严格解析站点（**不**回落）；不可用 → 打印原因 + 返回 2（不开窗、不发请求）
int protocolProbeForProvider(const std::string& provider_id, int timeout_seconds)
{
    attach_parent_console();

    const ai::ProviderSpec* spec = ai::providerSpecs().find(provider_id);
    const std::string       site_error = ai::webSiteError(spec);
    if (!site_error.empty()) {
        std::printf("[网页版探测] 站点不可用（条目 %s）：%s\n", provider_id.c_str(),
                    site_error.c_str());
        return 2;
    }

    const ai::ProviderWebSpec site    = ai::strictWebSpecFor(spec);
    const std::string         site_id = ai::strictWebProviderIdFor(spec);
    const LoginRequest        request = probe_login_request(site, site_id);
    std::printf("[网页版探测] 条目 %s → 站点 %s（适配器 %s）\n", provider_id.c_str(),
                request.url.c_str(), site.adapter.empty() ? "builtin" : site.adapter.c_str());
    if (!ai::webAdapterImplemented(site.adapter)) {
        std::printf("[网页版探测] 注意：适配器 %s 本版本尚未实现（已实现：builtin:deepseek）"
                    "—— 登录 / 探测可用，**生成**会明确报错（L3 PB2-13…16）\n",
                    site.adapter.c_str());
    }
    // L4（PB2-28）：不适用协议探测的站点 → 只读诊断 + 登录态结论（不再打印「未取得凭证」）
    return protocol_probe_with(request, spec, request.url, timeout_seconds);
}

void stopLoginWindow()
{
    // `PW-02`：收尾 = **逐窗口**（顺序由窗口表给出：后开的先关）—— 不再只关「那一个窗口」
    ctx_close_all();
    // 失败缓存一并释放（同进程内再开窗会重新尝试创建环境）
    {
        std::lock_guard<std::mutex> lock(envCache().mutex);
        envCache().fail.clear();
        envCache().creating.clear();
    }
}


} // namespace aiwrite::web
