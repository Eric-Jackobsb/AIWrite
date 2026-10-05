#include "ai/dom_web_client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "utils/log.h"
// M7B 批 3：**生产链路与诊断工具统一走新通道**（`M7B-18` 诊断换代 → `M7B-20` 生产换代）
//  * 本文件**不再 include `web/webview_host.h`**（该头拉入 WebView2 依赖）——
//    站点描述与全部纯函数已搬迁至 `web/site_ref.h`（零语义），会话证据在 `web/session_store.h`
#include "web/pydoll_channel.h"
#include "web/session_store.h"
#include "web/site_ref.h"

namespace aiwrite::ai {
namespace {

// 轮询参数硬上限（R13：站点字段填错 / 打错也不至于无限等待）
constexpr int kMinPollMs    = 200;
constexpr int kMaxPollMs    = 2000;
constexpr int kMinPolls     = 10;
constexpr int kMaxPolls     = 600;
constexpr int kStableRounds = 3; // 未配 done_when 时：文本连续 N 轮不变即完成

// 页面内「写入提示词 + 触发发送」（读 window.__aiwriteDom；返回诊断对象）
constexpr const char* kKickoffScript = R"JS(
(function () {
  const cfg = window.__aiwriteDom || {};
  const out = { ok: false, error: '', input_selector: cfg.input_selector || '',
                input_hits: 0, input_tag: '', send_kind: cfg.send_kind || 'key', send_hits: 0 };
  try {
    const sel = cfg.input_selector || '';
    const el = sel ? document.querySelector(sel) : null;
    out.input_hits = sel ? document.querySelectorAll(sel).length : 0;
    if (!el) { out.error = 'input_selector 未命中：' + (sel || '（空）'); return out; }
    out.input_tag = (el.tagName || '').toLowerCase();
    el.focus();
    const text = cfg.prompt || '';
    if (el.isContentEditable) {
      let inserted = false;
      try { inserted = document.execCommand('insertText', false, text); } catch (e) { inserted = false; }
      if (!inserted || !el.textContent) { el.textContent = text; }
      el.dispatchEvent(new InputEvent('input', { bubbles: true, cancelable: true, data: text,
                                                 inputType: 'insertText' }));
    } else {
      const proto = el.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
      const desc = Object.getOwnPropertyDescriptor(proto, 'value');
      if (desc && desc.set) { desc.set.call(el, text); } else { el.value = text; }
      el.dispatchEvent(new Event('input', { bubbles: true }));
      el.dispatchEvent(new Event('change', { bubbles: true }));
      if (el.value !== text) { out.error = 'input 写入失败（站点可能拦截脚本输入）'; return out; }
    }
    const kind = cfg.send_kind || 'key';
    if (kind === 'click') {
      const btn = cfg.send_value ? document.querySelector(cfg.send_value) : null;
      if (!btn) { out.error = 'send.selector 未命中：' + (cfg.send_value || '（空）'); return out; }
      btn.click();
      out.send_hits = 1;
    } else {
      const key = cfg.send_value || 'Enter';
      const init = { key: key, code: key, keyCode: key === 'Enter' ? 13 : 0,
                     which: key === 'Enter' ? 13 : 0, bubbles: true, cancelable: true, composed: true };
      ['keydown', 'keypress', 'keyup'].forEach(function (t) { el.dispatchEvent(new KeyboardEvent(t, init)); });
      out.send_hits = 1;
    }
    out.ok = true;
  } catch (e) { out.error = 'DOM 执行异常：' + (e && e.message ? e.message : String(e)); }
  return out;
})();
)JS";

// 页面内「取答案 + 判结束」（读 window.__aiwriteDom）
constexpr const char* kPollScript = R"JS(
(function () {
  const cfg = window.__aiwriteDom || {};
  const out = { found: false, count: 0, chars: 0, text: '', done: false, done_kind: cfg.done_kind || '' };
  try {
    const sel = cfg.answer_selector || '';
    const nodes = sel ? document.querySelectorAll(sel) : [];
    out.count = nodes.length;
    if (nodes.length > 0) {
      const last = nodes[nodes.length - 1];
      out.text = (last.innerText || last.textContent || '').trim();
      out.chars = out.text.length;
      out.found = true;
    }
    const dsel = cfg.done_selector || '';
    if (out.done_kind === 'selector_present') { out.done = dsel ? document.querySelectorAll(dsel).length > 0 : false; }
    else if (out.done_kind === 'selector_gone') { out.done = dsel ? document.querySelectorAll(dsel).length === 0 : false; }
  } catch (e) { out.text = ''; }
  return out;
})();
)JS";

// PB2-15：选择器探测（读 window.__aiwriteDomProbe）—— **只读诊断，不发送任何内容**
constexpr const char* kProbeScript = R"JS(
(function () {
  const cfg = window.__aiwriteDomProbe || {};
  const out = { url: location.href, title: document.title || '',
                input_selector: cfg.input_selector || '', input_hits: 0, input_visible: false,
                send_kind: cfg.send_kind || 'key', send_value: cfg.send_value || '', send_hits: 0,
                answer_selector: cfg.answer_selector || '', answer_hits: 0, answer_chars: 0,
                done_kind: cfg.done_kind || '', done_now: null,
                token_expr: cfg.token_expr || '', token_len: -1, token_json: false,
                cookies_wanted: cfg.cookie_names || [], cookies_present: [], error: '' };
  try {
    const q = function (s) { try { return s ? document.querySelectorAll(s) : []; } catch (e) { return []; } };
    const inputs = q(out.input_selector);
    out.input_hits = inputs.length;
    if (inputs.length > 0) {
      const r = inputs[0].getBoundingClientRect();
      out.input_visible = r.width > 0 && r.height > 0;
    }
    out.send_hits = (out.send_kind === 'click') ? q(out.send_value).length : out.input_hits;
    const answers = q(out.answer_selector);
    out.answer_hits = answers.length;
    if (answers.length > 0) { out.answer_chars = (answers[answers.length - 1].innerText || '').length; }
    const dsel = cfg.done_selector || '';
    if (out.done_kind === 'selector_present') { out.done_now = q(dsel).length > 0; }
    else if (out.done_kind === 'selector_gone') { out.done_now = q(dsel).length === 0; }
    if (out.token_expr) {
      try {
        const v = eval(out.token_expr);
        if (typeof v === 'string') {
          out.token_len = v.length;
          const t = v.trim();
          out.token_json = (t.charAt(0) === '{' || t.charAt(0) === '[' || t.charAt(0) === '"');
        } else if (v === null || v === undefined) { out.token_len = -1; }
      } catch (e) { out.token_len = -2; }
    }
    const jar = document.cookie || '';
    (out.cookies_wanted || []).forEach(function (n) {
      if (jar.indexOf(n + '=') >= 0) { out.cookies_present.push(n); }
    });
  } catch (e) { out.error = (e && e.message) ? e.message : String(e); }
  return out;
})();
)JS";
// M_patchB L4（PB2-29 / v15）：**选择器候选枚举**（只读）—— 让「填选择器」不必靠人肉 F12
//  * 在**当前页面**里枚举候选输入框 / 发送按钮 / 回答容器，回传标签·id·class·placeholder·aria 等指纹
//  * C++ 侧再据此给出**建议选择器**（优先 id → placeholder/aria → tag.class…），配合命中数探测即可逐站回填
//  * 纯只读：不写入、不发送、不读取任何值（与 `kProbeScript` 同族）
constexpr const char* kDiscoverScript = R"JS(
(function () {
  const out = { url: location.href, title: document.title || '', inputs: [], sends: [], answers: [],
                cookies: [], keys: '', input_candidates: 0 };
  const visible = function (el) {
    try { const r = el.getBoundingClientRect(); return r.width > 1 && r.height > 1; } catch (e) { return false; }
  };
  const desc = function (el) {
    const cls = (typeof el.className === 'string') ? el.className : '';
    return {
      tag: el.tagName.toLowerCase(),
      id: el.id || '',
      cls: cls.replace(/\s+/g, ' ').trim().slice(0, 200),
      name: el.getAttribute('name') || '',
      ph: el.getAttribute('placeholder') || '',
      ce: el.getAttribute('contenteditable') || '',
      aria: el.getAttribute('aria-label') || '',
      role: el.getAttribute('role') || '',
      visible: visible(el),
      text: (el.innerText || '').replace(/\s+/g, ' ').trim().slice(0, 40)
    };
  };
  const push = function (list, el, limit) { if (list.length < limit) { list.push(desc(el)); } };
  try {
    const inputs = document.querySelectorAll('textarea, input[type="text"], input[type="search"], input:not([type]), [contenteditable="true"], [contenteditable=""], [role="textbox"]');
    out.input_candidates = inputs.length;
    inputs.forEach(function (el) { push(out.inputs, el, 14); });
    document.querySelectorAll('button, [role="button"], [type="submit"], a[class*="send"], [class*="send"]')
      .forEach(function (el) { push(out.sends, el, 14); });
    document.querySelectorAll('[class*="markdown"], [class*="message"], [class*="answer"], [class*="chat-content"], [class*="response"], article, [role="listitem"]')
      .forEach(function (el) { push(out.answers, el, 14); });
    out.cookies = (document.cookie || '').split(';')
      .map(function (p) { return p.split('=')[0].trim(); })
      .filter(function (n) { return n.length > 0; });
    try { out.keys = Object.keys(localStorage).slice(0, 40).join(','); } catch (e) { out.keys = ''; }
  } catch (e) { out.error = (e && e.message) ? e.message : String(e); }
  return JSON.stringify(out);
})();
)JS";



} // namespace

void clamp_poll_params(const ProviderWebSpec& site, int* poll_ms, int* max_polls)
{
    const int interval = site.answer_poll_ms > 0 ? site.answer_poll_ms : 500;
    const int polls    = site.answer_max_polls > 0 ? site.answer_max_polls : 120;
    if (poll_ms != nullptr) {
        *poll_ms = std::clamp(interval, kMinPollMs, kMaxPollMs);
    }
    if (max_polls != nullptr) {
        *max_polls = std::clamp(polls, kMinPolls, kMaxPolls);
    }
}

std::string dom_cfg_json(const DomChatRequest& request)
{
    nlohmann::json cfg;
    cfg["prompt"]          = request.prompt;
    cfg["input_selector"]  = request.site.input_selector;
    cfg["send_kind"]       = request.site.send_kind.empty() ? std::string("key") : request.site.send_kind;
    cfg["send_value"]      = request.site.send_value.empty() ? std::string("Enter") : request.site.send_value;
    cfg["answer_selector"] = request.site.answer_selector;
    cfg["done_kind"]       = request.site.done_kind;
    cfg["done_selector"]   = request.site.done_selector;
    cfg["stable_rounds"]   = kStableRounds;
    return cfg.dump();
}

std::string dom_kickoff_script() { return std::string(kKickoffScript); }
std::string dom_poll_script() { return std::string(kPollScript); }
std::string dom_probe_script() { return std::string(kProbeScript); }

DomChatResult dom_chat(const DomChatRequest& request)
{
    DomChatResult result;
    const auto    started = std::chrono::steady_clock::now();
    const auto    finish  = [&result, started]() {
        result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
        return result;
    };

    // ---- 前置：站点与生成字段（登录型条目在 local_nodes 已拦，此处再兜一层；纯函数可断言）----
    if (request.site.login_url.empty()) {
        result.error = "条目缺少 web.login_url（站点登录页）";
        return finish();
    }
    if (request.site.input_selector.empty() || request.site.answer_selector.empty() ||
        request.site.send_kind.empty() || request.site.send_value.empty()) {
        result.error = "DOM 适配器需要 web.input_selector / web.answer_selector / web.send{kind,value}"
                       "（缺项见 `--provider-dump` 的「登录型站点条目」警告）";
        return finish();
    }

    // ---- 1) 按站点确保会话（`M7B-20`：**新通道** Pydoll 守护进程）----
    //  * 语义 = 「确保**有活会话且已导航到站点**」（`channel::ensure_session` = 起常驻浏览器 +
    //    `open_tab` + `I15′` 登录态判定）；已有会话则**复用**（不重复起浏览器）
    //  * 未登录 / 未取到会话**不阻断**：DOM 站点的可写性由页面自己暴露 ——
    //    「输入框在不在 / 能不能写入」交给下面的脚本诊断给出可操作原因
    //  * ⚠️ 与旧内嵌 WebView2 窗口不同：新通道起的是**独立有头浏览器 + 常驻守护进程**，
    //    会话**不随本进程消失** ⇒ 由 `main.cpp` 在应用退出时统一 `shutdown_session()` 收尾
    // `M7B step 11`：会话请求改**有头**（`visible_login_request`）—— 新通道的浏览器是用户
    // **唯一的登录入口**；离屏窗口看不见 ⇒ 未登录时无路可走（违反 `I21`）。旧 `boot_login_request`
    // （离屏）只服务 `builtin:deepseek` 协议栈（批 5 随其退役）。
    const web::LoginRequest site_request =
        web::visible_login_request(request.site, request.provider_id);
    std::string boot_error;
    const int   boot_timeout = std::max(15000, std::min(request.timeout_ms, 60000));
    if (!web::channel::ensure_session(site_request, boot_timeout, &boot_error)) {
        result.warning = "本次未取到已登录会话（仍会继续尝试驱动页面；选择器命中情况见脚本诊断）：" +
                         boot_error;
    }

    // ---- 2) 内容返回（**v4 · step 14 · P4**）：`send_prompt`（**真打字**注入 + 触发发送）----
    //  * 站点知识**由调用方下发**（`I14`）：`input_selector` / `send{kind,value}` 来自条目 `web` 段
    //  * `upload_evidence = false`：**纯文本生成不置** `I18` 位（只有图片链路才置 → `P7b-16`）
    //  * ⚠️ DOM 脚本常量（`dom_cfg_json` / `dom_kickoff_script` / `dom_poll_script`）**保留**为
    //    **诊断资产**（`--web-dom-dump` / `--web-adapter-selftest` 与 `VB2-22` 仍在使用），
    //    但**生产路径不再使用** ⇒ 口径由「DOM 层零改动」更新为「**纯函数不变 + 生产不再使用**」
    int poll_ms   = 0;
    int max_polls = 0;
    clamp_poll_params(request.site, &poll_ms, &max_polls);

    const std::string send_kind  = request.site.send_kind.empty() ? "key" : request.site.send_kind;
    const std::string send_value = request.site.send_value.empty() ? "Enter" : request.site.send_value;
    const int         send_timeout = std::max(15000, std::min(request.timeout_ms, 60000));
    std::string       send_error;
    if (!web::channel::send_prompt(site_request, request.prompt,
                                   std::vector<std::string>{request.site.input_selector},
                                   send_kind, send_value, /*upload_evidence=*/false, send_timeout,
                                   &send_error)) {
        result.error = "注入 / 发送失败：" + send_error +
                       "（用 --web-adapter-selftest --provider " + request.provider_id +
                       " 复核选择器）";
        return finish();
    }
    result.steps = "send_prompt（" + send_kind + "）+ read_answer";

    // ---- 3) 取回答：`read_answer`（轮询 + **显式截断**）----
    //  * 判据 `done_when`（条目 `web.done_when`）由调用方下发；缺省 = 文本连续 N 轮稳定
    //  * 硬上限：`poll_ms` / `max_polls`（`clamp_poll_params` 已钳制）+ 总超时
    //  * **截断显式**（`I21` · `M7B-55`）：超 48 KiB → `truncated=true` ⇒ 写入 `warning`
    std::string answer_text;
    std::string answer_error;
    bool        truncated  = false;
    int         text_bytes = 0;
    if (!web::channel::read_answer(site_request,
                                   std::vector<std::string>{request.site.answer_selector},
                                   request.site.done_kind, poll_ms, max_polls,
                                   request.timeout_ms > 0 ? request.timeout_ms : 180000,
                                   &answer_text, &truncated, &text_bytes, &answer_error)) {
        result.error = "未取到答案文本：" + answer_error;
        return finish();
    }
    result.text  = answer_text;
    result.polls = 0;   // v4：轮询在 Python 侧（`read_answer`）—— 协议不回传次数
    if (truncated) {
        result.warning = "回答正文超过 48 KiB：**已截断**（`truncated=true`，原始 " +
                         std::to_string(text_bytes) +
                         " 字节）—— 如需完整正文请缩短站点回答，或改用官方 API 条目";
    }
    if (result.text.empty()) {
        result.error = "未取到答案文本（`answer_selector` 未命中或站点仍在生成）";
        if (!answer_error.empty()) {
            result.error += "（" + answer_error + "）";
        }
        result.error += "；可用 --web-adapter-selftest --provider <id> 诊断选择器";
        return finish();
    }
    result.ok = true;
    log::info("[DOM 适配器] 站点=" + request.site.login_url + "（send_prompt + read_answer · " +
              std::to_string(result.elapsed_ms) + " ms）文本 " + std::to_string(result.text.size()) +
              " 字节" + (result.warning.empty() ? "" : ("；警告：" + result.warning)) +
              (result.error.empty() ? "" : ("；错误：" + result.error)));
    return finish();
}

// M_patchB L4（PB2-29 / v15）：**选择器候选枚举**（只读）—— 输出候选元素指纹 + 建议选择器
//  * 用途：`--web-dom-dump --provider <id>`；配合 `--web-adapter-selftest` 的命中数即可逐站回填选择器
//  * 只读：不写入、不发送、不读取任何值
std::string suggest_selector_of(const nlohmann::json& d)
{
    const std::string id   = d.value("id", std::string());
    const std::string ph   = d.value("ph", std::string());
    const std::string aria = d.value("aria", std::string());
    const std::string tag  = d.value("tag", std::string("div"));
    if (!id.empty()) {
        return "#" + id;
    }
    if (!ph.empty()) {
        return tag + "[placeholder=\"" + ph + "\"]";
    }
    if (!aria.empty()) {
        return "[aria-label=\"" + aria + "\"]";
    }
    // class 兜底：取前两个「不像哈希/状态」的类名（含数字且较长的一律跳过）
    const std::string cls = d.value("cls", std::string());
    std::vector<std::string> keep;
    std::size_t              begin = 0;
    while (begin < cls.size() && keep.size() < 2) {
        const std::size_t end  = cls.find(' ', begin);
        const std::string item = cls.substr(begin, end == std::string::npos ? std::string::npos
                                                                           : end - begin);
        if (!item.empty() && item.size() <= 24 &&
            !(item.find_first_of("0123456789") != std::string::npos && item.size() > 14)) {
            keep.push_back(item);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    if (keep.empty()) {
        return {};
    }
    std::string selector = tag;
    for (const std::string& item : keep) {
        selector += "." + item;
    }
    return selector;
}

// M7B 批 3 step 9（`M7B-18`）：**诊断工具走新通道**（Pydoll 守护进程 + `run_script`）——
//  * 会话由诊断函数**自己起**（换代后的 `channel::ensure_session` = 起常驻浏览器 + `open_tab` + 判登录态）
//  * `ChannelSessionGuard` 保证**任何 return / 抛异常**都收尾（不留孤儿浏览器 / 守护进程）；
//    WebView2 版不需要它（内嵌窗口随进程退出）⇒ 只在走新通道的**诊断**函数里用
namespace {
struct ChannelSessionGuard {
    ~ChannelSessionGuard()
    {
        if (aiwrite::web::channel::session_ready()) {
            aiwrite::web::channel::shutdown_session();
        }
    }
};
} // namespace

int dom_selector_dump(const std::string& provider_id, int timeout_ms)
{
  try { // 兜底：把任何 C++ 异常转成可读错误（否则 std::terminate → __fastfail 0xC0000409，输出全丢）
    const ChannelSessionGuard guard;   // 起会话后**任何路径**都收尾（含提前 return / 抛异常）
    const ProviderSpec* spec       = provider_specs().find(provider_id);
    const std::string   site_error = web_site_error(spec);
    if (!site_error.empty()) {
        std::printf("[候选枚举] 站点不可用（条目 %s）：%s\n", provider_id.c_str(), site_error.c_str());
        return 2;
    }
    const ProviderWebSpec   site    = strict_web_spec_for(spec);
    const std::string       site_id = strict_web_provider_id_for(spec);
    const web::LoginRequest request = web::interactive_login_request(site, site_id);

    std::printf("\n===== 选择器候选枚举（只读；用于填 input_selector / send / answer_selector）=====\n");
    std::printf("条目     : %s（%s）\n", spec->id.c_str(), spec->display.c_str());
    std::printf("登录页   : %s\n", site.login_url.c_str());
    std::fflush(stdout); // 排障：崩溃前已产生的输出先落盘（__fastfail 会丢缓冲）

    std::string boot_error;
    const int   boot_timeout =
        std::max(10000, std::min(timeout_ms > 0 ? timeout_ms * 1000 : 30000, 60000));
    // 会话 = **新通道**（`M7B-18` 换代）：起常驻浏览器 + `open_tab`（站点 `url`）+ `I15′` 登录态
    const bool        session_ok = web::channel::ensure_session(request, boot_timeout, &boot_error);
    const std::string site_key   = web::login_request_site(request);
    const web::Session session   = web::SessionStore::instance().snapshot(site_key);
    const WebSessionVerdict verdict =
        web_session_state(spec, web::web_session_evidence(session));
    std::printf("站点键   : %s\n", site_key.c_str());
    std::printf("登录态   : %s（%s）\n", web_session_state_label(verdict.state).c_str(),
                verdict.reason.c_str());
    std::printf("说明     : %s\n",
                session_ok ? "页面已就绪（候选来自**当前页面**）"
                           : "未取到内存会话：候选来自**未登录页面**（登录后建议重跑）");
    if (!session_ok && !boot_error.empty()) {
        std::printf("会话提示 : %s\n", boot_error.c_str());
    }
    std::fflush(stdout); // 排障：确认 ensure_session 已返回

    std::string raw;
    std::string script_error;
    if (!web::channel::run_script(request, std::string(kDiscoverScript), 15000, &raw,
                                  &script_error)) {
        std::printf("枚举失败 : %s\n", script_error.c_str());
        std::printf("（提示：窗口 / 页面未就绪，或站点拒绝脚本执行；可重跑本命令）\n");
        return 1;
    }
    if (!script_error.empty()) {
        // ⚠️ **成功但截断**（> 48 KiB）：`run_script` 用 `*error` 给说明 —— 不得静默（`I21`）
        std::printf("警告      : %s\n", script_error.c_str());
    }
    std::fflush(stdout); // 排障：确认发现脚本已执行
    nlohmann::json root;
    try {
        nlohmann::json parsed = nlohmann::json::parse(raw);
        // ExecuteScript 会把「脚本 return 的字符串」再包一层 JSON 字符串（既有做法见
        // webview_host.cpp 的 poll_protocol_probe）→ 这里统一解包，避免 type_error.306
        if (parsed.is_string()) {
            parsed = nlohmann::json::parse(parsed.get<std::string>());
        }
        if (!parsed.is_object()) {
            std::printf("枚举结果不是对象（实际类型 %s）：%s\n", parsed.type_name(),
                        raw.substr(0, 200).c_str());
            return 1;
        }
        root = std::move(parsed);
    }
    catch (const std::exception& ex) {
        std::printf("枚举结果解析失败：%s；原始：%s\n", ex.what(), raw.substr(0, 200).c_str());
        return 1;
    }
    std::printf("\nURL      : %s\n", root.value("url", std::string("?")).c_str());
    std::printf("标题     : %s\n", root.value("title", std::string("?")).c_str());
    {
        // 页面 Cookie 名（**只读名**，不含值）+ localStorage 键名（截断）—— 用于后续补 cookie_names
        const nlohmann::json cookies =
            root.contains("cookies") ? root["cookies"] : nlohmann::json::array();
        std::string names;
        for (const nlohmann::json& item : cookies) {
            if (!item.is_string()) {
                continue;
            }
            if (!names.empty()) {
                names += ",";
            }
            names += item.get<std::string>();
        }
        std::printf("页面 Cookie 名（%zu）: %s\n", cookies.size(),
                    names.empty() ? "(无 / HttpOnly 不可见)" : names.c_str());
        const std::string keys = root.value("keys", std::string());
        std::printf("localStorage 键: %s\n", keys.empty() ? "(空)" : keys.c_str());
    }
    std::fflush(stdout); // 排障：确认已解析并回显页面信息
    const auto dump_list = [&](const char* title, const char* key, const char* field) {
        const nlohmann::json list = root.contains(key) ? root[key] : nlohmann::json::array();
        std::printf("\n--- %s（%zu 个候选）---\n", title, list.size());
        int         shown = 0;
        std::string first_visible;
        for (const nlohmann::json& d : list) {
            const std::string selector = suggest_selector_of(d);
            const bool        visible  = d.value("visible", false);
            if (!visible && shown >= 8) {
                continue; // 隐藏元素只列前 8 个（多为模板 / 预渲染节点）
            }
            ++shown;
            std::printf("  %-2d %s %s [%s]\n", shown, visible ? "[可见]" : "[隐藏]",
                        selector.empty() ? "(无可用指纹)" : selector.c_str(),
                        d.value("tag", std::string("?")).c_str());
            if (!d.value("id", std::string()).empty()) {
                std::printf("      id=%s\n", d.value("id", std::string()).c_str());
            }
            if (!d.value("cls", std::string()).empty()) {
                std::printf("      class=%s\n", d.value("cls", std::string()).c_str());
            }
            if (!d.value("ph", std::string()).empty()) {
                std::printf("      placeholder=%s\n", d.value("ph", std::string()).c_str());
            }
            if (!d.value("aria", std::string()).empty()) {
                std::printf("      aria-label=%s\n", d.value("aria", std::string()).c_str());
            }
            if (!d.value("ce", std::string()).empty()) {
                std::printf("      contenteditable=%s\n", d.value("ce", std::string()).c_str());
            }
            if (!d.value("text", std::string()).empty()) {
                std::printf("      text=%s\n", d.value("text", std::string()).c_str());
            }
            if (visible && first_visible.empty() && !selector.empty()) {
                first_visible = selector;
            }
        }
        if (!first_visible.empty()) {
            std::printf("  ⇒ 建议 %s = %s\n", field, first_visible.c_str());
        }
        else {
            std::printf("  ⇒ 建议 %s = （未发现可见候选：可能需先登录 / 页面未渲染完）\n", field);
        }
    };

    dump_list("输入框候选（input / textarea / contenteditable）", "inputs", "input_selector");
    dump_list("发送候选（button / submit / send）", "sends", "send.selector");
    dump_list("回答容器候选（markdown / message / answer）", "answers", "answer_selector");

    std::printf("\n下一步：把建议填进该条目的 web 段 → 重跑 "
                "`aiwrite.exe --web-adapter-selftest --provider %s` 确认可达\n",
                provider_id.c_str());
    std::fflush(stdout);
    return 0;
  }
  catch (const std::exception& ex) {
    std::printf("枚举异常（已兜底，不再崩溃）：%s\n", ex.what());
    std::fflush(stdout);
    return 1;
  }
  catch (...) {
    std::printf("枚举异常（未知类型，已兜底）\n");
    std::fflush(stdout);
    return 1;
  }
}

int dom_adapter_selftest(const std::string& provider_id, int timeout_ms)
{
    const ChannelSessionGuard guard;   // 起会话后**任何路径**都收尾（含提前 return）
    const ProviderSpec* spec       = provider_specs().find(provider_id);
    const std::string   site_error = web_site_error(spec);
    if (!site_error.empty()) {
        std::printf("[选择器探测] 站点不可用（条目 %s）：%s\n", provider_id.c_str(),
                    site_error.c_str());
        return 2;
    }
    if (spec->web.adapter != "dom") {
        std::printf("[选择器探测] 条目 %s 的适配器是 %s —— 本自检只诊断 adapter=dom 的条目\n",
                    provider_id.c_str(), spec->web.adapter.c_str());
        return 2;
    }

    const ProviderWebSpec   site    = strict_web_spec_for(spec);
    const std::string       site_id = strict_web_provider_id_for(spec);
    const web::LoginRequest request = web::interactive_login_request(site, site_id);

    std::printf("\n===== 选择器探测 / 诊断（只读，不发送任何内容）=====\n");
    std::printf("条目        : %s（%s）\n", spec->id.c_str(), spec->display.c_str());
    std::printf("适配器      : %s\n", site.adapter.c_str());
    std::printf("登录页      : %s\n", site.login_url.c_str());
    if (!site.window_title.empty()) {
        std::printf("窗口标题    : %s\n", site.window_title.c_str());
    }
    const std::string field_warnings = web_site_field_warnings(spec);
    if (!field_warnings.empty()) {
        std::printf("字段缺失    : %s\n", field_warnings.c_str());
    }
    if (web_login_only(spec)) {
        std::printf("⚠️ 登录型条目：缺生成字段 → **生成不可用**（本自检仍可探测选择器命中情况）\n");
    }

    // 1) 按站点准备窗口（未登录 → 在该窗口手动登录一次；不代填密码、不绕过验证）
    std::string boot_error;
    const int   boot_timeout =
        std::max(10000, std::min(timeout_ms > 0 ? timeout_ms * 1000 : 30000, 60000));
    // 会话 = **新通道**（`M7B-18` 换代）：起常驻浏览器 + `open_tab`（站点 `url`）+ `I15′` 登录态
    const bool        session_ok = web::channel::ensure_session(request, boot_timeout, &boot_error);
    const std::string site_key   = web::login_request_site(request);
    const web::Session session   = web::SessionStore::instance().snapshot(site_key);
    std::printf("站点键      : %s\n", site_key.c_str());
    std::printf("登录态      : %s（仅内存；程序退出即销毁）\n",
                session.user_token.empty() ? "未取到（请在该窗口内手动登录一次后重跑）" : "已取得");
    if (!session_ok && !boot_error.empty()) {
        std::printf("会话提示    : %s\n", boot_error.c_str());
    }

    // 2) 注入探测配置 + 执行探测脚本（只读）
    nlohmann::json probe_cfg;
    probe_cfg["input_selector"]  = site.input_selector;
    probe_cfg["send_kind"]       = site.send_kind;
    probe_cfg["send_value"]      = site.send_value;
    probe_cfg["answer_selector"] = site.answer_selector;
    probe_cfg["done_kind"]       = site.done_kind;
    probe_cfg["done_selector"]   = site.done_selector;
    probe_cfg["token_expr"]      = site.token_expr;
    probe_cfg["cookie_names"]    = site.cookie_names;

    std::string raw;
    std::string script_error;
    if (!web::channel::run_script(request,
                                  "window.__aiwriteDomProbe = " + probe_cfg.dump() + "; 'ok';",
                                  10000, &raw, &script_error)) {
        std::printf("探测失败    : %s\n", script_error.c_str());
        std::printf("（提示：窗口 / 页面未就绪，或站点拒绝脚本执行；可重跑本命令）\n");
        return 1;
    }
    if (!web::channel::run_script(request, dom_probe_script(), 15000, &raw, &script_error)) {
        std::printf("探测失败    : %s\n", script_error.c_str());
        std::printf("（提示：窗口 / 页面未就绪，或站点拒绝脚本执行；可重跑本命令）\n");
        return 1;
    }
    if (!script_error.empty()) {
        // ⚠️ **成功但截断**（> 48 KiB）：如实提示（`I21`：不假装完整）
        std::printf("警告        : %s\n", script_error.c_str());
    }
    nlohmann::json r;
    try {
        r = nlohmann::json::parse(raw);
    }
    catch (const std::exception& ex) {
        std::printf("探测结果解析失败：%s；原始：%s\n", ex.what(), raw.substr(0, 200).c_str());
        return 1;
    }

    const int         input_hits   = r.value("input_hits", 0);
    const int         send_hits    = r.value("send_hits", 0);
    const int         answer_hits  = r.value("answer_hits", 0);
    const int         answer_chars = r.value("answer_chars", 0);
    const int         token_len    = r.value("token_len", -1);
    const bool        token_json   = r.value("token_json", false);
    const std::size_t cookies_ok =
        r.value("cookies_present", std::vector<std::string>()).size();

    std::printf("\n--- 当前页面 ---\n");
    std::printf("URL         : %s\n", r.value("url", std::string("?")).c_str());
    std::printf("标题        : %s\n", r.value("title", std::string("?")).c_str());
    std::printf("input       : %s → 命中 %d 个%s\n", site.input_selector.c_str(), input_hits,
                input_hits == 0
                    ? ""
                    : (r.value("input_visible", false) ? "（可见）"
                                                       : "（不可见：可能在登录页 / 隐藏面板）"));
    std::printf("send        : kind=%s value=%s → 命中 %d\n", site.send_kind.c_str(),
                site.send_value.c_str(), send_hits);
    std::printf("answer      : %s → 命中 %d 个（末条 %d 字）\n", site.answer_selector.c_str(),
                answer_hits, answer_chars);
    if (site.done_kind.empty()) {
        std::printf("done_when   : 未配置（将用「文本稳定 %d 轮」判定）\n", kStableRounds);
    }
    else {
        std::printf("done_when   : kind=%s selector=%s（当前：%s）\n", site.done_kind.c_str(),
                    site.done_selector.c_str(),
                    r.contains("done_now") && r["done_now"].is_boolean()
                        ? (r["done_now"].get<bool>() ? "已满足" : "未满足")
                        : "未知");
    }
    if (site.token_expr.empty()) {
        std::printf("token_expr  : 未配置（该站点无法自动探测凭证；登录仍可用）\n");
    }
    else if (token_len == -2) {
        std::printf("token_expr  : %s → **求值失败**（CSP 拦截 eval 或表达式有误）\n",
                    site.token_expr.c_str());
    }
    else if (token_len < 0) {
        std::printf("token_expr  : %s → 取到 null / undefined\n", site.token_expr.c_str());
    }
    else {
        std::printf("token_expr  : %s → 长度 %d%s\n", site.token_expr.c_str(), token_len,
                    token_json ? "（**是 JSON 包裹值** → 建议改成解包表达式；与 R19 同源）" : "");
    }
    std::printf("Cookie      : 期望 %zu 个 / 可读 %zu 个（HttpOnly 不计入）\n",
                site.cookie_names.size(), cookies_ok);

    // 3) 可操作建议
    bool                     ok = true;
    std::vector<std::string> tips;
    if (input_hits == 0) {
        ok = false;
        tips.emplace_back("input_selector 未命中：在窗口里按 F12 选中输入框 → 右键 Copy → Copy "
                          "selector，填进该条目的 web.input_selector");
    }
    if (send_hits == 0) {
        ok = false;
        tips.emplace_back(std::string("send 目标未命中：") +
                          (site.send_kind == "click"
                               ? "send.selector 不存在 → 修正选择器，或改 send.kind=key（Enter）"
                               : "输入框不可交互 → 确认页面已登录、输入框可见"));
    }
    if (answer_hits == 0) {
        ok = false;
        tips.emplace_back("answer_selector 未命中：先在窗口里手动发一条消息，再选中回答容器"
                          "（如 .markdown-body）取其 CSS 选择器");
    }
    if (site.done_kind.empty()) {
        tips.emplace_back("建议配 done_when（kind=selector_gone +「停止生成」按钮选择器）：否则只能靠"
                          "「文本稳定」判定，长回答可能被判早完成");
    }
    if (token_len == -2) {
        tips.emplace_back("token_expr 求值失败：改用可在页面直接求值的表达式（避免被 CSP 拦的写法）");
    }
    if (token_json) {
        tips.emplace_back("token_expr 取到 JSON 包裹值：改成解包（如 JSON.parse(...).token）");
    }
    if (!tips.empty()) {
        std::printf("\n--- 建议（按序修；改完重启程序再跑一次本命令）---\n");
        for (const std::string& tip : tips) {
            std::printf("  · %s\n", tip.c_str());
        }
    }
    std::printf("\n[选择器探测] 结果：%s\n", ok ? "必需选择器全部命中 —— 可在界面点「运行」验证生成"
                                                : "有缺项（见上「建议」）");
    if (ok) {
        std::printf("  提示：实测可用后请把 %s 的 verified 置 true，并回填 M_patchB 附录 E\n",
                    spec->id.c_str());
    }
    return ok ? 0 : 1;
}

} // namespace aiwrite::ai


