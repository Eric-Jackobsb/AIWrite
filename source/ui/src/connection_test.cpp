#include "connection_test.h"

#include "editor_state.h"
#include "log.h"

#include "credential.h"                   // 凭据库（与 CLI --provider-selftest 同序）
#include "deepseek_official_provider.h"   // ai：official_chat（联网 ping）
#include "dom_web_client.h"               // ai：dom_site_probe（只读探测，PB2-30②）
#include "provider_spec.h"                // ai：配置表 / 生效值解析
#include "session_store.h"                // web：站点键 / 会话快照
#include "site_ref.h"                     // web：interactive_login_request（按条目）
#include "webview_host.h"                 // web：login_window()（打开登录窗口）

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace aiwrite::ui {

namespace {

using aiwrite::ai::ProviderSpec;

// 后台诊断状态（工作线程写、UI 线程读）
struct DiagnosticState {
    std::mutex     mutex;
    bool           running      = false;
    DiagnosticKind running_kind = DiagnosticKind::None;
    std::string    provider_id;
    std::string    report;
    std::string    summary;
    std::string    title;
    int            code = -1;
    std::thread    worker; // 可 join（退出时统一收尾，避免 detached 线程在 log::shutdown 后写日志）
};

DiagnosticState& diag()
{
    static DiagnosticState instance;
    return instance;
}

std::string first_lines(const std::string& text, std::size_t limit)
{
    return text.size() <= limit ? text : (text.substr(0, limit) + "…");
}

std::string join_list(const std::vector<std::string>& items)
{
    std::string text;
    for (const std::string& item : items) {
        if (!text.empty()) {
            text += "、";
        }
        text += item;
    }
    return text.empty() ? std::string("（无）") : text;
}

// 解析 API Key：环境变量（表内 `env` 列表）→ 凭据库；顺序与 CLI `--provider-selftest` **一致**（PB-06）
std::string resolve_key(const ProviderSpec& spec, const std::string& key_ref, std::string* source)
{
    for (const std::string& env_name : spec.env_names) {
        const char* value = std::getenv(env_name.c_str());
        if (value != nullptr && *value != '\0') {
            if (source != nullptr) {
                *source = "环境变量 " + env_name;
            }
            return value;
        }
    }
    std::string       load_error;
    const std::string stored = utils::load_credential(key_ref, &load_error);
    if (!stored.empty()) {
        if (source != nullptr) {
            *source = "凭据库 " + key_ref;
        }
        return stored;
    }
    if (source != nullptr) {
        *source = load_error.empty() ? std::string("未找到") : load_error;
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
//  ① 「测试连接」（`PB2-07`）
// ---------------------------------------------------------------------------
namespace {

int run_connection_check(const std::string& provider_id, std::string* report, std::string* summary)
{
    std::ostringstream out;
    int                passed = 0;
    const int          failed = ai::provider_spec_selftest(&passed);
    out << "① 配置表离线断言：" << passed << " 通过 / " << failed << " 失败"
        << "（与 CLI `--provider-selftest` **同一实现**）";
    if (failed != 0) {
        for (const std::string& error : ai::provider_specs().report.errors) {
            out << "\n   [错误] " << error;
        }
        *summary = "配置表自检失败：" + std::to_string(failed) + " 项（详情见 Console）";
        *report  = out.str();
        return 1;
    }
    if (provider_id.empty()) {
        out << "\n② 未指定条目：离线部分已完成 —— 选中一个节点（或填好「提供商」）再点一次可测登录态 / 连通性";
        *summary = "配置表离线自检通过（未指定条目）";
        *report  = out.str();
        return 0;
    }

    const ai::ProviderSpec* spec = ai::provider_specs().find(provider_id);
    if (spec == nullptr) {
        out << "\n② 条目 " << provider_id << " 不在表里（表内 official " << ai::provider_specs().count_kind("official")
            << " 条 / web " << ai::provider_specs().count_kind("web") << " 条）—— 用「重新加载配置表」或检查 id 拼写";
        *summary = "条目 " + provider_id + " 不在配置表里";
        *report  = out.str();
        return 1;
    }

    // ---- 网页版条目：登录态 + 生效端点一致性（**不发送任何提示词**）----
    if (spec->kind == "web") {
        out << "\n② 条目 " << (spec->display.empty() ? spec->id : spec->display) << "（web · 适配器 "
            << (spec->web.adapter.empty() ? "builtin" : spec->web.adapter) << "）";
        const std::string site_error = ai::web_site_error(spec);
        if (!site_error.empty()) {
            out << "\n   ✗ 站点不可用：" << site_error << "（不会回落到内置默认站点）";
            *summary = "「" + provider_id + "」站点不可用（见 Console）";
            *report  = out.str();
            return 2;
        }
        const ai::ProviderWebSpec site     = ai::strict_web_spec_for(spec);
        const std::string         site_key = web::site_key_of(site.login_url);
        out << "\n   登录页：" << site.login_url << "（站点键 " << site_key << "）";
        out << "\n   协议探测适用：" << (ai::probe_is_applicable(site) ? "是" : "否（只读诊断站点）");
        if (!ai::web_adapter_implemented(site.adapter)) {
            out << "\n   ⚠ 适配器 " << site.adapter << " 本版本尚未实现：登录 / 诊断可用，**生成**会明确报错";
        }
        const web::Session           session = web::SessionStore::instance().snapshot(site_key);
        const ai::WebSessionVerdict  verdict =
            ai::web_session_state(spec, web::web_session_evidence(session));
        out << "\n   登录态：" << ai::web_session_state_label(verdict.state) << "（Cookie "
            << session.cookie_count() << " 条；判据：" << verdict.reason << "）";
        if (verdict.state != ai::WebSessionState::logged_in) {
            out << "\n   下一步：点「打开登录窗口（WebView2）」在该站点手动登录一次后重试"
                   "（本项**不发送任何提示词** —— 避免误触发站点调用与风控）";
            *summary = "未登录：「" + provider_id + "」—— 点「打开登录窗口」登录一次";
            *report  = out.str();
            return 2;
        }
        out << "\n③ 说明：网页版条目**不发送 ping**；真实生成请用节点运行或 `--run-selftest --web`";
        *summary = "网页版「" + provider_id + "」登录态就绪（未发送任何请求）";
        *report  = out.str();
        return 0;
    }

    // ---- API 条目：协议实现 → 生效值 → Key → 一条 ping ----
    out << "\n② 条目 " << (spec->display.empty() ? spec->id : spec->display) << "（official · protocol "
        << spec->protocol << "）";
    if (spec->protocol != "openai") {
        out << "\n   本版本已实现协议：" << join_list(ai::implemented_protocols());
        out << "\n   ⚠ protocol=" << spec->protocol << " 尚未实现 —— **不发请求**（如实报告，不假装通过）";
        *summary = "「" + provider_id + "」协议尚未实现（未发请求）";
        *report  = out.str();
        return 0;
    }

    const std::string base    = ai::resolve_api_base(*spec, std::string());
    const std::string model   = ai::resolve_model(*spec, std::string(), std::string(), false);
    const std::string key_ref = ai::resolve_key_ref(*spec, std::string());
    out << "\n   生效地址：" << (base.empty() ? "（空 —— 请用「API 地址」参数指定）" : base);
    out << "\n   生效模型：" << model << "\n   凭据引用：" << key_ref;
    if (base.empty()) {
        *summary = "「" + provider_id + "」缺少 API 地址";
        *report  = out.str();
        return 1;
    }
    std::string       key_source;
    const std::string key = resolve_key(*spec, key_ref, &key_source);
    if (key.empty()) {
        out << "\n   ✗ 未找到 API Key（环境变量 " << join_list(spec->env_names) << " / 凭据库 "
            << key_ref << "）—— 在面板「API Key」填一次即入库（PB-06）";
        *summary = "「" + provider_id + "」无 API Key（未发请求）";
        *report  = out.str();
        return 2;
    }
    out << "\n   凭据来源：" << key_source << "（脱敏：不打印 Key 本体）";

    ai::OfficialChatRequest request;
    request.api_base     = base;
    request.api_key      = key;
    request.model        = model;
    request.prompt       = "请只回复 pong";
    request.temperature  = 0.0;
    request.max_tokens   = 8;
    request.options      = ai::provider_options_from(*spec);
    out << "\n③ ping（「请只回复 pong」，消耗极小额度）…";
    const ai::OfficialChatResult result = ai::official_chat(request);
    out << "\n   HTTP " << result.http_status << " · " << static_cast<int>(result.elapsed_ms) << " ms";
    if (!result.ok) {
        out << "\n   ✗ " << first_lines(result.error, 200);
        *summary = "「" + provider_id + "」连接失败（HTTP " + std::to_string(result.http_status) + "）";
        *report  = out.str();
        return 1;
    }
    out << "\n   回复：" << first_lines(result.text, 60);
    *summary = "「" + provider_id + "」连通（HTTP " + std::to_string(result.http_status) + " · " +
               std::to_string(static_cast<int>(result.elapsed_ms)) + " ms）";
    *report  = out.str();
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
//  ② 「测试选择器」（`PB2-30②`）—— 复用只读探测 `ai::dom_site_probe()`
// ---------------------------------------------------------------------------
namespace {

int run_selector_check(const std::string& provider_id, std::string* report, std::string* summary)
{
    std::ostringstream out;
    if (provider_id.empty()) {
        out << "未指定条目：请先在参数面板选中一个节点（或填好「提供商」参数）再点本按钮";
        *summary = "测试选择器：未指定条目";
        *report  = out.str();
        return 2;
    }

    ai::DomSiteProbe probe;
    const int       code = ai::dom_site_probe(provider_id, /*timeout_ms=*/60, &probe);
    out << "条目：" << (probe.display.empty() ? provider_id : probe.display) << "（适配器 "
        << (probe.adapter.empty() ? "?" : probe.adapter) << "）";
    if (!probe.site_error.empty()) {
        out << "\n✗ " << probe.site_error;
        out << "\n（本诊断只读：不写入页面、不发送内容、不读任何值）";
        *summary = "测试选择器：「" + provider_id + "」条目不可用";
        *report  = out.str();
        return 2;
    }
    if (code != 0) {
        out << "\n✗ 探测失败：" << (probe.error.empty() ? std::string("未知原因") : probe.error);
        if (!probe.session_hint.empty()) {
            out << "\n   会话提示：" << probe.session_hint;
        }
        *summary = "测试选择器：「" + provider_id + "」探测失败（见 Console）";
        *report  = out.str();
        return 1;
    }

    out << "\n页面：" << probe.url << "\n标题：" << probe.title;
    out << "\n输入框：" << probe.input_hits << " 个（可见 " << (probe.input_visible ? "是" : "否")
        << "）· 候选池 " << probe.input_candidates;
    out << "\n发送按钮：" << probe.send_hits << " 个 · 答案容器：" << probe.answer_hits << " 个（末条 "
        << probe.answer_chars << " 字）";
    out << "\nCookie：期望 " << probe.cookies_wanted << " / 可读 " << probe.cookies_present
        << "（HttpOnly 不计入）";
    if (probe.token_len < 0) {
        out << "\ntoken_expr：未配置或未取到（该站点凭 Cookie 判登录态）";
    }
    else {
        out << "\ntoken_expr：取到长度 " << probe.token_len << (probe.token_json ? "（JSON 包裹）" : "");
    }

    if (probe.needs_login) {
        out << "\n需登录判据：" << (probe.login_evidence.empty() ? std::string("（有登录迹象）")
                                                              : probe.login_evidence);
        out << "\n⇒ **需人工登录**：在选择器「缺项」之前先登录一次（缺项属「没法测」而非「真失败」）";
        *summary = "测试选择器：「" + provider_id + "」需先手动登录";
        *report  = out.str();
        return 3;
    }
    if (probe.page_abnormal) {
        out << "\n⇒ **页面异常**：既无输入区候选也无登录迹象（加载中 / 反爬？）—— 请重跑一次";
        *summary = "测试选择器：「" + provider_id + "」页面异常";
        *report  = out.str();
        return 1;
    }

    const bool ok = probe.input_hits > 0 || probe.send_hits > 0 || probe.answer_hits > 0;
    if (ok) {
        out << "\n⇒ 选择器命中：可在界面点「运行」验证生成；实测可用后把该条目的 `verified` 置 true"
               "（并把 `answer_selector` 回填 —— 唯一人工关卡是「先有一条回答」）";
        *summary = "测试选择器：「" + provider_id + "」命中（输入 " + std::to_string(probe.input_hits) +
                   " · 发送 " + std::to_string(probe.send_hits) + " · 答案 " +
                   std::to_string(probe.answer_hits) + "）";
    }
    else {
        out << "\n⇒ 有缺项：用 `aiwrite.exe --web-dom-dump --provider " << provider_id
            << "` 取候选选择器并回填 providers.json";
        *summary = "测试选择器：「" + provider_id + "」有缺项（见 Console）";
    }
    *report = out.str();
    return ok ? 0 : 1;
}

} // namespace

// ---------------------------------------------------------------------------
//  后台执行编排（工作线程；Console 逐行 + 状态栏一行）
// ---------------------------------------------------------------------------
namespace {

void run_in_background(DiagnosticKind kind, const std::string& provider_id, const std::string& title,
                       int (*fn)(const std::string&, std::string*, std::string*))
{
    if (diag().worker.joinable()) {
        diag().worker.join(); // 上一轮已结束：回收线程对象（此时未在跑）
    }
    {
        std::lock_guard<std::mutex> lock(diag().mutex);
        if (diag().running) {
            return; // 已有一个诊断在跑（按钮侧已禁用；这里再兜一层）
        }
        diag().running      = true;
        diag().running_kind = kind;
        diag().title        = title;
    }
    diag().worker = std::thread([provider_id, title, fn] {
        std::string report;
        std::string summary;
        const int   code = fn(provider_id, &report, &summary);
        {
            std::lock_guard<std::mutex> lock(diag().mutex);
            diag().running      = false;
            diag().running_kind = DiagnosticKind::None;
            diag().provider_id  = provider_id;
            diag().report       = report;
            diag().summary      = summary;
            diag().code         = code;
        }
        std::istringstream lines(report);
        std::string        line;
        while (std::getline(lines, line)) {
            if (!line.empty()) {
                log::info("[" + title + "] " + line);
            }
        }
        log::info("[" + title + "] 结论：" + summary + "（码 " + std::to_string(code) + "）");
        editor().post_background_status(summary.empty() ? (title + "完成") : summary);
    });
}

} // namespace

DiagnosticKind diagnostic_running_kind()
{
    std::lock_guard<std::mutex> lock(diag().mutex);
    return diag().running ? diag().running_kind : DiagnosticKind::None;
}

bool diagnostic_running()
{
    std::lock_guard<std::mutex> lock(diag().mutex);
    return diag().running;
}

std::string diagnostic_report()
{
    std::lock_guard<std::mutex> lock(diag().mutex);
    return diag().report;
}

std::string diagnostic_summary()
{
    std::lock_guard<std::mutex> lock(diag().mutex);
    return diag().summary;
}

int diagnostic_code()
{
    std::lock_guard<std::mutex> lock(diag().mutex);
    return diag().code;
}

std::string diagnostic_title()
{
    std::lock_guard<std::mutex> lock(diag().mutex);
    return diag().title;
}

void request_connection_check(const std::string& provider_id)
{
    run_in_background(DiagnosticKind::Connection, provider_id, "测试连接", &run_connection_check);
}

void request_selector_check(const std::string& provider_id)
{
    run_in_background(DiagnosticKind::Selector, provider_id, "测试选择器", &run_selector_check);
}

void stop_diagnostics(int wait_ms)
{
    if (!diag().worker.joinable()) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (diagnostic_running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (diagnostic_running()) {
        log::warn("[诊断] 退出时后台诊断仍未结束（已等 " + std::to_string(wait_ms) +
                  " ms）—— 分离该线程以避免退出竞态（本轮结果不再回溯）");
        diag().worker.detach();
        return;
    }
    diag().worker.join();
}

// ---------------------------------------------------------------------------
//  ③ 「重新登录」（`PB-07`）：错误条 / 面板共用的动作 —— 打开该条目的登录窗口
//  * 严格按条目解析站点（**不回落**内置默认站点）—— `D-22②` / `I14`
//  * 只碰**界面登录窗口**这一个归属（`I24`：不抢占任何节点窗口）
// ---------------------------------------------------------------------------
void open_web_login_window(const std::string& provider_id)
{
    const std::string label      = provider_id.empty() ? std::string("(未指定条目)") : provider_id;
    const ai::ProviderSpec* spec = ai::provider_specs().find(provider_id);
    const std::string       site_error = ai::web_site_error(spec);
    if (!site_error.empty()) {
        log::warn("[重新登录] 站点不可用（条目 " + label + "）：" + site_error);
        editor().set_status("无法打开登录窗口：" + site_error);
        return;
    }

    const ai::ProviderWebSpec site     = ai::strict_web_spec_for(spec);
    const std::string         site_key = web::site_key_of(site.login_url);
    web::LoginWindow&         window   = web::login_window();
    if (window.running() && !web::window_on_site(web::ui_window_owner(), site_key)) {
        window.request_close(); // 同一归属换站点：先关旧窗（`I24`：同一个 owner 允许）
        window.join();
    }
    std::string error;
    if (!window.running()) {
        if (!window.start(web::interactive_login_request(site,
                                                         ai::strict_web_provider_id_for(spec)),
                          &error)) {
            log::warn("[重新登录] 打开登录窗口失败：" + error);
            editor().set_status("打开登录窗口失败：" + error);
            return;
        }
        log::info("[重新登录] 已打开「" + label + "」登录窗口（站点 " + site.login_url + "）");
        editor().set_status("已打开「" + label + "」登录窗口：登录完成后点「运行」重跑");
        return;
    }
    log::info("[重新登录] 「" + label + "」登录窗口已在运行（直接在其中登录即可）");
    editor().set_status("「" + label + "」登录窗口已打开（直接在其中登录）");
}

} // namespace aiwrite::ui
