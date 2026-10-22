#include "probe_verdict.h"

#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace aiwrite::web {

namespace {

bool is_digit(char ch) { return ch >= '0' && ch <= '9'; }

bool is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string trim_copy(const std::string& text)
{
    std::size_t begin = 0;
    std::size_t end   = text.size();
    while (begin < end && is_space(text[begin])) {
        ++begin;
    }
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// 文本里找**独立**整数（前后都不接数字）—— 避免 `40002` 被当成 `400` / `4000`
bool find_int_token(const std::string& text, int value)
{
    const std::string digits = std::to_string(value);
    std::size_t       pos    = text.find(digits);
    while (pos != std::string::npos) {
        const bool        left_ok = (pos == 0) || !is_digit(text[pos - 1]);
        const std::size_t after   = pos + digits.size();
        const bool        right_ok = (after >= text.size()) || !is_digit(text[after]);
        if (left_ok && right_ok) {
            return true;
        }
        pos = text.find(digits, pos + 1);
    }
    return false;
}

// 响应体里取 `code` / `biz_code` 的整数（容错：`"code":0` / `code=0` / `"biz_code":40003`）
bool extract_code_field(const std::string& body, int* out)
{
    if (out == nullptr) {
        return false;
    }
    const std::string key = "code";
    std::size_t       pos = body.find(key);
    while (pos != std::string::npos) {
        std::size_t i = pos + key.size();
        // 键名闭合引号 / 空白都跳过（`biz_code` 命中的就是键名里的 `code`）
        while (i < body.size() && (is_space(body[i]) || body[i] == '"' || body[i] == '\'')) {
            ++i;
        }
        if (i < body.size() && (body[i] == ':' || body[i] == '=')) {
            ++i;
            while (i < body.size() && (is_space(body[i]) || body[i] == '"' || body[i] == '\'')) {
                ++i;
            }
            std::size_t begin = i;
            if (i < body.size() && (body[i] == '-' || body[i] == '+')) {
                ++i;
            }
            while (i < body.size() && is_digit(body[i])) {
                ++i;
            }
            if (i > begin) {
                try {
                    *out = std::stoi(body.substr(begin, i - begin));
                    return true;
                }
                catch (const std::exception&) {
                    return false;
                }
            }
        }
        pos = body.find(key, pos + 1);
    }
    return false;
}

} // namespace

std::vector<EndpointProbeLine> parseEndpointsReport(const std::string& report)
{
    std::vector<EndpointProbeLine> lines;
    const std::string              separator = " || ";
    std::size_t                    begin     = 0;
    while (begin <= report.size()) {
        const std::size_t end = report.find(separator, begin);
        const std::string raw = trim_copy(
            report.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        begin = (end == std::string::npos) ? report.size() + 1 : end + separator.size();
        if (raw.empty()) {
            continue;
        }

        EndpointProbeLine line;
        const std::size_t arrow = raw.find(" -> ");
        if (arrow == std::string::npos) {
            line.path        = raw;            // 畸形行：整行留作路径（诊断可见，不假装成功）
            line.status_text = "(无状态段)";
            lines.push_back(line);
            continue;
        }
        line.path        = trim_copy(raw.substr(0, arrow));
        line.status_text = trim_copy(raw.substr(arrow + 4));

        if (!line.status_text.empty() && is_digit(line.status_text[0])) {
            std::size_t i = 0;
            while (i < line.status_text.size() && is_digit(line.status_text[i])) {
                ++i;
            }
            try {
                line.status = std::stoi(line.status_text.substr(0, i));
            }
            catch (const std::exception&) {
                line.status = 0;
            }
            line.body = trim_copy(line.status_text.substr(i));
        }
        else if (line.status_text.rfind("error", 0) == 0) {
            line.transport_error = true;
            line.body            = trim_copy(line.status_text.substr(5));
        }
        line.has_code = extract_code_field(line.body, &line.code);

        lines.push_back(std::move(line));
    }
    return lines;
}

const EndpointProbeLine* find_endpoint(const std::vector<EndpointProbeLine>& lines,
                                       const std::string& path)
{
    if (path.empty()) {
        return nullptr;
    }
    const std::string wanted = trim_copy(path);
    for (const EndpointProbeLine& line : lines) {
        if (line.path == wanted) {
            return &line;
        }
    }
    // 容错：忽略末尾 `/`
    for (const EndpointProbeLine& line : lines) {
        std::string got = line.path;
        if (!got.empty() && got.back() == '/') {
            got.pop_back();
        }
        std::string ask = wanted;
        if (!ask.empty() && ask.back() == '/') {
            ask.pop_back();
        }
        if (!got.empty() && got == ask) {
            return &line;
        }
    }
    return nullptr;
}

std::string probeVerdictName(ProbeVerdict verdict)
{
    switch (verdict) {
    case ProbeVerdict::Pass:            return "通过";
    case ProbeVerdict::NotLoggedIn:     return "未登录";
    case ProbeVerdict::SessionInvalid:  return "会话已失效";
    case ProbeVerdict::RequestRejected: return "请求被拒";
    case ProbeVerdict::EndpointUnknown: return "端点结论未确认";
    case ProbeVerdict::ScriptFailed:    return "探测脚本失败";
    case ProbeVerdict::NotApplicable:   return "不适用协议探测";
    }
    return "?";
}

ProbeJudgement judge_probe_result(const ProbeResult& probe, const std::string& required_path,
                                  bool probe_applicable)
{
    ProbeJudgement out;

    if (!probe_applicable) {
        out.verdict   = ProbeVerdict::NotApplicable;
        out.reason    = "该站点不适用协议探测（DOM 站点只跑只读诊断）：登录态用**站点无关**判据（`I15`）";
        out.next_step = "下一步：看面板「网页版会话」区的登录态；页面事实用 `--web-dom-dump`"
                        " 或面板「测试选择器（只读诊断）」";
        return out;
    }

    const std::string path = required_path.empty() ? std::string("/api/v0/users/current")
                                                   : required_path;
    const std::vector<EndpointProbeLine> lines    = parseEndpointsReport(probe.endpoints_report);
    const EndpointProbeLine*             required = find_endpoint(lines, path);

    const bool http_401 = (required != nullptr && required->status == 401);
    const bool http_403 = (required != nullptr && required->status == 403);
    const bool code_40002 =
        (required != nullptr && required->has_code && required->code == 40002);
    const bool code_40003 =
        (required != nullptr && required->has_code && required->code == 40003);

    const std::string observed =
        (required == nullptr)
            ? std::string("报告里没有端点 ") + path
            : (required->transport_error
                   ? (std::string("端点 ") + path + " 请求失败（" + required->body + "）")
                   : (std::string("端点 ") + path + " → HTTP " +
                      std::to_string(required->status) +
                      (required->has_code ? ("，code=" + std::to_string(required->code))
                                          : std::string("，响应体无 code 字段"))));

    if (http_401 || code_40002) {
        out.verdict        = ProbeVerdict::SessionInvalid;
        out.session_related = true;
        out.needs_relogin  = true;
        out.reason         = "**会话已失效**（" + observed + "）—— 凭证过期或被站点拒绝";
        out.next_step      = "下一步：点参数面板「打开登录窗口（WebView2）」重新登录该站点后重跑 "
                             "`--web-probe`（界面错误条亦给出「重新登录」入口）";
        return out;
    }
    if (http_403 || code_40003) {
        out.verdict        = ProbeVerdict::RequestRejected;
        out.session_related = true;
        out.reason         = "**请求被站点拒绝**（" + observed + "）—— 常见于风控 / 人机验证 / PoW / 前端版本变化";
        out.next_step      = "下一步：先在窗口内完成人机验证或重新登录；若持续出现，用 `--web-dom-dump`"
                             " 复核站点选择器，并登记站点版本变化（`R19` / `R20`）";
        return out;
    }
    if (!probe.ok) {
        out.verdict   = ProbeVerdict::ScriptFailed;
        out.reason    = "探测脚本本身失败（页面没加载 / WebView2 异常）" +
                        (probe.error.empty() ? std::string() : (std::string("：") + probe.error));
        out.next_step = "下一步：重跑一次；仍失败请看 Console 与 `app.log`（本判据不看凭证）";
        return out;
    }
    if (!probe.has_user_token) {
        out.verdict        = ProbeVerdict::NotLoggedIn;
        out.session_related = true;
        out.reason         = "脚本跑通但**未取到凭证**（站点未登录 / 凭证形状变化）";
        out.next_step      = "下一步：点「打开登录窗口（WebView2）」登录一次，或跑 `aiwrite.exe --login-selftest`";
        return out;
    }
    if (required == nullptr) {
        out.verdict   = ProbeVerdict::EndpointUnknown;
        out.reason    = "有凭证，但端点探测报告里**没有**判据要求的端点（站点端点形状可能已变）：" +
                        observed;
        out.next_step = "下一步：用 `aiwrite.exe --web-dom-dump --provider <id>` 复核，并核对 providers.json"
                        " 的 `web.probe_paths` / `web.endpoints`";
        return out;
    }
    if (required->transport_error) {
        out.verdict   = ProbeVerdict::EndpointUnknown;
        out.reason    = "有凭证，但端点请求本身失败：" + observed;
        out.next_step = "下一步：确认网络 / 代理；页面内 fetch 失败也可能是站点改了端点";
        return out;
    }
    if (required->status == 200 && required->has_code && required->code == 0) {
        out.verdict   = ProbeVerdict::Pass;
        out.pass      = true;
        out.reason    = "脚本跑通 + 有凭证 + 端点 `200` 且 `code=0`：" + observed;
        out.next_step = "下一步：可直接运行网页版节点（`--web-chat` / `--run-selftest --web`）";
        return out;
    }

    out.verdict   = ProbeVerdict::EndpointUnknown;
    out.reason    = "有凭证，但**端点结论不是 `200` + `code=0`** ⇒ 不再判 PASS：" + observed;
    out.next_step = "下一步：按观测到的状态码处理（401/403 → 重新登录或过验证；其它码 → 登记站点变化）";
    return out;
}

SessionFailure classifySessionFailureText(const std::string& text)
{
    SessionFailure out;
    if (text.empty()) {
        return out;
    }

    // ---- HTTP 状态码：先看显式写法 `HTTP 401`，再看**独立**数字 ----
    for (const int status : {401, 403}) {
        if (text.find("HTTP " + std::to_string(status)) != std::string::npos) {
            out.http_status = status;
            break;
        }
    }
    if (out.http_status == 0) {
        for (const int status : {401, 403}) {
            if (find_int_token(text, status)) {
                out.http_status = status;
                break;
            }
        }
    }

    // ---- 业务码（容错：`code=40002` / `"code": 40002` / `biz_code=40003`）----
    for (const int code : {40002, 40003}) {
        if (find_int_token(text, code)) {
            out.biz_code = code;
            break;
        }
    }

    const bool phrase_invalid = text.find("会话已失效") != std::string::npos;
    const bool phrase_relogin = text.find("重新登录") != std::string::npos;
    const bool phrase_login   = text.find("未登录") != std::string::npos;
    const bool phrase_auth    = text.find("invalid token") != std::string::npos ||
                                text.find("invalid_token") != std::string::npos ||
                                text.find("unauthorized") != std::string::npos;

    out.needs_relogin   = (out.http_status == 401) || (out.biz_code == 40002) || phrase_invalid;
    out.session_related = out.needs_relogin || (out.http_status == 403) ||
                          (out.biz_code == 40003) || phrase_relogin || phrase_login || phrase_auth;

    std::string evidence;
    if (out.http_status != 0) {
        evidence = "HTTP " + std::to_string(out.http_status);
    }
    if (out.biz_code != 0) {
        if (!evidence.empty()) {
            evidence += " · ";
        }
        evidence += "code=" + std::to_string(out.biz_code);
    }
    if (phrase_invalid) {
        if (!evidence.empty()) {
            evidence += " · ";
        }
        evidence += "含「会话已失效」";
    }
    if (evidence.empty()) {
        if (phrase_relogin) {
            evidence = "含「重新登录」提示";
        }
        else if (phrase_login) {
            evidence = "含「未登录」提示";
        }
        else if (phrase_auth) {
            evidence = "鉴权失败特征串（invalid token / unauthorized）";
        }
    }
    out.evidence = evidence;
    return out;
}

std::string sessionReloginText(const SessionFailure& failure, const std::string& provider_id)
{
    const std::string site =
        provider_id.empty() ? std::string("该站点") : ("「" + provider_id + "」");
    const std::string why =
        failure.evidence.empty() ? std::string("见 Console") : failure.evidence;
    if (failure.needs_relogin) {
        return "会话已失效：" + site + "的登录态已过期或被拒绝（判据 " + why +
               "）—— 点本按钮重新登录后重跑";
    }
    if (failure.session_related) {
        return "站点拒绝请求：" + site + "可能需要人机验证 / 重新登录（判据 " + why +
               "）—— 点本按钮打开登录窗口处理";
    }
    return {};
}

// ---------------------------------------------------------------------------
//  离线自检（`api_probe --exec-selftest`）
// ---------------------------------------------------------------------------
int probeVerdictSelftest(int* passed_out)
{
    int passed = 0;
    int failed = 0;
    const auto check = [&passed, &failed](bool ok, const std::string& what) {
        if (ok) {
            ++passed;
            std::printf("  ✓ %s\n", what.c_str());
        }
        else {
            ++failed;
            std::printf("  ✗ %s\n", what.c_str());
        }
    };

    // ---- ① 端点报告解析 ----
    const std::string report_ok =
        "/api/v0/users/current -> 200 {\"code\":0,\"msg\":\"\"} || "
        "/api/v0/chat_session/fetch_page -> 200 {\"code\":0}";
    const std::vector<EndpointProbeLine> lines_ok = parseEndpointsReport(report_ok);
    check(lines_ok.size() == 2, "解析：两条端点报告 ⇒ 2 行");
    check(!lines_ok.empty() && lines_ok[0].path == "/api/v0/users/current" &&
              lines_ok[0].status == 200 && lines_ok[0].has_code && lines_ok[0].code == 0,
          "解析：路径 / HTTP 状态 / code 逐项取出");
    check(!lines_ok.empty() && !lines_ok[0].transport_error, "解析：正常行不带传输失败标记");

    const std::vector<EndpointProbeLine> lines_err =
        parseEndpointsReport("/api/v0/users/current -> error TypeError: Failed to fetch");
    check(lines_err.size() == 1 && lines_err[0].transport_error && lines_err[0].status == 0,
          "解析：`-> error …` ⇒ 传输失败（status 0）");

    const std::vector<EndpointProbeLine> lines_junk = parseEndpointsReport("just-some-text");
    check(lines_junk.size() == 1 && lines_junk[0].status_text == "(无状态段)",
          "解析：畸形行保路径、不假装成功");

    check(parseEndpointsReport("").empty(), "解析：空报告 ⇒ 0 行");
    check(parseEndpointsReport("   ").empty(), "解析：纯空白报告 ⇒ 0 行");

    const std::vector<EndpointProbeLine> lines_biz =
        parseEndpointsReport("/api/v0/users/current -> 200 {\"biz_code\":40003}");
    check(lines_biz.size() == 1 && lines_biz[0].has_code && lines_biz[0].code == 40003,
          "解析：`biz_code` 容错取码（40003）");

    const std::vector<EndpointProbeLine> lines_nocode =
        parseEndpointsReport("/api/v0/users/current -> 200 {\"ok\":true}");
    check(lines_nocode.size() == 1 && !lines_nocode[0].has_code,
          "解析：响应体无 code 字段 ⇒ has_code=false（不默认 0）");

    check(find_endpoint(lines_ok, "/api/v0/users/current") != nullptr &&
              find_endpoint(lines_ok, "/api/v0/chat_session/fetch_page/") != nullptr &&
              find_endpoint(lines_ok, "/api/v0/absent") == nullptr,
          "查询：精确命中 + 尾斜杠容错 + 缺失返回空");

    // ---- ② 判据（`PB2-25`：不再「有凭证就算 PASS」）----
    ProbeResult probe;
    probe.ok               = true;
    probe.has_user_token   = true;
    probe.endpoints_report = report_ok;
    const ProbeJudgement pass_case = judge_probe_result(probe, "", true);
    check(pass_case.verdict == ProbeVerdict::Pass && pass_case.pass && !pass_case.needs_relogin &&
              !pass_case.session_related,
          "判据：ok + 凭证 + 200/code=0 ⇒ PASS");
    check(pass_case.reason.find("code=0") != std::string::npos && !pass_case.next_step.empty(),
          "判据：PASS 也给出判据与下一步（可核对、可操作）");

    ProbeResult probe_40003 = probe;
    probe_40003.endpoints_report =
        "/api/v0/users/current -> 200 {\"code\":40003,\"msg\":\"rejected\"}";
    const ProbeJudgement rejected = judge_probe_result(probe_40003, "", true);
    check(!rejected.pass && rejected.verdict == ProbeVerdict::RequestRejected &&
              rejected.session_related && !rejected.needs_relogin,
          "判据：端点 40003 **不再 PASS**（旧判据会假 PASS）⇒ 请求被拒且不误判需重登");

    ProbeResult probe_40002 = probe;
    probe_40002.endpoints_report = "/api/v0/users/current -> 200 {\"code\": 40002}";
    const ProbeJudgement invalid = judge_probe_result(probe_40002, "", true);
    check(!invalid.pass && invalid.verdict == ProbeVerdict::SessionInvalid &&
              invalid.needs_relogin && invalid.session_related,
          "判据：code=40002 ⇒ 会话已失效（需重新登录）");

    ProbeResult probe_401 = probe;
    probe_401.endpoints_report = "/api/v0/users/current -> 401 {\"msg\":\"unauthorized\"}";
    check(judge_probe_result(probe_401, "", true).needs_relogin,
          "判据：端点 HTTP 401 ⇒ 需重新登录");

    ProbeResult probe_403 = probe;
    probe_403.endpoints_report = "/api/v0/users/current -> 403 {}";
    const ProbeJudgement denied = judge_probe_result(probe_403, "", true);
    check(denied.verdict == ProbeVerdict::RequestRejected && !denied.needs_relogin &&
              denied.session_related,
          "判据：端点 HTTP 403 ⇒ 请求被拒（风控/验证，不擅自作废会话）");

    ProbeResult probe_nocode = probe;
    probe_nocode.endpoints_report = "/api/v0/users/current -> 200 {\"ok\":true}";
    const ProbeJudgement unknown = judge_probe_result(probe_nocode, "", true);
    check(!unknown.pass && unknown.verdict == ProbeVerdict::EndpointUnknown,
          "判据：200 但无 code ⇒ 结论未确认（不 PASS）");

    ProbeResult probe_missing_path = probe;
    probe_missing_path.endpoints_report = "/api/v0/chat_session/fetch_page -> 200 {\"code\":0}";
    check(!judge_probe_result(probe_missing_path, "", true).pass,
          "判据：报告缺**指定**端点 ⇒ 不 PASS（端点形状变化可见）");
    check(judge_probe_result(probe_missing_path, "/api/v0/chat_session/fetch_page", true).pass,
          "判据：required_path 可按调用方指定（换端点即换判据）");

    ProbeResult probe_fail;
    probe_fail.error              = "探测超时";
    const ProbeJudgement s_failed = judge_probe_result(probe_fail, "", true);
    check(s_failed.verdict == ProbeVerdict::ScriptFailed && !s_failed.pass &&
              s_failed.reason.find("探测超时") != std::string::npos,
          "判据：脚本失败 ⇒ ScriptFailed（原因带上原文）");

    ProbeResult probe_anon;
    probe_anon.ok               = true;
    probe_anon.endpoints_report = "/api/v0/users/current -> 401 {}";
    const ProbeJudgement anon = judge_probe_result(probe_anon, "", true);
    check(anon.verdict == ProbeVerdict::SessionInvalid && anon.needs_relogin,
          "判据：无凭证 + 端点 401 ⇒ 会话已失效（而非笼统「未登录」）");

    ProbeResult probe_no_token;
    probe_no_token.ok = true;
    const ProbeJudgement no_token = judge_probe_result(probe_no_token, "", true);
    check(no_token.verdict == ProbeVerdict::NotLoggedIn && !no_token.pass &&
              no_token.session_related,
          "判据：无凭证且无端点证据 ⇒ 未登录（给登录引导）");

    const ProbeJudgement not_applicable =
        judge_probe_result(probe, "/api/v0/users/current", /*probe_applicable=*/false);
    check(not_applicable.verdict == ProbeVerdict::NotApplicable && !not_applicable.pass &&
              !not_applicable.needs_relogin,
          "判据：不适用协议探测的站点 ⇒ NotApplicable（不越权判登录态）");

    ProbeResult probe_transport = probe;
    probe_transport.endpoints_report = "/api/v0/users/current -> error TypeError: fetch failed";
    check(judge_probe_result(probe_transport, "", true).verdict == ProbeVerdict::EndpointUnknown,
          "判据：端点传输失败 ⇒ 结论未确认");

    check(probeVerdictName(ProbeVerdict::Pass) !=
                  probeVerdictName(ProbeVerdict::SessionInvalid) &&
              probeVerdictName(ProbeVerdict::RequestRejected) !=
                  probeVerdictName(ProbeVerdict::SessionInvalid) &&
              probeVerdictName(ProbeVerdict::NotLoggedIn) !=
                  probeVerdictName(ProbeVerdict::SessionInvalid),
          "文案：四类会话结论**互不混写**（可分辨）");

    // ---- ③ 错误串反查分类（`PB-07`：界面「重新登录」入口的地基）----
    const SessionFailure f_401 = classifySessionFailureText("HTTP 401：unauthorized");
    check(f_401.needs_relogin && f_401.http_status == 401 && f_401.session_related,
          "分类：HTTP 401 ⇒ 需重新登录（证据只留码，守 R20）");

    const SessionFailure f_40002 =
        classifySessionFailureText("HTTP 200 但解析失败；会话已失效（code 40002）");
    check(f_40002.needs_relogin && f_40002.biz_code == 40002 &&
              f_40002.evidence.find("40002") != std::string::npos,
          "分类：code 40002 ⇒ 需重新登录");

    const SessionFailure f_biz = classifySessionFailureText("{\"biz_code\":40003,\"msg\":\"rejected\"}");
    check(f_biz.session_related && f_biz.biz_code == 40003 && !f_biz.needs_relogin,
          "分类：biz_code 40003 ⇒ 与会话有关但**不**擅自作废（请求被拒）");

    const SessionFailure f_phrase =
        classifySessionFailureText("会话已失效：登录态已过期或被站点拒绝");
    check(f_phrase.needs_relogin && f_phrase.evidence.find("会话已失效") != std::string::npos,
          "分类：中文短语「会话已失效」⇒ 需重新登录");

    const SessionFailure f_timeout =
        classifySessionFailureText("HTTP 200 但未解析出文本（请查看 raw_head 原始行）");
    check(!f_timeout.session_related && !f_timeout.needs_relogin,
          "分类：普通解析 / 超时错误 ⇒ **不误报**会话失效");

    const SessionFailure f_token =
        classifySessionFailureText("challenge 解析失败: invalid token");
    check(f_token.session_related && !f_token.needs_relogin,
          "分类：`invalid token` ⇒ 与会话有关（不擅自判必须重登）");

    const SessionFailure f_boundary = classifySessionFailureText("HTTP 40010：参数错误");
    check(f_boundary.http_status == 0 && !f_boundary.session_related,
          "分类：`40010` 不被误判成 401（独立数字边界）");

    const std::string relogin_text = sessionReloginText(f_401, "deepseek-web");
    check(relogin_text.find("重新登录") != std::string::npos &&
              relogin_text.find("deepseek-web") != std::string::npos,
          "文案：重登提示含站点条目 id 与可操作动作");
    check(sessionReloginText(f_timeout, "kimi-web").empty(),
          "文案：与会话无关的失败 ⇒ **不生成**重登提示");

    if (passed_out != nullptr) {
        *passed_out = passed;
    }
    return failed;
}

} // namespace aiwrite::web
