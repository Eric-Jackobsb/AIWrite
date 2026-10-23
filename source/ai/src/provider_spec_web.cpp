// provider_spec_web.cpp -- Provider 配置表：网页版站点派生 / 登录态判定 / 探测适用性
//
// ARCH-Z1（2026-10-23）：从 provider_spec.cpp 拆出（原 248〜505 行）。
//   webSpecFor（旧回落语义，CLI/自检守 I2）· strictWebSpecFor（界面/运行期**不回落**，
//   D-22② / I14）· 不可用文案与字段缺失清单（D-26）· 登录态判定（D-27）·
//   探测适用性（D-28 / I16）· adapter 是否已实现（R12）。全部纯函数，无 IO。

#include "provider_spec.h"
#include "provider_spec_detail.h"

#include <string>
#include <vector>

namespace aiwrite::ai {

using provider_detail::joinCn;
using provider_detail::startsWith;

// ============================================================================
//  网页版站点：派生 / 文案 / 登录态 / 探测适用性（PB2-17 · 22 · 26 · 27 · 28）
// ============================================================================
const ProviderWebEndpoints& defaultDeepseekWebEndpoints()
{
    static const ProviderWebEndpoints endpoints; // 与改造前常量逐字一致
    return endpoints;
}

// M_patchB L1 续（PB2-17）：生效条目 → 网页版站点参数（web 条目用自己；否则回落表内第一个 web 条目）
ProviderWebSpec webSpecFor(const ProviderSpec* spec, const ProviderSpecs* table)
{
    if (spec != nullptr && spec->kind == "web") {
        return spec->web;
    }
    if (table == nullptr) {
        return ProviderWebSpec{};
    }
    for (const ProviderSpec& item : table->items) {
        if (item.kind == "web") {
            return item.web; // 内置表第一个 web 条目 = deepseek-web（保持改造前行为）
        }
    }
    return ProviderWebSpec{};
}

std::string webProviderIdFor(const ProviderSpec* spec, const ProviderSpecs* table)
{
    if (spec != nullptr && spec->kind == "web") {
        return spec->id;
    }
    if (table == nullptr) {
        return {};
    }
    for (const ProviderSpec& item : table->items) {
        if (item.kind == "web") {
            return item.id;
        }
    }
    return {};
}

// ---- M_patchB L1 修订（PB2-22 / D-22② / I14）：**严格**站点解析（不回落）----
ProviderWebSpec strictWebSpecFor(const ProviderSpec* spec)
{
    if (spec == nullptr || spec->kind != "web") {
        return {}; // 非网页版条目**没有**站点（绝不回落到「表内第一个 web 条目」）
    }
    return spec->web;
}

std::string strictWebProviderIdFor(const ProviderSpec* spec)
{
    if (spec == nullptr || spec->kind != "web") {
        return {};
    }
    return spec->id;
}

namespace {

// 统一的三条可操作引导（界面 / 运行前校验 / 运行期文案**逐字一致**）
std::string web_site_guidance()
{
    return "；可选：① 把「提供商」改为网页版条目（如 deepseek-web）；"
           "② 新建网页版站点条目（放进 ~/.brain-ai/providers.d/，格式见使用说明 §9 / M_patchB 附录 D）；"
           "③ 把「模式」改回 official";
}

} // namespace

std::string webSiteError(const ProviderSpec* spec)
{
    if (spec == nullptr) {
        return "配置表里没有该提供商条目，无法确定网页版站点" + web_site_guidance();
    }
    const std::string display = spec->display.empty() ? spec->id : spec->display;
    if (spec->kind != "web") {
        return "「" + display + "」不是网页版条目（没有网页版站点，**不会**回落到内置默认站点）" +
               web_site_guidance();
    }
    if (spec->web.login_url.empty()) {
        return "「" + display +
               "」缺少 web.login_url（站点登录页）——无法确定要打开哪个站点"
               "（**不会**回落到内置默认站点）" +
               web_site_guidance();
    }
    return {};
}

std::string webSiteFieldWarnings(const ProviderSpec* spec)
{
    if (spec == nullptr || spec->kind != "web") {
        return {};
    }
    std::vector<std::string> login_missing; // 影响「自动探测凭证 / 展示」
    std::vector<std::string> gen_missing;   // 生成字段（未就绪）
    if (spec->web.window_title.empty()) {
        login_missing.push_back("window_title");
    }
    if (spec->web.probe_paths.empty()) {
        login_missing.push_back("probe_paths");
    }
    if (spec->web.token_expr.empty()) {
        login_missing.push_back("token_expr");
    }
    if (spec->web.cookie_names.empty()) {
        login_missing.push_back("cookie_names");
    }
    if (spec->web.input_selector.empty()) {
        gen_missing.push_back("input_selector");
    }
    if (spec->web.send_kind.empty() || spec->web.send_value.empty()) {
        gen_missing.push_back("send");
    }
    if (spec->web.answer_selector.empty()) {
        gen_missing.push_back("answer_selector");
    }
    std::string text;
    if (!login_missing.empty()) {
        // M_patchB L4（PB2-27④ / 不变量 I16）：DOM 站点**不适用**协议探测 →
        // 不得写成「无法自动探测凭证」（那是 DeepSeek 协议站点的语义，会被读作「登录失败」）
        if (!probeIsApplicable(spec->web)) {
            text = "条目缺 " + joinCn(login_missing) +
                   "：**协议探测不适用**（DOM 站点：登录态由浏览器 profile 维持，"
                   "登录 / 生成不受影响；探测仅做只读诊断）";
        }
        else {
            // 决策 D-26 / PB2-26：这些字段**可以**回落默认值，但对非 DeepSeek 站点是**误导** → 点明后果
            text = "条目缺 " + joinCn(login_missing) +
                   "：该站点**无法自动探测凭证**（登录仍可用）";
        }
    }
    if (!gen_missing.empty()) {
        if (!text.empty()) {
            text += "；";
        }
        text += "生成未就绪（缺 " + joinCn(gen_missing) + "）";
    }
    return text;
}

// M_patchB L1 修订（PB2-26）：**登录型站点条目** —— 生成字段（选择器 / 发送 / 取答案）未就绪
//  * 登录 / 协议探测**可用**；生成会在运行时因「适配器未实现 / 生成字段未就绪」**明确报错**
//  * 不新增 schema 字段：L3 落地后把字段补齐即**自动**变为就绪
bool webLoginOnly(const ProviderSpec* spec)
{
    if (spec == nullptr || spec->kind != "web" || spec->web.adapter != "dom") {
        return false;
    }
    return spec->web.input_selector.empty() || spec->web.send_kind.empty() ||
           spec->web.send_value.empty() || spec->web.answer_selector.empty();
}

// ---- M_patchB L4（PB2-27）：站点无关的登录态判定 / 显示条件 / 探测适用性（纯函数）----
// 说明：三条都**不含**任何 DeepSeek 专有物（不读 userToken / ds_session_id / /api/v0/*，不变量 I15）
WebSessionVerdict webSessionState(const ProviderSpec* spec, const WebSessionEvidence& evidence)
{
    WebSessionVerdict verdict;
    if (spec == nullptr || spec->kind != "web") {
        verdict.state  = WebSessionState::unknown;
        verdict.reason = "该条目不是网页版条目：没有站点登录态";
        return verdict;
    }

    // ① 条目声明了 cookie_names → 命中任一即「已登录」（D-27 ①，站点无关证据优先）
    if (!spec->web.cookie_names.empty()) {
        for (const std::string& name : evidence.cookie_names) {
            for (const std::string& wanted : spec->web.cookie_names) {
                if (name == wanted) {
                    verdict.state  = WebSessionState::loggedIn;
                    verdict.reason = "该站点 Cookie「" + name + "」已就位（判据：条目 cookie_names 命中）";
                    return verdict;
                }
            }
        }
        if (!evidence.cookies_known) {
            verdict.state  = WebSessionState::unknown;
            verdict.reason = "尚未读取该站点 Cookie（点「打开登录窗口」，或直接点运行由程序复读）";
            return verdict;
        }
        verdict.state  = WebSessionState::logged_out;
        verdict.reason = "该站点 Cookie 里没有 " + joinCn(spec->web.cookie_names) +
                         "（未登录，或会话已失效）";
        return verdict;
    }

    // ② 未配 cookie_names → 该 origin 只要有 Cookie 即视为已登录（D-27「并集」）
    if (evidence.cookieCount > 0) {
        verdict.state  = WebSessionState::loggedIn;
        verdict.reason = "该站点已有 " + std::to_string(evidence.cookieCount) +
                         " 条 Cookie（判据：该 origin Cookie 非空；条目未配 cookie_names）";
        return verdict;
    }
    if (evidence.cookies_known) {
        verdict.state  = WebSessionState::logged_out;
        verdict.reason = "该站点没有 Cookie（未登录）";
        return verdict;
    }
    verdict.state  = WebSessionState::unknown;
    verdict.reason = "尚未读取该站点 Cookie（点「打开登录窗口」，或直接点运行由程序复读）";
    return verdict;
}

std::string webSessionStateLabel(WebSessionState state)
{
    switch (state) {
    case WebSessionState::loggedIn:
        return "已登录";
    case WebSessionState::logged_out:
        return "未登录";
    default:
        return "未确认";
    }
}

bool webShowsUserToken(const ProviderSpec* spec)
{
    return spec != nullptr && spec->kind == "web" && !spec->web.token_expr.empty();
}

bool probeIsApplicable(const ProviderWebSpec& web)
{
    if (web.adapter.empty() || startsWith(web.adapter, "builtin:")) {
        return true; // 内置适配器：协议探测（站点端点 / PoW / token）就是为它设计的
    }
    // 非内置（dom 等）：只有**显式**配了探测字段才适用（endpoints 对 dom 无意义，加载期已警告）
    return !web.probe_paths.empty() || !web.token_expr.empty();
}

bool probeIsApplicable(const ProviderSpec* spec)
{
    return spec != nullptr && spec->kind == "web" && probeIsApplicable(spec->web);
}


bool webAdapterImplemented(const std::string& adapter)
{
    if (adapter.empty()) {
        return true; // 空 = 用内置默认适配器行为
    }
    for (const std::string& candidate : implementedWebAdapters()) {
        if (candidate == adapter) {
            return true;
        }
    }
    return false;
}

} // namespace aiwrite::ai
