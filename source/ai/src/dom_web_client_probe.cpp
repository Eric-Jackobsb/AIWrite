// ============================================================================//  dom_web_client_probe.cpp — 单站探测：selector dump + 探测脚本执行 + 结果解析//  * ARCH-Z1b：自 2398 行单文件 `dom_web_client.cpp` 逐字拆出，**零语义变化**//  * 模块内契约见 `dom_web_client_detail.h`；公开 API 见 `dom_web_client.h`// ============================================================================
#include "dom_web_client_detail.h"
#include "dom_web_client_scripts.h"
#include "dom_web_client_scripts_poll.h"

namespace aiwrite::ai {

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

int domSelectorDump(const std::string& provider_id, int timeout_ms)
{
  try { // 兜底：把任何 C++ 异常转成可读错误（否则 std::terminate → __fastfail 0xC0000409，输出全丢）
    const ProviderSpec* spec       = providerSpecs().find(provider_id);
    const std::string   site_error = webSiteError(spec);
    if (!site_error.empty()) {
        std::printf("[候选枚举] 站点不可用（条目 %s）：%s\n", provider_id.c_str(), site_error.c_str());
        return 2;
    }
    const ProviderWebSpec   site    = strictWebSpecFor(spec);
    const std::string       site_id = strictWebProviderIdFor(spec);
    const web::LoginRequest request = web::interactive_login_request(site, site_id);

    std::printf("\n===== 选择器候选枚举（只读；用于填 input_selector / send / answer_selector）=====\n");
    std::printf("条目     : %s（%s）\n", spec->id.c_str(), spec->display.c_str());
    std::printf("登录页   : %s\n", site.login_url.c_str());
    std::fflush(stdout); // 排障：崩溃前已产生的输出先落盘（__fastfail 会丢缓冲）

    std::string boot_error;
    const int   boot_timeout =
        std::max(10000, std::min(timeout_ms > 0 ? timeout_ms * 1000 : 30000, 60000));
    const bool        session_ok = web::ensureSession(request, boot_timeout, &boot_error);
    const std::string site_key   = web::login_request_site(request);
    const web::Session session   = web::SessionStore::instance().snapshot(site_key);
    const WebSessionVerdict verdict =
        webSessionState(spec, web::webSessionEvidence(session));
    std::printf("站点键   : %s\n", site_key.c_str());
    std::printf("登录态   : %s（%s）\n", webSessionStateLabel(verdict.state).c_str(),
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
    if (!web::run_script_sync(request, std::string(kDiscoverScript), 15000, &raw, &script_error)) {
        std::printf("枚举失败 : %s\n", script_error.c_str());
        std::printf("（提示：窗口 / 页面未就绪，或站点拒绝脚本执行；可重跑本命令）\n");
        return 1;
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


// 报告时间戳（"YYYY-MM-DD HH:MM:SS"，与其它落盘报告同口径）
std::string probe_timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

// 站点状态（5 态中的 4 态；「新增 / 消失」由 diff 判定）—— 判据与 §3.2 一一对应
std::string probe_state_of(const DomSiteProbe& p)
{
    if (!p.site_error.empty()) {
        return "未测"; // 条目不可用：根本没测
    }
    if (!p.probe_ok || p.page_abnormal) {
        return "失败"; // 探测没跑通，或页面既无输入区候选也无登录迹象
    }
    return p.needs_login ? "需登录" : "可用";
}

// 状态文案（人类可读，附一句原因）
std::string probe_state_text(const DomSiteProbe& p)
{
    const std::string state = probe_state_of(p);
    if (state == "未测") {
        return "未测（" + p.site_error + "）";
    }
    if (state == "失败") {
        const std::string why =
            p.page_abnormal ? std::string("页面异常：既无输入区候选也无登录迹象")
                            : (p.error.empty() ? std::string("原因不明") : p.error);
        return "失败（" + why + "）";
    }
    if (state == "需登录") {
        return "需登录（" + p.login_evidence + "）";
    }
    return "可用（匿名）";
}

// 一行判据（机器可读版进 JSON；这里给人看）：输入区候选 / 登录控件 / 登录证据
std::string probe_criteria_text(const DomSiteProbe& p)
{
    return "输入区候选 " + std::to_string(p.input_candidates) + " · 登录控件 " +
           std::to_string(p.login_controls) + " · 登录证据 " +
           (p.login_evidence.empty() ? std::string("无") : p.login_evidence);
}

// 每站紧凑块（2–4 行；命令输出即报告素材）
void print_probe_block(int index, int total, const DomSiteProbe& p)
{
    std::printf("[%2d/%2d] %s（%s）\n", index, total, p.id.c_str(), p.display.c_str());
    std::printf("         状态    : %s\n", probe_state_text(p).c_str());
    std::printf("         判据    : %s\n", probe_criteria_text(p).c_str());
    if (p.probe_ok) {
        std::printf("         页面    : %s\n", p.url.empty() ? "?" : p.url.c_str());
        std::printf("         命中    : input %d（%s）· send %d · answer %d（末条 %d 字）"
                    "· token 长度 %d · Cookie %d/%d\n",
                    p.input_hits, p.input_visible ? "可见" : "不可见", p.send_hits, p.answer_hits,
                    p.answer_chars, p.token_len, p.cookies_present, p.cookies_wanted);
    }
    else if (!p.error.empty()) {
        std::printf("         原因    : %s\n", p.error.c_str());
    }
    if (p.needs_login) {
        std::printf("         → 人工介入：在该窗口内手动登录一次，然后重跑本命令\n");
    }
    if (p.attempts > 1) {
        std::printf("         重试    : 已按 loop_dev §3.2/§5.1③ 重试 1 次（本站本轮共 %d 次尝试）\n",
                    p.attempts);
    }
    std::fflush(stdout);
}

// 单站探测的共用实现（`dom_adapter_selftest` 与批量探测都走这里跑脚本）
//  * 只读：注入配置 + 执行探测脚本；**不写入、不发送、不读任何值**
static bool run_site_probe_script(const ProviderWebSpec& site, const std::string& site_id,
                                  int timeout_ms, DomSiteProbe* out)
{
    const web::LoginRequest request = web::interactive_login_request(site, site_id);
    std::string             boot_error;
    const int               boot_timeout =
        std::max(10000, std::min(timeout_ms > 0 ? timeout_ms * 1000 : 30000, 60000));
    out->session_ok = web::ensureSession(request, boot_timeout, &boot_error);
    if (!out->session_ok) {
        out->session_hint = boot_error;
    }

    nlohmann::json probe_cfg;
    probe_cfg["input_selector"]  = site.input_selector;
    probe_cfg["send_kind"]       = site.send_kind;
    probe_cfg["send_value"]      = site.send_value;
    probe_cfg["answer_selector"] = site.answer_selector;
    probe_cfg["done_kind"]       = site.done_kind;
    probe_cfg["done_selector"]   = site.done_selector;
    probe_cfg["token_expr"]      = site.token_expr;
    probe_cfg["cookie_names"]    = site.cookie_names;
    // 输入区候选池：与运行期自动识别**同源**（`kInputPoolSelector`）—— 口径不复制
    probe_cfg["input_pool"]      = kInputPoolSelector;

    std::string raw;
    std::string script_error;
    if (!web::run_script_sync(request, "window.__aiwriteDomProbe = " + probe_cfg.dump() + "; 'ok';",
                              10000, &raw, &script_error) ||
        !web::run_script_sync(request, domProbeScript(), 15000, &raw, &script_error)) {
        out->error = "探测脚本执行失败：" + script_error;
        return false;
    }
    if (!domProbeResultFromJson(raw, out)) {
        out->error = "探测结果解析失败（前 200 字符）：" + raw.substr(0, 200);
        return false;
    }
    return true;
}

int domSiteProbe(const std::string& provider_id, int timeout_ms, DomSiteProbe* out)
{
    if (out == nullptr) {
        return 2;
    }
    *out    = DomSiteProbe();
    out->id = provider_id;

    const ProviderSpec* spec = providerSpecs().find(provider_id);
    if (spec == nullptr) {
        out->site_error = "表里没有该条目（id 未知）";
        return 2;
    }
    out->display = spec->display;
    out->adapter = spec->web.adapter;

    const std::string site_error = webSiteError(spec);
    if (!site_error.empty()) {
        out->site_error = site_error;
        return 2;
    }
    if (spec->web.adapter != "dom") {
        out->site_error = "适配器是 " + spec->web.adapter + "（本探测只诊断 adapter=dom 的条目）";
        return 2;
    }

    const ProviderWebSpec site    = strictWebSpecFor(spec);
    const std::string     site_id = strictWebProviderIdFor(spec);
    if (!run_site_probe_script(site, site_id, timeout_ms, out)) {
        return 1;
    }
    return 0;
}

bool domProbeResultFromJson(const std::string& raw, DomSiteProbe* out)
{
    if (out == nullptr) {
        return false;
    }
    nlohmann::json r;
    try {
        r = nlohmann::json::parse(raw);
    }
    catch (const std::exception&) {
        return false;
    }
    if (!r.is_object()) {
        return false;
    }
    const auto str_of = [&r](const char* key) -> std::string {
        return (r.contains(key) && r[key].is_string()) ? r[key].get<std::string>() : std::string();
    };
    // 数值缺失 → 回调默认值；**不把缺失当事实**（页面事实要么有要么没有）
    const auto num_of = [&r](const char* key, int fallback) -> int {
        return (r.contains(key) && r[key].is_number()) ? r[key].get<int>() : fallback;
    };
    const auto bool_of = [&r](const char* key, bool fallback) -> bool {
        return (r.contains(key) && r[key].is_boolean()) ? r[key].get<bool>() : fallback;
    };
    const auto size_of = [&r](const char* key) -> int {
        return (r.contains(key) && r[key].is_array()) ? static_cast<int>(r[key].size()) : 0;
    };

    DomSiteProbe p         = *out; // 保留调用方填好的入口字段（id / display / adapter）
    p.url                  = str_of("url");
    p.title                = str_of("title");
    p.input_hits           = num_of("input_hits", 0);
    p.input_visible        = bool_of("input_visible", false);
    p.send_hits            = num_of("send_hits", 0);
    p.answer_hits          = num_of("answer_hits", 0);
    p.answer_chars         = num_of("answer_chars", 0);
    p.token_len            = num_of("token_len", -1);
    p.token_json           = bool_of("token_json", false);
    p.cookies_wanted       = size_of("cookies_wanted");
    p.cookies_present      = size_of("cookies_present");
    p.input_candidates     = num_of("input_candidates", 0);
    p.login_controls       = num_of("login_controls", 0);
    p.login_evidence       = str_of("login_evidence");
    p.needs_login          = bool_of("needs_login", false);
    p.page_abnormal        = bool_of("page_abnormal", false);
    const std::string serr = str_of("error"); // 脚本自身抛错 ⇒ 事实可能不完整 ⇒ 不算探测成功
    if (!serr.empty()) {
        p.error = "页面脚本报错：" + serr;
    }
    p.probe_ok = serr.empty();
    *out       = p;
    return true;
}


} // namespace aiwrite::ai

