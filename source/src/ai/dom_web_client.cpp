#include "ai/dom_web_client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "utils/log.h"
#include "web/webview_host.h"

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

    // ---- 1) 按站点确保窗口（未登录不阻断：DOM 站点的登录态在浏览器 profile 里；
    //         页面自己会告诉我们「输入框在不在 / 能不能写入」——由脚本诊断给出可操作原因）----
    const web::LoginRequest site_request =
        web::boot_login_request(request.site, request.provider_id);
    std::string boot_error;
    const int   boot_timeout = std::max(15000, std::min(request.timeout_ms, 60000));
    if (!web::ensure_session(site_request, boot_timeout, &boot_error)) {
        result.warning = "未取到内存凭证（该站点可能未登录，或该站点不产生 userToken）：" + boot_error;
    }

    // ---- 2) 页面内驱动：注入提示词 → 触发发送 → 轮询答案（全部走 web::run_script_sync）----
    int poll_ms   = 0;
    int max_polls = 0;
    clamp_poll_params(request.site, &poll_ms, &max_polls);

    std::string raw;
    std::string script_error;
    if (!web::run_script_sync(site_request,
                              "window.__aiwriteDom = " + dom_cfg_json(request) + "; 'ok';", 10000,
                              &raw, &script_error)) {
        result.error = "注入站点配置失败：" + script_error;
        return finish();
    }
    if (!web::run_script_sync(site_request, dom_kickoff_script(), 15000, &raw, &script_error)) {
        result.error = "站点页面交互失败：" + script_error;
        return finish();
    }
    try {
        const nlohmann::json kick       = nlohmann::json::parse(raw);
        const bool           kicked     = kick.value("ok", false);
        const int            input_hits = kick.value("input_hits", 0);
        const int            send_hits  = kick.value("send_hits", 0);
        result.steps = "input_selector 命中 " + std::to_string(input_hits) + " 个（" +
                       kick.value("input_tag", std::string("?")) + "）；send=" +
                       kick.value("send_kind", std::string("key")) + " 命中 " +
                       std::to_string(send_hits);
        if (!kicked) {
            result.error = kick.value("error", std::string("写入 / 发送失败")) +
                           "（用 --web-adapter-selftest --provider " + request.provider_id +
                           " 复核选择器）";
            return finish();
        }
    }
    catch (const std::exception& ex) {
        result.error = std::string("解析 kickoff 结果失败：") + ex.what() + "；原始返回：" +
                       raw.substr(0, 200);
        return finish();
    }

    // ---- 3) 轮询答案：done_when 命中 / 文本连续 N 轮稳定 / 到达上限（R13：如实返回 + 提示）----
    std::string last_text;
    std::string last_poll_error;
    int         stable = 0;
    for (int attempt = 1; attempt <= max_polls; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
        result.polls = attempt;
        std::string poll_raw;
        std::string poll_error;
        if (!web::run_script_sync(site_request, dom_poll_script(), 15000, &poll_raw, &poll_error)) {
            last_poll_error = poll_error; // 单次失败不致命：下一轮重试
            continue;
        }
        try {
            const nlohmann::json poll   = nlohmann::json::parse(poll_raw);
            const bool           found  = poll.value("found", false);
            const bool           is_done = poll.value("done", false);
            const std::string    text   = found ? poll.value("text", std::string()) : std::string();
            if (found && !text.empty()) {
                if (result.text.empty()) {
                    result.steps += "；answer_selector 命中 " +
                                    std::to_string(poll.value("count", 0)) + " 个";
                }
                result.text = text;
            }
            if (is_done) {
                break; // done_when 条件成立
            }
            if (!result.text.empty() && result.text == last_text) {
                if (++stable >= kStableRounds) {
                    break; // 未配 done_when：文本稳定即完成
                }
            }
            else {
                stable    = 0;
                last_text = result.text;
            }
        }
        catch (const std::exception&) {
            continue; // 页面切换 / 偶发解析失败：下一轮重试
        }
        if (std::chrono::steady_clock::now() - started >
            std::chrono::milliseconds(request.timeout_ms > 0 ? request.timeout_ms : 180000)) {
            result.warning = "达到总超时（" + std::to_string(request.timeout_ms) +
                             " ms）：已取回当前文本，可能仍在生成中";
            break;
        }
    }
    if (result.text.empty()) {
        result.error = "未取到答案文本（answer_selector 未命中或站点仍在生成）"
                       "；可用 --web-adapter-selftest --provider <id> 诊断选择器";
        if (!last_poll_error.empty()) {
            result.error += "（最后一次轮询错误：" + last_poll_error + "）";
        }
        return finish();
    }
    if (result.warning.empty() && result.polls >= max_polls) {
        result.warning = "轮询上限（" + std::to_string(max_polls) +
                         " 次 / 约 " + std::to_string((max_polls * poll_ms) / 1000) +
                         " 秒）到达：已取回文本，可能不完整";
    }
    result.ok = true;
    log::info("[DOM 适配器] 站点=" + request.site.login_url + " 轮询=" + std::to_string(result.polls) +
              " 次 / " + std::to_string(result.elapsed_ms) + " ms，文本 " +
              std::to_string(result.text.size()) + " 字节" +
              (result.warning.empty() ? "" : ("；警告：" + result.warning)) +
              (result.error.empty() ? "" : ("；错误：" + result.error)));
    return finish();
}

int dom_adapter_selftest(const std::string& provider_id, int timeout_ms)
{
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
    const bool        session_ok = web::ensure_session(request, boot_timeout, &boot_error);
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
    if (!web::run_script_sync(request, "window.__aiwriteDomProbe = " + probe_cfg.dump() + "; 'ok';",
                              10000, &raw, &script_error) ||
        !web::run_script_sync(request, dom_probe_script(), 15000, &raw, &script_error)) {
        std::printf("探测失败    : %s\n", script_error.c_str());
        std::printf("（提示：窗口 / 页面未就绪，或站点拒绝脚本执行；可重跑本命令）\n");
        return 1;
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


