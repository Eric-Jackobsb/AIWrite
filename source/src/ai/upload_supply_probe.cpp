#include "ai/upload_supply_probe.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "ai/provider_spec.h"
#include "utils/log.h"
#include "utils/paths.h"
#include "web/session_store.h"
#include "web/webview_host.h"

namespace aiwrite::ai {
namespace {

// ---------------------------------------------------------------- 页面取样脚本 --
//  * **只读**：不写入、不点击、不发送
//  * 只读 `localStorage` / `sessionStorage` 的**键名**，外加「键名或值里含 `device`」的项（值截断）
//  * `document.cookie` 只能看到**非 HttpOnly** 项；HttpOnly 由会话取样（`SessionStore`）覆盖
constexpr const char* kSupplySampleScript = R"JS(
(function () {
  const out = { readback: 'm8-supply-ok', url: location.href, title: document.title || '',
                device_hits: [], storage_keys: [], cookie_names_js: [], error: '' };
  const push = function (where, key, val) {
    out.device_hits.push({ where: where, key: key, value: String(val).slice(0, 300) });
  };
  try {
    for (let i = 0; i < localStorage.length; i++) {
      const k = localStorage.key(i) || '';
      let v = '';
      try { v = localStorage.getItem(k) || ''; } catch (e) { v = ''; }
      out.storage_keys.push(k);
      if (k.indexOf('device') >= 0 || v.indexOf('device_id') >= 0) { push('localStorage', k, v); }
    }
  } catch (e) { out.error += 'ls:' + (e && e.message ? e.message : String(e)) + ';'; }
  try {
    for (let i = 0; i < sessionStorage.length; i++) {
      const k = sessionStorage.key(i) || '';
      let v = '';
      try { v = sessionStorage.getItem(k) || ''; } catch (e) { v = ''; }
      if (k.indexOf('device') >= 0 || v.indexOf('device_id') >= 0) { push('sessionStorage', k, v); }
    }
  } catch (e) { out.error += 'ss:' + (e && e.message ? e.message : String(e)) + ';'; }
  const guessFrom = function (raw) {
    if (!raw) { return '' ; }
    try {
      const o = JSON.parse(raw);
      if (o && typeof o === 'object') {
        if (typeof o.device_id === 'string') { return o.device_id; }
        if (o.device_id && typeof o.device_id.value === 'string') { return o.device_id.value; }
        if (typeof o.deviceId === 'string') { return o.deviceId; }
      }
    } catch (e2) {}
    const m = String(raw).match(/device_id[^0-9A-Za-z]{0,4}([0-9A-Za-z_-]{6,64})/);
    return m ? m[1] : '';
  };
  out.device_id_guess = '';
  for (let i = 0; i < out.device_hits.length; i++) {
    const g = guessFrom(out.device_hits[i].value);
    if (g) { out.device_id_guess = g; break; }
  }
  try {
    (document.cookie || '').split(';').forEach(function (p) {
      const n = p.split('=')[0].trim();
      if (n) { out.cookie_names_js.push(n); }
    });
  } catch (e) { out.error += 'ck:' + (e && e.message ? e.message : String(e)) + ';'; }
  return out;
})();
)JS";

// scheme://host[:port]（无法解析 → 空串）
std::string origin_of(const std::string& url)
{
    const std::size_t pos = url.find("://");
    if (pos == std::string::npos) {
        return {};
    }
    const std::size_t slash = url.find('/', pos + 3);
    return (slash == std::string::npos) ? url : url.substr(0, slash);
}

// 站点请求头：Cookie 由会话决定；**不**放 Authorization（本站点族用 Cookie 鉴权）
httplib::Headers cookie_headers(const web::Session& session, const std::string& origin)
{
    httplib::Headers headers;
    headers.emplace("Accept", "application/json, text/plain, */*");
    headers.emplace("Origin", origin);
    headers.emplace("Referer", origin + "/");
    headers.emplace("User-Agent",
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                    "Chrome/126.0.0.0 Safari/537.36");
    if (!session.cookies.empty()) {
        std::string line;
        for (const web::Cookie& cookie : session.cookies) {
            if (!line.empty()) {
                line += "; ";
            }
            line += cookie.name + "=" + cookie.value;
        }
        headers.emplace("Cookie", line);
    }
    return headers;
}

std::string now_stamp()
{
    const std::time_t t = std::time(nullptr);
    std::tm           tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

// ExecuteScript 的「两层 JSON 字符串」解包（与 dom_selector_dump / webview_host 既有做法一致）
bool parse_two_layer(const std::string& raw, nlohmann::json* out, std::string* error)
{
    try {
        nlohmann::json parsed = nlohmann::json::parse(raw);
        if (parsed.is_string()) {
            parsed = nlohmann::json::parse(parsed.get<std::string>());
        }
        if (!parsed.is_object()) {
            *error = std::string("结果不是对象（实际类型 ") + parsed.type_name() + "）";
            return false;
        }
        *out = std::move(parsed);
        return true;
    }
    catch (const std::exception& ex) {
        *error = ex.what();
        return false;
    }
}

// 响应体里疑似「签名 / 票据」的键名（用于给出「服务端下发 or 需页面 JS」的判定）
const char* const kSignKeys[] = {"s",          "sign",      "signature",  "sig",        "token",
                                 "ticket",     "auth",      "authorization", "upload_token",
                                 "session_token", "upload_host", "store_uri", "session_key"};

} // namespace

// --------------------------------------------------------------------- 主流程 --
int upload_supply_probe(const std::string& provider_id, int timeout_s, const std::string& get_path)
{
    try { // 兜底：把异常转成可读输出（本项目既有做法：避免 std::terminate 丢掉全部输出）
        // ---- 0) 条目（**严格解析**，不回落内置默认站点 —— 决策 `D-26`）----
        std::string id = provider_id;
        if (id.empty()) {
            const std::vector<std::string> web_ids = provider_specs().ids("web");
            if (web_ids.empty()) {
                std::printf("[供料探针] 配置表里没有 kind=web 条目\n");
                return 2;
            }
            id = web_ids.front();
        }
        const ProviderSpec* spec       = provider_specs().find(id);
        const std::string   site_error = web_site_error(spec);
        if (!site_error.empty()) {
            std::printf("[供料探针] 站点不可用（条目 %s）：%s\n", id.c_str(), site_error.c_str());
            return 2;
        }
        const ProviderWebSpec site    = strict_web_spec_for(spec);
        const std::string     site_id = strict_web_provider_id_for(spec);
        const std::string     origin  = origin_of(site.login_url);
        if (origin.empty()) {
            std::printf("[供料探针] 无法从 web.login_url 解析 origin：%s\n", site.login_url.c_str());
            return 2;
        }
        const web::LoginRequest request = web::interactive_login_request(site, site_id);
        const int               wait_s  = (timeout_s >= 60) ? timeout_s : 300;
        const std::string       mode    = get_path.empty()
                                              ? std::string("**只读取样**（不碰站点接口）")
                                              : ("只读取样 + **一次站点 GET**：" + get_path);

        std::printf("\n===== M8 供料探针（Phase 2 · Gate）=====\n");
        std::printf("条目     : %s（%s）\n", spec->id.c_str(), spec->display.c_str());
        std::printf("登录页   : %s\n", site.login_url.c_str());
        std::printf("origin   : %s\n", origin.c_str());
        std::printf("模式     : %s\n", mode.c_str());
        std::printf("等待上限 : %d 秒（人工登录用；`--timeout` ≥ 60 才生效）\n", wait_s);
        std::printf("含义     : 需**人工登录**一次；若本窗口是新开的未登录页，\n");
        std::printf("           登录后请**重跑本命令**（第二次才能从会话里读到 Cookie）。\n");
        std::fflush(stdout);

        nlohmann::json report;
        report["probe"]       = "m8-supply-probe";
        report["provider_id"] = spec->id;
        report["login_url"]   = site.login_url;
        report["origin"]      = origin;
        report["get_path"]    = get_path;
        report["mode"]        = get_path.empty() ? "read_only" : "read_only+one_get";
        report["started"]     = now_stamp();

        // ---- 1) 会话：有头 WebView2 + 人工登录 ----
        const auto      t_boot     = std::chrono::steady_clock::now();
        std::string     boot_error;
        const bool      session_ok = web::ensure_session(request, wait_s * 1000, &boot_error);
        const long long boot_ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - t_boot)
                                      .count();
        const std::string site_key = web::login_request_site(request);
        const web::Session session = web::SessionStore::instance().snapshot(site_key);
        const WebSessionVerdict verdict =
            web_session_state(spec, web::web_session_evidence(session));
        const std::string boot_note = boot_error.empty() ? std::string() : ("｜" + boot_error);
        std::printf("\n[1] 会话\n");
        std::printf("  站点键   : %s\n", site_key.c_str());
        std::printf("  登录态   : %s（%s）\n", web_session_state_label(verdict.state).c_str(),
                    verdict.reason.c_str());
        std::printf("  取凭证   : %s（%lld ms）%s\n", session_ok ? "OK" : "未取到", boot_ms,
                    boot_note.c_str());
        std::printf("  Cookie   : %zu 条（值脱敏）\n", session.cookies.size());
        std::fflush(stdout);
        report["session_ok"]     = session_ok;
        report["boot_ms"]        = boot_ms;
        report["boot_error"]     = boot_error;
        report["site_key"]       = site_key;
        report["session_label"]  = web_session_state_label(verdict.state);
        report["session_reason"] = verdict.reason;
        report["cookie_count"]   = session.cookies.size();

        // ---- 2) 页面内只读取样（device_id 候选 / JS 可见 Cookie 名 / storage 键）----
        std::printf("\n[2] 页面取样（只读）\n");
        std::string    raw;
        std::string    script_error;
        nlohmann::json sample;
        bool           sample_ok = web::run_script_sync(request, std::string(kSupplySampleScript),
                                                       20000, &raw, &script_error);
        if (sample_ok) {
            sample_ok = parse_two_layer(raw, &sample, &script_error);
        }
        if (!sample_ok) {
            std::printf("  取样失败 : %s\n", script_error.c_str());
            std::printf("  （提示：窗口/页面未就绪，或站点拒绝脚本；可重跑本命令）\n");
        }
        else {
            std::printf("  URL      : %s\n", sample.value("url", std::string("?")).c_str());
            std::printf("  标题     : %s\n", sample.value("title", std::string("?")).c_str());
            const nlohmann::json& hits = sample["device_hits"];
            std::printf("  device 候选：%zu 项\n", hits.is_array() ? hits.size() : 0u);
            if (hits.is_array()) {
                for (const nlohmann::json& hit : hits) {
                    std::printf("    - %s::%s = %s\n", hit.value("where", std::string("?")).c_str(),
                                hit.value("key", std::string("?")).c_str(),
                                hit.value("value", std::string("")).c_str());
                }
            }
            const nlohmann::json& js_cookies = sample["cookie_names_js"];
            std::printf("  JS 可见 Cookie 名：%zu 个 ⇒ ",
                        js_cookies.is_array() ? js_cookies.size() : 0u);
            if (js_cookies.is_array()) {
                for (const nlohmann::json& name : js_cookies) {
                    std::printf("%s ", name.get<std::string>().c_str());
                }
            }
            std::printf("\n  localStorage 键：%zu 个\n", sample["storage_keys"].size());
            const std::string sample_err = sample.value("error", std::string(""));
            if (!sample_err.empty()) {
                std::printf("  取样告警 : %s\n", sample_err.c_str());
            }
        }
        std::fflush(stdout);
        report["sample_ok"] = sample_ok;
        report["sample"]    = sample_ok ? sample : nlohmann::json{{"error", script_error}};

        // `--get` 支持 `{device_id}` 占位符（用页面取样值替换 —— 免去人工手抄长 id）
        std::string       get_path_final  = get_path;
        const std::string device_guess    = sample_ok ? sample.value("device_id_guess", std::string()) : std::string();
        if (get_path_final.find("{device_id}") != std::string::npos) {
            if (device_guess.empty()) {
                std::printf("[供料探针] `--get` 含 {device_id} 占位符，但页面未取到 device_id ⇒ 中止（或改用显式填值）\n");
                return 1;
            }
            std::string replaced;
            std::size_t pos = 0;
            const std::string ph = "{device_id}";
            while (true) {
                const std::size_t hit = get_path_final.find(ph, pos);
                if (hit == std::string::npos) { replaced += get_path_final.substr(pos); break; }
                replaced += get_path_final.substr(pos, hit - pos) + device_guess;
                pos = hit + ph.size();
            }
            get_path_final = replaced;
            std::printf("[取样] device_id 占位符已替换（值 %zu 字符）\n", device_guess.size());
        }

        // ---- 3) 会话 Cookie 清单（名 + 脱敏值）与条目 cookie_names 命中 ----
        std::printf("\n[3] 会话 Cookie（值脱敏）\n");
        nlohmann::json cookie_list = nlohmann::json::array();
        for (const web::Cookie& cookie : session.cookies) {
            cookie_list.push_back({{"name", cookie.name},
                                   {"masked", web::mask_value(cookie.value)},
                                   {"http_only", cookie.http_only},
                                   {"session", cookie.session}});
            std::printf("  %-30s %s%s\n", cookie.name.c_str(),
                        web::mask_value(cookie.value).c_str(),
                        cookie.http_only ? "（HttpOnly）" : "");
        }
        std::vector<std::string> name_hits;
        for (const std::string& wanted : site.cookie_names) {
            for (const web::Cookie& cookie : session.cookies) {
                if (cookie.name == wanted) {
                    name_hits.push_back(wanted);
                    break;
                }
            }
        }
        std::printf("  条目 cookie_names 命中：%zu / %zu", name_hits.size(),
                    site.cookie_names.size());
        if (site.cookie_names.empty()) {
            std::printf("（条目未配 cookie_names ⇒ 判据取「该 origin 有 Cookie」）");
        }
        std::printf("\n");
        std::fflush(stdout);
        report["cookies"]          = cookie_list;
        report["cookie_names_hit"] = name_hits;

        // ---- 4) 可选：一次站点只读 GET（判定 `s=` 是否由站点接口下发）----
        bool        get_ok      = false;
        int         http_status = 0;
        long long   get_ms      = 0;
        std::string sign_hits;
        if (!get_path.empty()) {
            std::printf("\n[4] 站点 GET（只读）：%s%s\n", origin.c_str(), get_path_final.c_str());
            if (session.cookies.empty()) {
                std::printf("  **中止**：会话里没有 Cookie ⇒ 无法带鉴权发请求。\n");
                std::printf("  处置：先在窗口里登录 → **重跑本命令** → 再带 --get。\n");
                report["get"] = {{"ok", false}, {"aborted", "session_cookies_empty"}};
            }
            else {
                httplib::Client client(origin);
                client.set_connection_timeout(15, 0);
                client.set_read_timeout(60, 0);
                client.enable_server_certificate_verification(true);
                const auto             t_get   = std::chrono::steady_clock::now();
                const httplib::Headers headers = cookie_headers(session, origin);
                const auto             resp    = client.Get(get_path_final.c_str(), headers);
                get_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t_get)
                             .count();
                if (!resp) {
                    std::printf("  请求失败 : %s\n", httplib::to_string(resp.error()).c_str());
                    report["get"] = {{"ok", false},
                                     {"error", httplib::to_string(resp.error())},
                                     {"elapsed_ms", get_ms}};
                }
                else {
                    get_ok      = true;
                    http_status = resp->status;
                    const std::string body = resp->body;
                    std::printf("  HTTP %d（%lld ms，响应体 %zu 字节）\n", http_status, get_ms,
                                body.size());
                    std::printf("  响应体前 4000 字符（**含站点票据，外发前请自行脱敏**）：\n");
                    std::printf("%s\n", body.substr(0, 4000).c_str());
                    std::vector<std::string> found;
                    nlohmann::json           body_json;
                    bool                     body_is_json = false;
                    try {
                        body_json    = nlohmann::json::parse(body);
                        body_is_json = true;
                    }
                    catch (const std::exception&) {
                        body_is_json = false;
                    }
                    for (const char* key : kSignKeys) {
                        bool hit = false;
                        if (body_is_json && body_json.is_object()) {
                            if (body_json.contains(key)) {
                                hit = true;
                            }
                            else if (body_json.contains("data") && body_json["data"].is_object() &&
                                     body_json["data"].contains(key)) {
                                hit = true;
                            }
                        }
                        if (!hit && body.find(std::string("\"") + key + "\"") != std::string::npos) {
                            hit = true;
                        }
                        if (hit) {
                            found.push_back(key);
                        }
                    }
                    for (const std::string& key : found) {
                        if (!sign_hits.empty()) {
                            sign_hits += ", ";
                        }
                        sign_hits += key;
                    }
                    std::printf("\n  **签名类键名命中**：%s\n",
                                sign_hits.empty() ? "（无）" : sign_hits.c_str());
                    report["get"] = {{"ok", true},
                                     {"status", http_status},
                                     {"elapsed_ms", get_ms},
                                     {"body_len", body.size()},
                                     {"body_prefix", body.substr(0, 4000)},
                                     {"sign_key_hits", found}};
                }
            }
            std::fflush(stdout);
        }

        // ---- 5) 落盘 + 结论 ----
        paths::ensure_data_dirs();
        const std::string           out_name = "m8_supply_probe_" + now_stamp() + ".json";
        const std::filesystem::path out_path = paths::logs_dir() / out_name;
        {
            std::ofstream out(out_path, std::ios::binary);
            if (out) {
                out << report.dump(2);
            }
        }
        std::printf("\n[5] 报告：%s\n", out_path.string().c_str());
        std::printf("===== 判定 =====\n");
        if (!get_path.empty() && get_ok) {
            if (!sign_hits.empty()) {
                std::printf("  ① 站点接口**下发了**签名类字段（%s）⇒ 方案 E **无需页面 JS 签名**\n",
                            sign_hits.c_str());
                std::printf("     下一步：把该字段接入 C++ 原生上传链（`M8-13`）\n");
            }
            else {
                std::printf("  ② 该接口响应**未见**签名类字段 ⇒ 需核对端点，或改为页面内 JS 供料\n");
            }
        }
        else if (!get_path.empty()) {
            std::printf("  ③ 未能完成站点 GET（见 §4 原因）—— 供料链**未验证**\n");
        }
        else {
            std::printf("  ④ 只读取样完成：device 候选 %zu 项 / 会话 Cookie %zu 条\n",
                        sample_ok ? sample["device_hits"].size() : 0u, session.cookies.size());
            std::printf("     带 `--get \"<路径?查询串>\"` 可继续验证「站点接口是否下发签名材料」\n");
        }
        std::printf("  注：本探针**只读**；`%s` 内含站点票据，外发前请自行脱敏。\n\n", out_name.c_str());
        std::fflush(stdout);
        log::info("[供料探针] 条目=" + spec->id + " 会话=" + (session_ok ? std::string("OK") : std::string("未取到")) +
                  " Cookie=" + std::to_string(session.cookies.size()) + " 取样=" +
                  (sample_ok ? std::string("OK") : std::string("失败")) + " GET=" +
                  (get_path.empty() ? std::string("跳过") : (get_ok ? std::string("OK") : std::string("失败"))) +
                  " 报告=" + out_path.string());

        return (sample_ok || get_ok) ? 0 : 1;
    }
    catch (const std::exception& ex) {
        std::printf("[供料探针] 异常：%s\n", ex.what());
        return 1;
    }
}
} // namespace aiwrite::ai
