// ============================================================================
//  webview_host_pow.cpp — 页面内 PoW 求解 + 按站点注销（窗口线程侧）
//  * ARCH-Z1b：自 3514 行单文件 `webview_host.cpp` 逐字拆出，**零语义变化**
//  * 模块内契约见 `webview_host_detail.h`；公开 API 见 `webview_host.h`
// ============================================================================

#include "webview_host_detail.h"
#include "log.h"
#include "probe_verdict.h"

namespace aiwrite::web {


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


} // namespace aiwrite::web
