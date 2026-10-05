#pragma once

// ============================================================================
//  站点引用（`SiteRef`）—— M7B 批 2（`M7B-16`/`M7B-17` · §6.2 新增文件）
//
//  * **搬迁**自 `web/webview_host.h`（`LoginRequest` + 全部纯函数），**零语义变化**：
//    `--web-probe` / `--web-chat` / `--login-selftest` / 参数面板的调用点与结果**逐字不变**（守 `I2`）
//  * **目的**：让新通道（`web/pydoll_channel`）能拿到站点描述而**不 include `webview_host.h`**
//    （后者拉入 WebView2 依赖）⇒ 批 3 切换时调用方零改动（§6.2 接口映射表）
//  * `SiteRef` = 站点描述的值类型（`using SiteRef = LoginRequest;` —— **同一类型**，纯别名）
//    字段：`provider_id` · `url`（origin / 登录页）· `window_title` · `cookie_names[]` ·
//    `probe_paths[]` · `token_expr` · `probe_applicable` · **`attach`**（M7B 新增，本批不消费）
//  * 本文件**不含任何 Win32 / WebView2 / 命名管道依赖** ⇒ 可被 `api_probe` 离线断言（纯函数）
// ============================================================================

#include <string>
#include <vector>

#include "ai/provider_spec.h"     // 站点参数来自配置表条目（ProviderWebSpec）
#include "web/session_store.h"    // 站点键（origin）= site_key_of

namespace aiwrite::web {

// ---- 内置默认站点（= 改造前写死在 webview_host.h / property_panel.cpp 的常量）----
//  * 集中定义，保证「内置条目字段值 == 旧常量」（不变量 I2：`--web-probe` / `--web-chat` /
//    `--web-session-selftest` 结果与改造前一致）
//  * 配置表条目的对应字段为空时，一律回落到这里
inline constexpr const char* kDefaultSiteLoginUrl    = "https://chat.deepseek.com/";
inline constexpr const char* kDefaultSiteWindowTitle = "AIwrite · DeepSeek 网页版登录（登录后关闭本窗口）";
inline constexpr const char* kDefaultSiteProbeTitle  = "AIwrite · 网页版协议探测（登录后自动探测）";

struct LoginRequest {
    std::string url          = kDefaultSiteLoginUrl;
    std::string profile_dir;                                  // 空 = ~/.brain-ai/webview2（旧）/ pydoll-profile（新）
    std::string window_title = kDefaultSiteWindowTitle;
    bool        offscreen       = false;  // true：窗口放在桌面可见区域之外（自检用）
    int         timeout_seconds = 0;      // >0：到时自动关闭（自检用；0 = 直到用户关闭）
    bool        auto_close_after_cookies = false; // true：取到 Cookie 且稳定 3 秒后自动关闭（自检用）
    bool        probe_after_load    = false;      // 页面加载完成后自动执行协议探测（M4-06/M4-08）
    bool        auto_close_after_probe = false;   // 探测完成即关闭窗口（命令行自检用）
    // ---- M_patchB L1（PB2-05）：站点参数来自配置表（默认 = 改造前写死的 DeepSeek 常量）----
    std::vector<std::string> probe_paths;         // 协议探测路径（空 = 默认两条）
    std::string              challenge_path;      // PoW 挑战路径（空 = 默认）
    std::string              completion_path;     // 生成路径（空 = 默认）
    // ---- M_patchB L1 续（PB2-17）：站点身份 / 归属 ----
    std::string              provider_id;         // 配置表条目 id（诊断 / 界面显示）
    std::string              token_expr;          // 页面内取 token 表达式（空 = 内置脚本原样）
    std::vector<std::string> cookie_names;        // 需要的 Cookie 名（空 = 界面按内置默认显示）
    // ---- M_patchB L4（PB2-27 / PB2-28）：该站点的**协议探测**是否适用 ----
    //  * 默认 true → 无参路径 / CLI 自检（--web-probe、--web-chat、--login-selftest）**逐字不变**（守 I2）
    //  * `false`（DOM 站点）：`ensure_session` 不再等内存 `userToken`（只看该 origin 有没有 Cookie，I15），
    //    页面内只做**只读诊断**，**不注入**任何站点内部端点（I16）
    bool                     probe_applicable = true;
    // ---- M7B（`B12-C2`）：上传入口形态 `auto | file_input | drop_zone | paste_only | none` ----
    //  * **本批只解析 / 携带、运行期不消费**（取值归批 3 `P7b-10`）；缺省空串 = 未声明
    std::string              attach;
};

//: `SiteRef` = 站点描述（**与 `LoginRequest` 同一类型**的纯别名；新通道按此名调用，零语义）
using SiteRef = LoginRequest;

// ---- 纯函数（**搬迁**自 `webview_host.h`，零语义；可离线断言）----

// 登录请求所属站点键（origin）—— 会话归档 / 窗口归属判定（与 SessionStore 同源）
inline std::string login_request_site(const LoginRequest& request)
{
    return site_key_of(request.url);
}

// 由配置表条目的 `web` 段构造请求（空字段回落内置默认 → 与改造前行为一致）
//  * for_probe：探测窗口（标题缺省用「协议探测」模板）
inline LoginRequest login_request_of(const ai::ProviderWebSpec& site, const std::string& provider_id,
                                     bool for_probe, bool offscreen)
{
    LoginRequest request;
    if (!site.login_url.empty()) {
        request.url = site.login_url;
    }
    if (!site.window_title.empty()) {
        request.window_title = site.window_title;
    }
    else if (for_probe) {
        request.window_title = kDefaultSiteProbeTitle;
    }
    request.provider_id      = provider_id;
    request.offscreen        = offscreen;
    request.probe_paths      = site.probe_paths;
    request.challenge_path   = site.endpoints.challenge_path;
    request.completion_path  = site.endpoints.completion_path;
    request.token_expr       = site.token_expr;
    request.cookie_names     = site.cookie_names;
    // L4（PB2-27 / I15）：协议探测**只对内置协议站点适用**；DOM 站点 → false（不等 userToken、不注入端点）
    request.probe_applicable = ai::probe_is_applicable(site);
    request.attach           = site.attach;  // M7B（B12-C2）：只携带，运行期不消费
    return request;
}

// 兼容（= 旧常量，逐字一致）：手动登录窗口 → 页面加载后自动探测一次
inline LoginRequest interactive_login_request()
{
    LoginRequest request;
    request.url              = kDefaultSiteLoginUrl;
    request.window_title     = kDefaultSiteWindowTitle;
    request.probe_after_load = true;
    return request;
}

// 参数面板「打开登录窗口」：按生效条目的站点（PB2-17）
inline LoginRequest interactive_login_request(const ai::ProviderWebSpec& site,
                                              const std::string& provider_id)
{
    LoginRequest request = login_request_of(site, provider_id, /*for_probe=*/false, /*offscreen=*/false);
    request.probe_after_load = true;
    return request;
}

// 参数面板「探测网页版协议（dev）」：按生效条目的站点（PB2-17）
inline LoginRequest probe_login_request(const ai::ProviderWebSpec& site, const std::string& provider_id)
{
    LoginRequest request = login_request_of(site, provider_id, /*for_probe=*/true, /*offscreen=*/false);
    request.probe_after_load = true;
    return request;
}

// 会话自动引导（ensure_session）用的离屏请求：按生效条目的站点（PB2-17）
//  * ⚠️ **离屏语义只服务旧通道**（WebView2 内嵌窗口 / `builtin:deepseek` 的 `web_chat` PoW 求解）
inline LoginRequest boot_login_request(const ai::ProviderWebSpec& site, const std::string& provider_id)
{
    LoginRequest request = login_request_of(site, provider_id, /*for_probe=*/false, /*offscreen=*/true);
    request.probe_after_load = true;
    return request;
}

// 网页版文字生成（`dom_chat` · **新通道**）的会话引导请求：按生效条目的站点（PB2-17）
//  * **M7B step 11**：与 `boot_login_request` 的**唯一差别** = `offscreen=false`（**有头**）——
//    新通道起的浏览器是**独立进程**，且是用户**唯一的登录入口**（旧通道另有面板内嵌 WebView2 窗口）；
//    离屏窗口用户看不见 ⇒ 未登录时**无路可走** ⇒ 违反 `I21`（必须给出可操作路径）
//  * ⚠️ 新通道（`web/pydoll_channel`）只消费 `url` / `provider_id` / `cookie_names` ——
//    `probe_after_load` / `window_title` / 探测端点等**一律不被消费**（新通道不注入站点端点，`I16`）
inline LoginRequest visible_login_request(const ai::ProviderWebSpec& site, const std::string& provider_id)
{
    return login_request_of(site, provider_id, /*for_probe=*/false, /*offscreen=*/false);
}

// ---------------------------------------------------------------------------
// 会话自动引导决策（**纯逻辑，便于离线断言**；由 ensure_session 使用）
//
// 教训（2026-09-26 真 bug）：窗口“已打开”**不等于**内存里有凭证 ——
// 用户手动点「打开登录窗口」时页面加载后并不探测，于是 userToken 永远是空的，
// 而旧版 ensure_session 见到窗口已开就只“等”，必然等到超时 →「未取得网页版凭证」。
// 因此“窗口已开”必须映射为 **ReuseAndProbe（补一次探测）**。
// ---------------------------------------------------------------------------
enum class SessionBoot {
    HaveToken,      // 内存已有 userToken：直接用，不做任何事
    ReuseAndProbe,  // 登录窗口已开且浏览器就绪：在现有页面里补一次协议探测
    StartAndProbe,  // 无可用窗口：离屏起一个（probe_after_load=true 自动探测）
};

inline SessionBoot plan_session_boot(bool has_token, bool window_open)
{
    if (has_token) {
        return SessionBoot::HaveToken;
    }
    // 只看「窗口是否开着」，**不**看浏览器是否已就绪：已开窗口时再 start() 会因
    // 「已有登录窗口在运行」失败，而探测请求是**可重试**的（ensure_session 每 6 秒补一次），
    // 所以“还没就绪”只需重试，不该另开窗口。
    return window_open ? SessionBoot::ReuseAndProbe : SessionBoot::StartAndProbe;
}

} // namespace aiwrite::web
