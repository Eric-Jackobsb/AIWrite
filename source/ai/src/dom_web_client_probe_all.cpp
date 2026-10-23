// ============================================================================//  dom_web_client_probe_all.cpp — 批量探测：domProbeAll（报告 + 台账）//  * ARCH-Z1b：自 2398 行单文件 `dom_web_client.cpp` 逐字拆出，**零语义变化**//  * 模块内契约见 `dom_web_client_detail.h`；公开 API 见 `dom_web_client.h`// ============================================================================
#include "dom_web_client_detail.h"
#include "dom_web_client_scripts.h"

namespace aiwrite::ai {

int domProbeAll(int timeout_ms, nlohmann::json* report, std::vector<DomSiteProbe>* probes)
{
    // 站点集合：`kind=web 且 web.adapter=dom` —— 表驱动，**不写死站点清单**
    std::vector<const ProviderSpec*> targets;
    for (const ProviderSpec& spec : providerSpecs().items) {
        if (spec.kind == "web" && spec.web.adapter == "dom") {
            targets.push_back(&spec);
        }
    }

    std::printf("\n===== 网页版 DOM 站点批量只读探测（loop_dev 批 3 · FEA-L01）=====\n");
    std::printf("条目 %zu 个（kind=web 且 adapter=dom）· 超时 %d s/站"
                "· 只读：不写入、不发送、不读任何值\n",
                targets.size(), timeout_ms);
    if (targets.empty()) {
        std::printf("[批量探测] 表里没有可诊断的 DOM 站点（检查 providers.json / 覆盖链）\n");
        return 2;
    }

    nlohmann::json rep;
    rep["schema"]       = "aiwrite.web_adapter_report/1";
    rep["generated_at"] = probe_timestamp();
    rep["timeout_ms"]   = timeout_ms;
    rep["sites"]        = nlohmann::json::array();

    int                       usable = 0, need_login = 0, failed = 0, skipped = 0;
    std::vector<DomSiteProbe> all;
    all.reserve(targets.size());
    int index = 0;
    for (const ProviderSpec* spec : targets) {
        DomSiteProbe p;
        domSiteProbe(spec->id, timeout_ms, &p);
        // ---- 环境级失败 ⇒ **重试 1 次**（`loop_dev` §3.2 行 3 + §5.1③；§3.3：同轮同站上限 2 次）----
        //  * 只对「探测没跑通（窗口 / 页面未就绪）」与「页面异常」重试 —— 二者可能是重站首屏慢；
        //  * **不**对「需登录」重试（登录墙不会自己消失；重试只会白等一轮）
        if (p.site_error.empty() && (!p.probe_ok || p.page_abnormal)) {
            std::printf("[%2d/%2d] %s 环境级失败（%s）⇒ 按 §3.2 重试 1 次\n", index + 1,
                        static_cast<int>(targets.size()), p.id.c_str(),
                        p.page_abnormal ? "页面异常" : "探测未跑通");
            std::fflush(stdout);
            DomSiteProbe again;
            domSiteProbe(spec->id, timeout_ms, &again);
            again.attempts = 2;
            p              = again;
        }
        // ---- 批 3 串行化（2026-10-17 R2-navfix；2026-10-18 `D-35④` 修订根因）----
        //  * 全部 DOM 站共享**同一个默认 profile**（providers.json 未填 profile_dir ⇒ 空 = 默认目录）。
        //    此前「同 profile 复用同一 environment」被**别的窗口线程**跨线程使用 ⇒
        //    `CreateCoreWebView2Controller` 同步 0x802A000C（线程亲和违反，9/10 站失败的
        //    真正根因；webview_host.cpp 已改为每窗口线程自建环境）。
        //  * 这里仍**每站完全销毁本站窗口**（`close_owner_window` 等待窗口线程结束 ⇒ 控制器 Close +
        //    浏览器进程退出），再按 `kReopenSettleMs` 同口径避让后探下一站 ⇒ 全程只存活一个窗口，
        //    给下一站新环境创建留出浏览器侧释放时间（`D-35③`），同时消除 10 窗并存的资源风暴。
        web::closeOwnerWindow(web::WebOwner{}, web::siteKeyOf(spec->web.login_url));
        std::this_thread::sleep_for(std::chrono::milliseconds(kProbeSiteSettleMs));
        print_probe_block(++index, static_cast<int>(targets.size()), p);

        const std::string state = probe_state_of(p);
        if (state == "可用") {
            ++usable;
        }
        else if (state == "需登录") {
            ++need_login;
        }
        else if (state == "失败") {
            ++failed;
        }
        else {
            ++skipped;
        }

        nlohmann::json item;
        item["id"]               = p.id;
        item["display"]          = p.display;
        item["adapter"]          = p.adapter;
        item["state"]            = state;
        item["probe_ok"]         = p.probe_ok;
        item["session_ok"]       = p.session_ok;
        item["url"]              = p.url;
        item["title"]            = p.title;
        item["input_hits"]       = p.input_hits;
        item["input_visible"]    = p.input_visible;
        item["input_candidates"] = p.input_candidates;
        item["login_controls"]   = p.login_controls;
        item["login_evidence"]   = p.login_evidence;
        item["needs_login"]      = p.needs_login;
        item["page_abnormal"]    = p.page_abnormal;
        item["send_hits"]        = p.send_hits;
        item["answer_hits"]      = p.answer_hits;
        item["answer_chars"]     = p.answer_chars;
        item["token_len"]        = p.token_len;
        item["token_json"]       = p.token_json;
        item["cookies_wanted"]   = p.cookies_wanted;
        item["cookies_present"]  = p.cookies_present;
        item["error"]            = p.error;
        item["site_error"]       = p.site_error;
        item["attempts"]         = p.attempts;
        rep["sites"].push_back(item);
        all.push_back(p);
    }

    rep["counts"] = {{"total", static_cast<int>(targets.size())},
                     {"usable", usable},
                     {"needs_login", need_login},
                     {"failed", failed},
                     {"skipped", skipped}};

    std::printf("----- 汇总 -----\n");
    std::printf("可用 %d · 需登录 %d · 失败 %d · 未测 %d（共 %zu 站）\n", usable, need_login, failed,
                skipped, targets.size());
    if (need_login > 0) {
        std::printf("需人工介入（退出码 3）：上列「需登录」站点 —— 在窗口里手动登录一次后重跑本命令\n");
    }
    if (failed > 0) {
        std::printf("**真失败**（退出码 1）：上列「失败」站点 —— 待修清单，需人读原因后处置\n");
    }
    std::fflush(stdout);

    if (report != nullptr) {
        *report = rep;
    }
    if (probes != nullptr) {
        *probes = all;
    }
    if (failed > 0) {
        return 1; // 真失败优先（比「需登录」更需要人手）
    }
    return need_login > 0 ? 3 : 0;
}

bool dom_report_diff(const nlohmann::json& previous, const nlohmann::json& current,
                     std::vector<std::string>* lines)
{
    if (!previous.is_object() || !previous.contains("sites") || !previous["sites"].is_array() ||
        !current.is_object() || !current.contains("sites") || !current["sites"].is_array()) {
        return false;
    }
    // 状态口径与 `probe_state_of` 一一对应；旧版报告没有 `state` 字段时按事实字段回推
    const auto state_of = [](const nlohmann::json& item) -> std::string {
        if (item.contains("state") && item["state"].is_string()) {
            return item["state"].get<std::string>();
        }
        if (!item.value("site_error", std::string()).empty()) {
            return "未测";
        }
        if (item.contains("probe_ok") && !item.value("probe_ok", false)) {
            return "失败";
        }
        if (item.value("page_abnormal", false)) {
            return "失败";
        }
        if (item.value("needs_login", false)) {
            return "需登录";
        }
        // 与页面脚本共用同一条 §3.2 判据：既无输入区候选、也无登录迹象 ⇒ 页面异常（失败）
        if (item.value("input_hits", 0) == 0 && item.value("input_candidates", 0) == 0 &&
            item.value("login_evidence", std::string()).empty()) {
            return "失败";
        }
        return "可用";
    };

    std::map<std::string, const nlohmann::json*> before;
    for (const nlohmann::json& item : previous["sites"]) {
        if (item.is_object() && item.contains("id") && item["id"].is_string()) {
            before[item["id"].get<std::string>()] = &item;
        }
    }

    std::vector<std::string>    out;
    std::map<std::string, bool> seen;
    int                         unchanged = 0;
    for (const nlohmann::json& item : current["sites"]) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) {
            continue;
        }
        const std::string id        = item["id"].get<std::string>();
        const std::string now_state = state_of(item);
        seen[id]                    = true;
        const auto it               = before.find(id);
        if (it == before.end()) {
            out.push_back("新增      " + id + "：" + now_state + "（上一轮报告里没有这个条目）");
            continue;
        }
        const nlohmann::json& old_item      = *it->second;
        const std::string     old_state     = state_of(old_item);
        const std::string     now_ev        = item.value("login_evidence", std::string());
        const std::string     old_ev        = old_item.value("login_evidence", std::string());
        const std::string     now_url       = item.value("url", std::string());
        const std::string     old_url       = old_item.value("url", std::string());
        const bool            state_changed = (old_state != now_state);
        const bool            ev_changed    = (now_ev != old_ev);
        const bool            url_changed   = (now_url != old_url);
        if (!state_changed && !ev_changed && !url_changed) {
            ++unchanged;
            continue;
        }
        if (state_changed) {
            out.push_back("状态变化  " + id + "：" + old_state + " → " + now_state);
        }
        if (ev_changed) {
            out.push_back("判据变化  " + id + "：登录证据「" + (old_ev.empty() ? "无" : old_ev) +
                          "」→「" + (now_ev.empty() ? "无" : now_ev) + "」");
        }
        if (url_changed) {
            out.push_back("页面变化  " + id + "：" + (old_url.empty() ? "?" : old_url) + " → " +
                          (now_url.empty() ? "?" : now_url));
        }
    }
    for (const auto& kv : before) {
        if (seen.count(kv.first) == 0) {
            out.push_back("消失      " + kv.first + "：上一轮有、本轮没有（条目被删 / 改名？）");
        }
    }
    if (out.empty()) {
        out.push_back("无变化（" + std::to_string(unchanged) + " 站逐项一致）");
    }
    if (lines != nullptr) {
        *lines = out;
    }
    return true;
}


} // namespace aiwrite::ai

