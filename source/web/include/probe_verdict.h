#pragma once

// ============================================================================
//  网页版**会话诊断**（M_patchAB_rest §2.2 `PB-07` / §3.3 `PB2-25`）—— **纯函数**
//
//  两个问题、两个纯函数：
//   ① 判据偏松（`PB2-25`）：`--web-probe` 原来只判 `probe.ok && has_user_token`，
//      于是**端点 40003（请求被拒）时仍打印 PASS**。现在判据 = 「脚本跑通 + 有凭证
//      + **要求端点 200 且 `code=0`**」⇒ `judge_probe_result()`；
//   ② 失效不可诊断（`PB-07`）：运行期只在**错误串**里出现「invalid token」，
//      界面没有「重新登录」入口 ⇒ 错误串反查分类 `classify_session_failure_text()`。
//
//  口径（沿用 `M_patchAB_rest` §2.2 / §3.3 与不变量 `I15` / `I20` / `R20`）：
//   * **不看** `userToken` 判登录态（`I15`）—— 这里判的是**探测结论**，不是登录判据；
//   * 脱敏（`R20`）：本文件只做**分类**与**文案**，绝不返回任何凭证片段；
//   * 只做**只读**解析：不联网、不开窗、不写盘 ⇒ 可被 `api_probe --exec-selftest` 断言。
// ============================================================================

#include <string>
#include <vector>

#include "session_store.h"   // ProbeResult（探测结果值类型）

namespace aiwrite::web {

// ---------------------------------------------------------------------------
//  ① 端点探测报告的**结构化解析**（`endpoints_report` 是 JS 拼出的文本）
//
//  报告格式（`webview_host.cpp` 的探测脚本）：
//      `<path> -> <HTTP 状态> <响应体（已脱敏、截断 400 字）>` 以 `" || "` 连接
//      传输失败时 = `<path> -> error <异常文本>`
//  例：`/api/v0/users/current -> 200 {"code":0,"msg":""} || /api/v0/chat_session/fetch_page -> 200 {...}`
// ---------------------------------------------------------------------------
struct EndpointProbeLine {
    std::string path;             // 端点路径（去空白）
    std::string status_text;      // 原始状态段（`200` / `error …`）—— 诊断用
    int         status = 0;       // HTTP 状态码；0 = 未解析出（含传输失败）
    bool        transport_error = false; // 请求本身失败（fetch 抛错 / 网络不可达）
    std::string body;             // 响应体片段（已脱敏）
    bool        has_code = false; // 响应体里有 `code` / `biz_code` 字段
    int         code = 0;         // 业务码（has_code=false 时无意义）
};

// 解析（容错：空报告 / 缺字段 / 畸形行都不抛异常，只跳过/留空）
std::vector<EndpointProbeLine> parse_endpoints_report(const std::string& report);

// 按路径查一条（找不到返回 nullptr）—— 判据只需要**指定路径**的结论
const EndpointProbeLine* find_endpoint(const std::vector<EndpointProbeLine>& lines,
                                       const std::string& path);

// ---------------------------------------------------------------------------
//  ② 探测结论（`PB2-25` 判据收紧）
// ---------------------------------------------------------------------------
enum class ProbeVerdict {
    Pass,            // 脚本跑通 + 有凭证 + 要求端点 `200` 且 `code=0`
    NotLoggedIn,     // 脚本跑通但**没有凭证** ⇒ 请在窗口内登录一次
    SessionInvalid,  // 401 / `code=40002` ⇒ **会话已失效，必须重新登录**
    RequestRejected, // 403 / `code=40003` ⇒ 请求被站点拒绝（风控 / PoW / 前端版本）
    EndpointUnknown, // 端点缺失 / 传输失败 / 200 但没有 `code` ⇒ 形状变了，**不 PASS**
    ScriptFailed,    // 探测脚本本身失败（页面没加载 / WebView2 异常）
    NotApplicable,   // 该站点不适用协议探测（只读诊断路径）
};

std::string probe_verdict_name(ProbeVerdict verdict);

struct ProbeJudgement {
    ProbeVerdict verdict        = ProbeVerdict::ScriptFailed;
    bool         pass           = false; // 可直接用作 PASS/FAIL 与退出码 0/1
    bool         needs_relogin  = false; // 「会话已失效，点此重新登录」
    bool         session_related = false;// 结论与会话/鉴权有关（错误条据此给入口）
    std::string  reason;                 // 判据（一句话，含观测到的码）
    std::string  next_step;              // 可操作下一步（`I21`）
};

// 判据主体（纯函数）
//  * `required_path` 为空 = 默认 `/api/v0/users/current`
//  * `probe_applicable` = 该站点是否适用协议探测（`ai::probe_is_applicable`）；
//    false ⇒ `NotApplicable`（只读诊断路径自己判登录态，本函数不越权）
ProbeJudgement judge_probe_result(const ProbeResult& probe, const std::string& required_path,
                                  bool probe_applicable);

// ---------------------------------------------------------------------------
//  ③ 运行期**错误串**反查分类（`PB-07`：界面「重新登录」入口的地基）
//
//  * 输入 = 节点失败原因 / `--web-chat` 错误串（已含 `web_session_failure_hint()` 的追加部分）
//  * 容错匹配：`HTTP 401` / 401 / 403 / `code=40002` / `biz_code=40003` /「会话已失效」等
//  * 输出只有分类与码，**不含**任何凭证片段（`R20`）
// ---------------------------------------------------------------------------
struct SessionFailure {
    bool session_related = false; // 与会话 / 鉴权有关（值得给「重新登录」入口）
    bool needs_relogin   = false; // 必须重新登录（401 / 40002 /「会话已失效」）
    int  http_status     = 0;     // 识别到的 HTTP 状态码（0 = 未识别）
    int  biz_code        = 0;     // 识别到的业务码（0 = 未识别）
    std::string evidence;         // 命中的判据片段（**脱敏**：只有码 / 短语，无凭证）
};

SessionFailure classify_session_failure_text(const std::string& text);

// 一句话文案（错误条 / 面板 / 状态栏共用；`provider_id` 可空）
std::string session_relogin_text(const SessionFailure& failure, const std::string& provider_id);

// ---------------------------------------------------------------------------
//  ④ 离线自检（`api_probe --exec-selftest` 调用）；返回失败项数
// ---------------------------------------------------------------------------
int probe_verdict_selftest(int* passed_out = nullptr);

} // namespace aiwrite::web
