// ============================================================================//  dom_web_client_selftest.cpp — 自检：domAdapterSelftest + domProbeSelftest//  * ARCH-Z1b：自 2398 行单文件 `dom_web_client.cpp` 逐字拆出，**零语义变化**//  * 模块内契约见 `dom_web_client_detail.h`；公开 API 见 `dom_web_client.h`// ============================================================================
#include "dom_web_client_detail.h"
#include "dom_web_client_scripts.h"
#include "check_counter.h"  // make_selftest_check（去重专项）

namespace aiwrite::ai {

int domAdapterSelftest(const std::string& provider_id, int timeout_ms)
{
    const ProviderSpec* spec       = providerSpecs().find(provider_id);
    const std::string   site_error = webSiteError(spec);
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

    const ProviderWebSpec   site    = strictWebSpecFor(spec);
    const std::string       site_id = strictWebProviderIdFor(spec);
    const web::LoginRequest request = web::interactive_login_request(site, site_id);

    std::printf("\n===== 选择器探测 / 诊断（只读，不发送任何内容）=====\n");
    std::printf("条目        : %s（%s）\n", spec->id.c_str(), spec->display.c_str());
    std::printf("适配器      : %s\n", site.adapter.c_str());
    std::printf("登录页      : %s\n", site.login_url.c_str());
    if (!site.window_title.empty()) {
        std::printf("窗口标题    : %s\n", site.window_title.c_str());
    }
    const std::string field_warnings = webSiteFieldWarnings(spec);
    if (!field_warnings.empty()) {
        std::printf("字段缺失    : %s\n", field_warnings.c_str());
    }
    if (webLoginOnly(spec)) {
        std::printf("⚠️ 缺生成字段（选择器 / 发送 / 取答案）：**生成仍可用** —— 运行期会自动识别"
                    "（M8-37，结果只打印）；本自检仅用于只读复核命中数 + 固化建议值\n");
    }

    // 1) 按站点准备窗口（未登录 → 在该窗口手动登录一次；不代填密码、不绕过验证）
    std::string boot_error;
    const int   boot_timeout =
        std::max(10000, std::min(timeout_ms > 0 ? timeout_ms * 1000 : 30000, 60000));
    const bool        session_ok = web::ensureSession(request, boot_timeout, &boot_error);
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
    probe_cfg["input_pool"]      = kInputPoolSelector; // 候选池与运行期自动识别同源

    std::string raw;
    std::string script_error;
    if (!web::run_script_sync(request, "window.__aiwriteDomProbe = " + probe_cfg.dump() + "; 'ok';",
                              10000, &raw, &script_error) ||
        !web::run_script_sync(request, domProbeScript(), 15000, &raw, &script_error)) {
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

    // 2b) 需登录判据（loop_dev §3.2）：只用页面事实 → 「可用 / 需登录 / 页面异常」三分类
    //     单一真相源：判据在页面脚本里算（此处只搬运结果）
    DomSiteProbe facts;
    facts.id      = spec->id;
    facts.display = spec->display;
    facts.adapter = site.adapter;
    if (!domProbeResultFromJson(raw, &facts)) {
        std::printf("\n需登录判据  : **解析失败**（探测结果不是 JSON 对象）—— 归失败\n");
        return 1;
    }
    std::printf("\n需登录判据  : 输入区候选 %d · 登录控件 %d · 登录证据 %s\n",
                facts.input_candidates, facts.login_controls,
                facts.login_evidence.empty() ? "无" : facts.login_evidence.c_str());
    if (facts.needs_login) {
        std::printf("            → **需登录**（退出码 3）：在窗口里手动登录一次后重跑本命令\n");
    }
    else if (facts.page_abnormal) {
        std::printf("            → **页面异常**：既无输入区候选也无登录迹象（可能是加载中 / 反爬"
                    "拦截页）—— 归失败\n");
    }
    else {
        std::printf("            → 匿名可用（无需登录即可发送）\n");
    }

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
    // 退出码（loop_dev §6.3）：3 = 需人工介入（需登录）**优先于** 1 —— 登录墙下选择器缺项是
    // 「没法测」而不是「真失败」，两者处置不同（前者人手登录，后者改选择器）
    if (facts.needs_login) {
        std::printf("\n[选择器探测] 需人工介入：先在该窗口内登录一次，再重跑本命令（退出码 3）\n");
        return 3;
    }
    if (facts.page_abnormal) {
        std::printf("\n[选择器探测] 页面异常：既无输入区候选也无登录迹象（反爬 / 加载中？）"
                    "—— 归失败（退出码 1）\n");
        return 1;
    }
    return ok ? 0 : 1;
}

// ----------------------------------------------------------------------------
//  Loop Dev（批 3）：**离线自检** —— 报告解析 / diff / 状态映射的纯函数断言
//  * 不开窗口、不联网、不写文件（`FEA-L01` 的回归资产；真机走 `--web-adapter-selftest`）
//  * 断言口径 = `docs/loop_dev.md` §3.2（判据）+ §6.2（五态）+ §6.3（退出码）
// ----------------------------------------------------------------------------
int domProbeSelftest(int* passed_out)
{
    int passed = 0;
    int failed = 0;
    const auto check = make_selftest_check(passed, failed);  // 去重专项：见 utils/check_counter.h

    // ---- ① `dom_probe_result_from_json`：事实搬运（不重算判据）----
    const std::string full = R"({"url":"https://x/login","title":"T","input_hits":0,)"
                             R"("input_visible":false,"send_hits":0,"answer_hits":2,"answer_chars":7,)"
                             R"("token_len":64,"token_json":true,"cookies_wanted":["a","b"],)"
                             R"("cookies_present":["a"],"input_candidates":0,"login_controls":3,)"
                             R"("login_evidence":"password-input x1 + login-node x2",)"
                             R"("needs_login":true,"page_abnormal":false,"error":""})";
    DomSiteProbe p;
    p.id      = "kimi-web";
    p.display = "Kimi";
    p.adapter = "dom";
    check(domProbeResultFromJson(full, &p), "解析：合法 JSON 对象 ⇒ true");
    check(p.url == "https://x/login" && p.title == "T" && p.answer_hits == 2 &&
              p.answer_chars == 7,
          "解析：页面事实逐项搬运（url / title / answer）");
    check(p.token_len == 64 && p.token_json && p.cookies_present == 1 && p.cookies_wanted == 2,
          "解析：token / Cookie 计数搬运（长度而非值 —— 守 I15）");
    check(p.needs_login && !p.page_abnormal &&
              p.login_evidence == "password-input x1 + login-node x2" && p.login_controls == 3,
          "解析：**需登录判据来自页面**（needs_login / login_evidence 只搬运、不重算）");
    check(p.probe_ok && p.error.empty(), "解析：无 error ⇒ probe_ok（事实完整）");
    check(p.id == "kimi-web" && p.display == "Kimi" && p.adapter == "dom" && p.attempts == 1,
          "解析：入口字段（id / display / adapter）与 attempts 不被脚本结果覆盖");

    DomSiteProbe bare;
    check(domProbeResultFromJson(R"({"url":"https://y/"})", &bare) &&
              bare.input_hits == 0 && bare.input_candidates == 0 && bare.token_len == -1 &&
              !bare.needs_login && !bare.page_abnormal && bare.login_evidence.empty(),
          "解析：字段缺失 = 「无事实」（不猜、不默认成功；token_len 回 -1）");

    DomSiteProbe bad;
    bad.input_hits = 9;
    check(!domProbeResultFromJson("{not json", &bad) && bad.input_hits == 9,
          "解析：非法 JSON ⇒ false（**不改**调用方结构体）");
    check(!domProbeResultFromJson("[1,2,3]", &bad), "解析：JSON 数组（非对象）⇒ false");
    check(!domProbeResultFromJson(full, nullptr), "解析：空指针 ⇒ false（不崩）");

    DomSiteProbe err;
    check(domProbeResultFromJson(R"({"url":"https://z/","error":"boom"})", &err) &&
              !err.probe_ok && err.error.find("boom") != std::string::npos,
          "解析：脚本自身报错 ⇒ probe_ok = false（事实可能不完整）");

    // ---- ② `probe_state_of`：§6.2 五态中的 4 态（第 5 态由 diff 判「新增 / 消失」）----
    {
        DomSiteProbe ok_p;
        ok_p.probe_ok = true;
        check(probe_state_of(ok_p) == "可用", "状态：探测跑通且无登录迹象 ⇒ 可用");
        ok_p.needs_login = true;
        check(probe_state_of(ok_p) == "需登录", "状态：needs_login ⇒ 需登录（退出码 3，**不是**失败）");
        ok_p.page_abnormal = true;
        check(probe_state_of(ok_p) == "失败", "状态：页面异常 ⇒ 失败（优先级高于需登录）");
        DomSiteProbe bad_p;
        check(probe_state_of(bad_p) == "失败", "状态：探测没跑通（probe_ok = false）⇒ 失败");
        bad_p.site_error = "表里没有该条目（id 未知）";
        check(probe_state_of(bad_p) == "未测", "状态：条目不可用 ⇒ 未测（根本没测，不算失败）");
    }

    // ---- ③ `dom_report_diff`：五态 + 旧报告回推（§6.2）----
    const auto site = [](const std::string& id, const std::string& state, bool ok = true,
                         bool need = false, bool abnormal = false,
                         const std::string& ev = "", const std::string& url = "",
                         int hits = 1, int cands = 1) {
        nlohmann::json item;
        item["id"]               = id;
        item["state"]            = state;
        item["probe_ok"]         = ok;
        item["needs_login"]      = need;
        item["page_abnormal"]    = abnormal;
        item["login_evidence"]   = ev;
        item["url"]              = url;
        item["input_hits"]       = hits;
        item["input_candidates"] = cands;
        return item;
    };
    const auto report_of = [](const std::vector<nlohmann::json>& sites) {
        nlohmann::json doc;
        doc["schema"] = "aiwrite.web_adapter_report/1";
        doc["sites"]  = nlohmann::json::array();
        for (const nlohmann::json& s : sites) {
            doc["sites"].push_back(s);
        }
        return doc;
    };
    const auto join = [](const std::vector<std::string>& lines) {
        std::string text;
        for (const std::string& line : lines) {
            text += line + "\n";
        }
        return text;
    };

    {
        const nlohmann::json prev = report_of({site("a", "可用", true, false, false, "", "u1"),
                                               site("b", "需登录", true, true, false, "login-node x1",
                                                    "u2")});
        std::vector<std::string> lines;
        check(dom_report_diff(prev, prev, &lines) && lines.size() == 1 &&
                  lines[0].find("无变化") != std::string::npos,
              "diff：两轮逐项一致 ⇒ 「无变化（2 站逐项一致）」");
        check(!dom_report_diff(nlohmann::json::object(), prev, &lines),
              "diff：上一轮缺 sites ⇒ false（如实报错，**不**假装无变化）");
        check(!dom_report_diff(nlohmann::json("[1]"), prev, &lines), "diff：非对象报告 ⇒ false");
    }
    {
        const nlohmann::json prev =
            report_of({site("gone", "可用", true, false, false, "", "u1"),
                       site("stay", "可用", true, false, false, "", "u1"),
                       site("shift", "需登录", true, true, false, "login-node x1", "u2")});
        const nlohmann::json now =
            report_of({site("stay", "可用", true, false, false, "", "u1"),
                       site("shift", "可用", true, false, false, "", "u3", 1, 2),
                       site("fresh", "失败", false)});
        std::vector<std::string> lines;
        const bool               ok = dom_report_diff(prev, now, &lines);
        const std::string        text = join(lines);
        check(ok && text.find("新增      fresh：失败") != std::string::npos,
              "diff：本轮新增条目 ⇒ 「新增」（含其当前状态）");
        check(text.find("消失      gone") != std::string::npos,
              "diff：上一轮有、本轮没有 ⇒ 「消失」（条目被删 / 改名）");
        check(text.find("状态变化  shift：需登录 → 可用") != std::string::npos,
              "diff：状态变化逐行给出「旧 → 新」");
        check(text.find("页面变化  shift：u2 → u3") != std::string::npos,
              "diff：页面 URL 变化 ⇒ 单独一行（不掩在状态里）");
        check(text.find("stay") == std::string::npos, "diff：逐项一致的站**不**出现在输出里");
    }
    {
        // 旧版报告（无 `state`）⇒ 按事实字段回推 §3.2 判据（含「候选 0 且无登录迹象 ⇒ 失败」）
        nlohmann::json old_abnormal = site("legacy1", "", true, false, false, "", "u", 0, 0);
        old_abnormal.erase("state");
        nlohmann::json old_untested = site("legacy2", "", false);
        old_untested.erase("state");
        old_untested["site_error"] = "表里没有该条目（id 未知）";
        nlohmann::json old_need = site("legacy3", "", true, true, false, "login-url:/login");
        old_need.erase("state");
        const nlohmann::json prev = report_of({old_abnormal, old_untested, old_need});
        const nlohmann::json now  = report_of({site("legacy1", "可用"),
                                               site("legacy2", "可用"),
                                               site("legacy3", "可用")});
        std::vector<std::string> lines;
        check(dom_report_diff(prev, now, &lines) &&
                  join(lines).find("状态变化  legacy1：失败 → 可用") != std::string::npos,
              "diff：旧报告无 state ⇒ 回推「候选 0 + 无 Login 迹象 = 失败」（§3.2 同一判据）");
        check(join(lines).find("状态变化  legacy2：未测 → 可用") != std::string::npos,
              "diff：旧报告 site_error ⇒ 回推「未测」");
        check(join(lines).find("状态变化  legacy3：需登录 → 可用") != std::string::npos,
              "diff：旧报告有登录证据 ⇒ 回推「需登录」");
    }

    if (passed_out != nullptr) {
        *passed_out = passed;
    }
    std::printf("  → DOM 探测报告自检：%d 通过 / %d 失败\n", passed, failed);
    return failed;
}


} // namespace aiwrite::ai

