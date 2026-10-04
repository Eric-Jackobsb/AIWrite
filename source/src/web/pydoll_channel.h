#pragma once

// ============================================================================
//  新通道：Pydoll 守护进程（命名管道）—— M7B 批 2（`M7B-15` / `M7B-16` · §6.2）
//
//  * 与 `web/webview_host.h` **同构的自由函数集**（§6.2 接口映射表）——
//    目的：批 3（`M7B-20`）把调用方（`ai/dom_web_client.cpp` / `nodes/local_nodes.cpp` /
//    `ui/property_panel.cpp`）**零改动**切到本通道
//  * **本批（批 1–4「只加不替换」· `B12-C1`）不接线**：生产路径仍走 WebView2；
//    本文件当前只被 `main.cpp` 的 `--pydoll-login` / `--pydoll-selftest`（批 2 冒烟）使用
//  * 传输：命名管道（`web/pipe_client.h`）× Python 守护进程（`brain_ai_browser --serve`）；
//    帧结构 = `web/channel_frames.h`（§6.1 词表）
//  * 线程契约（`M7.md` `Q4`）：本文件全部函数**阻塞式** ⇒ **不得在 UI 线程调用**
//    （批 3 接线时由执行器工作线程 / 专用线程调用）
//  * `I21`：依赖缺失（无 Python / 无浏览器 / 守护进程未起）→ `false` + **可操作错误**，
//    **不静默降级、不回落旧通道**
// ============================================================================

#include <string>

#include "web/site_ref.h"

namespace aiwrite::web::channel {

// 命令超时（毫秒；对齐 §6.1「容量与超时（写死，防膨胀）」）
inline constexpr int kTimeoutOpenMs  = 30'000;   // `open_tab`（含首次起浏览器）
inline constexpr int kTimeoutStateMs = 20'000;   // `login_state`
inline constexpr int kTimeoutCloseMs = 10'000;   // `shutdown`

// ---- 与 `webview_host` 同构的对外接口（§6.2 接口映射表）----
//  ⚠️ 本批**只落地 `ensure_session` / `selftest` / `pydoll_login`**；其余如实返回
//  「尚未实现（批 3）」+ 可操作原因（**不假装成功** —— `I21` 同族）。

// 按站点确保登录态：判据 = `I15′`（CDP Cookie 名单命中条目 `cookie_names`，**去 `userToken`**）
//  * 守护进程未起 / 浏览器未开 → 返回 false + 可操作原因（批 3 起自动 `open_tab`）
bool ensure_session(const SiteRef& site, int timeout_ms, std::string* error);

// 按站点注销（新通道应走 `Storage.clearDataForOrigin`）—— ⬜ 词表暂无该命令（批 3 加）
bool logout_site(const SiteRef& site, int timeout_ms, std::string* error);

// 在页面内执行 JS 并取回结果 —— ⬜ 批 3（`send_prompt` / `read_answer` 走各自专用命令）
bool run_script(const SiteRef& site, const std::string& script_js, int timeout_ms,
                std::string* json_result, std::string* error);

// 当前 tab 所属站点 / tab 是否在指定站点 —— ⬜ 批 3（守护进程需先暴露「当前 tab」）
std::string current_tab_site();
bool        tab_on_site(const std::string& site);

// ---- 批 2 冒烟（`M7B-19` 门槛 · `M7B-18` 的一半）----

// 通道自检：起守护进程（`--serve --once`）→ `hello` → 校验 `ready` → `shutdown` → 等退出。
//  * **不开浏览器**（只验：管道可用 / 词表版本 / 依赖探测结果），毫秒级可重复
//  * 返回退出码：0 = 通过；1 = 失败；2 = 依赖问题（无 Python / 无浏览器，`I21`）
int selftest(int timeout_ms);

// `--pydoll-login <id>`：起**有头**浏览器 → `open_tab`（条目 `web.login_url`）→ 轮询
// `login_state` 判到 `logged_in`（`cookie_names` 命中，`I15′`）→ 写内存 `SessionStore` →
// **`shutdown`（`Browser.close` 等进程退出，`I23①`）**
//  * 合规：**有头窗口 + 人工登录**（不代填密码、不绕验证，§13）
//  * 返回：0 = 登录成功；1 = 超时 / 失败（带可操作提示）；2 = 参数 / 依赖问题
int pydoll_login(const std::string& provider_id, int timeout_seconds);

} // namespace aiwrite::web::channel
