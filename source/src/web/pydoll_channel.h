#pragma once

// ============================================================================
//  新通道：Pydoll 守护进程（命名管道）—— M7B 批 2（`M7B-15` / `M7B-16` · §6.2）
//
//  * 与 `web/webview_host.h` **同构的自由函数集**（§6.2 接口映射表）—— 调用方
//    （`ai/dom_web_client.cpp` / `nodes/local_nodes.cpp` / `ui/property_panel.cpp`）按名切换
//  * **接线进度（批 3）**：step 9 诊断换代（`--web-dom-dump` / `--web-adapter-selftest`）·
//    step 10 `dom_chat`（网页版文字生成）· **step 11 会话族**（`login_site` / `logout_site` /
//    `current_tab_site` / `tab_on_site` · **词表 v3**）· **step 12（`M7B-44`）登录轮询收口**
//    （`login_site` **观测失败即止** + `request_cancel_session_ops()`；**词表不变仍 v3**）—— UI 面板换代随 step 11 同批
//  * 传输：命名管道（`web/pipe_client.h`）× Python 守护进程（`brain_ai_browser --serve`）；
//    帧结构 = `web/channel_frames.h`（§6.1 词表 · **v3**）
//  * 线程契约（`M7.md` `Q4`）：本文件全部函数**阻塞式** ⇒ **不得在 UI 线程调用**
//    （调用方用执行器工作线程 / 专用线程 —— 面板登录窗口沿用旧 WebView2 的独立线程模式）
//  * `I21`：依赖缺失（无 Python / 无浏览器 / 守护进程未起）→ `false` + **可操作错误**，
//    **不静默降级、不回落旧通道**
// ============================================================================

#include <string>
#include <vector>

#include "web/site_ref.h"

namespace aiwrite::web::channel {

// 命令超时（毫秒；对齐 §6.1「容量与超时（写死，防膨胀）」）
inline constexpr int kTimeoutOpenMs  = 30'000;   // `open_tab`（含首次起浏览器）
inline constexpr int kTimeoutStateMs = 20'000;   // `login_state`
inline constexpr int kTimeoutCloseMs = 10'000;   // `shutdown`
inline constexpr int kTimeoutScriptMs = 20'000;  // `run_script`（v2 · 只读诊断脚本）

// ---- 与 `webview_host` 同构的对外接口（§6.2 接口映射表）----
//  ⚠️ **批 3 起全部落地**（无占位）：`ensure_session` / `run_script` / `logout_site` /
//  `current_tab_site` / `tab_on_site` / `login_site` / `selftest` / `script_selftest` / `pydoll_login`
//  —— 任何失败一律回**可操作原因**（**不假装成功** —— `I21` 同族）。

// ---- 会话生命周期（批 3 step 9 · `M7B-18` 诊断换代的地基）----
//  * 语义与 WebView2 版**同名函数对齐**：`ensure_session` = 「确保**有活会话且已导航到站点**」
//    （step 6 的老实现只是**只读判定**：要求已有守护进程在跑 ⇒ 诊断工具自己起不了会话）
//  * ⚠️ `ensure_session` 会起**常驻浏览器**（`--serve` **不带** `--once`）⇒ 调用方**必须**在收尾调
//    `shutdown_session()`，否则留孤儿浏览器 / 守护进程（`I23` 同族纪律）
bool session_ready();      // 纯查询：本进程是否已有活动会话（**不起任何东西**）
bool shutdown_session();   // 关会话（`Browser.close` → 等进程退出）；返回「是否确实关过」；**幂等**

// **M7B step 12（`M7B-44`）**：请求取消**正在进行的登录态轮询**（`login_site` / `pydoll_login`）
//  * 面板关窗 / 进程退出时由 `ui::wait_web_tasks()` 调用 —— 否则退出要把登录上限干等完
//    （实测旧版：关主窗口后还多等了 3 分钟）
//  * **幂等 · 线程安全**；只改**轮询循环的继续条件**，不动会话、不发任何命令
//  * 下一次 `login_site` / `pydoll_login` 进入时会**自动复位**（一次取消只作用于在跑的那一次）
void request_cancel_session_ops();

// 确保会话 + 按站点判定登录态（判据 `I15′` = CDP Cookie 名单命中条目 `cookie_names`，**去 `userToken`**）
//  * 无会话 → 起守护进程（**常驻**）+ `open_tab`（站点 `url`）；已有会话 → **复用**（不重复起浏览器）
//  * 未登录 → false + 可操作原因，但**会话仍可用**（只读诊断脚本照跑 —— 由调用方决定）
bool ensure_session(const SiteRef& site, int timeout_ms, std::string* error);

// 「打开登录窗口」（**新通道 · v3 · step 11**）：确保**有头**会话 → `open_tab`（站点 `url`）→
//   轮询 `login_state` 直到 `logged_in`（判据 `I15′` = 条目 `cookie_names` 命中）→ 写内存
//   `SessionStore` → **保持会话**（**不** `shutdown`）
//  * 与 CLI 的 `pydoll_login` 的**唯一差别**：登录成功后**不关会话** ——
//    用户刚登录的浏览器**继续给网页节点复用**（`dom_chat` 的 `ensure_session` 会复用同一会话）
//  * 已有会话（管道能连上）→ **复用**（不重复起浏览器）；无会话 → 起**常驻**守护进程
//  * ⚠️ **阻塞式**（`Q4`）⇒ UI **不得**在 UI 线程调用 —— 面板用独立后台线程（同旧 WebView2 窗口做法）
//  * 返回：true = 已登录；false + *error = 超时 / 依赖问题（**可操作**；`I21`：**绝不**回落旧通道）
//  * 收尾责任：调用方负责在本进程退出前 `shutdown_session()`（与 `ensure_session` 同族纪律）
bool login_site(const SiteRef& site, int timeout_seconds, std::string* error);

// 按站点注销（**v3 · step 11 已落地**）：`Storage.clearDataForOrigin` —— 只清**该 origin** 的
//   Cookie / localStorage / IndexedDB / Cache（**不影响**其他站点；**不删** profile）
//  * `origin` 由本函数按 `login_request_site(site)` 派生后下发（`I14`：站点只来自条目）
//  * 成功后同步清内存 `SessionStore` 该站点会话（旧通道 `logout_site` 的既有语义一致）
//  * **会话不在运行 / 命令失败** → false + 可操作原因（`I21`：**不假装成功**、**不回落旧通道**）
bool logout_site(const SiteRef& site, int timeout_ms, std::string* error);

// 在页面内执行 JS 并取回结果（**批 3 step 8 已落地**：词表 v2 的 `run_script` → `script_done`）
//  * 语义 = **只读诊断**（`--web-dom-dump` / `--web-adapter-selftest` 走新通道的地基，`M7B-18`）；
//    注入提示词仍归 `send_prompt`（`I18` 物理分离）—— 站点选择器由**调用方**给出（`I14`）
//  * 结果超 48 KiB → 服务端**截断**并在 `*error` 给出说明（**返回 true**：命令成功但数据不全，
//    调用方**必须提示「已截断」**，不得假装完整）
bool run_script(const SiteRef& site, const std::string& script_js, int timeout_ms,
                std::string* json_result, std::string* error);

// ---- 内容返回（**v4 · step 14**：`upload_image` / `send_prompt` / `read_answer`）----
//  * 与 `run_script` 同族的**阻塞**接口 ⇒ **仅后台线程 / CLI**（`Q4`：渲染路径禁止）
//  * 站点知识**全在调用方**（`I14`）：选择器 / 发送方式由参数下发，本层不认识任何站点
//  * 失败一律 false + **可操作原因**（`I21`：不回落旧通道、不假装成功）

// `upload_image`：注入本地图片（**只注入、不发提示词** —— `I18` 物理分离）
//  * `attach_selector` = 文件输入框（`<input type=file>`）选择器 —— **由调用方按条目下发**
//  * `attach`：`auto` / `file_input`（主路线）｜`drop_zone` / `paste_only`（**未实现** ⇒ 拒绝）
//    ｜`none`（该站点无上传入口 ⇒ 拒绝）
//  * 成功 ⇒ 服务端发 `evidence{page, network:[], both=false}`（**网络回执归 `P7b-11`**，如实标注）
bool upload_image(const SiteRef& site, const std::vector<std::string>& images,
                  const std::string& attach, const std::string& attach_selector,
                  int timeout_ms, std::string* error);

// `send_prompt`：挑输入框 → **真打字**注入 → 按 `send` 触发（`key` / `click`）→ `stage{send, ok}`
//  * `input_selector` = 多候选（首个「命中且可见」者胜 · `M7B-24`）；全不中 ⇒ 可操作失败
//  * `send.kind` = `key`（`send_value` 缺省 `Enter`）｜`click`（`send_value` = CSS 选择器）
//  * `upload_evidence = true` ⇒ 服务端按 `I18` 协议级拦截（本会话无上传成功证据 → 拒绝发送）
bool send_prompt(const SiteRef& site, const std::string& prompt,
                 const std::vector<std::string>& input_selector,
                 const std::string& send_kind, const std::string& send_value,
                 bool upload_evidence, int timeout_ms, std::string* error);

// `read_answer`：轮询 `answer_selector` → `answer_done{text, text_bytes, truncated}`
//  * `done_when_kind` = `selector_present` / `selector_gone`（空 = 「文本连续 N 轮稳定」）
//  * `truncated` = true 时**仍返回 true**，但 `*error` 带说明 ⇒ 调用方**必须**标注「已截断」（`I21`）
bool read_answer(const SiteRef& site, const std::vector<std::string>& answer_selector,
                 const std::string& done_when_kind, int poll_ms, int max_polls,
                 int timeout_ms, std::string* text, bool* truncated, int* text_bytes,
                 std::string* error);

// 当前 tab 所属站点 / tab 是否在指定站点（**v3 · step 11 已落地**，走 `current_tab` 命令）
//  * `current_tab_site()`：无会话 / 读失败 → **空串**（调用方按「无 tab / 未知」处置 —— 不假装）
//  * `tab_on_site(site)`：入参可为**站点键（origin）**或**任意 URL**（内部先归一到 origin 再比）
//    —— 语义对齐旧通道 `window_on_site`（`site_key_of(current) == site_key_of(want)`）
std::string current_tab_site();
bool        tab_on_site(const std::string& site);

// ---- 批 2 冒烟（`M7B-19` 门槛 · `M7B-18` 的一半）----

// 通道自检：起守护进程（`--serve --once`）→ `hello` → 校验 `ready` → `shutdown` → 等退出。
//  * **不开浏览器**（只验：管道可用 / 词表版本 / 依赖探测结果），毫秒级可重复
//  * 返回退出码：0 = 通过；1 = 失败；2 = 依赖问题（无 Python / 无浏览器，`I21`）
int selftest(int timeout_ms);

// **跨语言端到端**（批 3 step 8 · 词表 v2）：起守护进程 → `open_tab`（**本地临时 HTML**）→
// `run_script` 读 DOM / 验类型保真 → 空脚本应回可操作错误 → `shutdown`
//  * 验的能力 = `--web-dom-dump` / `--web-adapter-selftest` 换代后所依赖的
//    「**页面内执行 JS + 取回结构化结果**」（`M7B-18`）
//  * **零外网、零登录**（夹具落临时目录 · `file:///`）；开的是**独立管道名**，不撞生产 / 其他自检
//  * 返回：0 = 通过；1 = 失败；2 = 依赖问题
int script_selftest(int timeout_seconds);

// **内容返回端到端**（**step 14 · P4** · 词表 v4）：起守护进程 → `open_tab`（本地 `file:///` 夹具：
// 输入框 + 发送按钮 + 回答容器）→ **`send_prompt`**（真打字 + 触发发送）→ 逐字符自证（`run_script` 只读）
// → **`read_answer`**（正文与夹具渲染**字节一致**）→ `I18` 拦截 / `attach_unsupported` 可操作拒绝 → `shutdown`
//  * 验的能力 = **生产内容路径（v4 三命令）**：注入 / 发送 / 取回答 + 长文本截断契约
//  * **零外网、零登录**；独立管道名（不撞生产 / 其他自检）
//  * 返回：0 = 通过；1 = 失败；2 = 依赖问题
int chat_selftest(int timeout_seconds);

// `--pydoll-login <id>`：起**有头**浏览器 → `open_tab`（条目 `web.login_url`）→ 轮询
// `login_state` 判到 `logged_in`（`cookie_names` 命中，`I15′`）→ 写内存 `SessionStore` →
// **`shutdown`（`Browser.close` 等进程退出，`I23①`）**
//  * 合规：**有头窗口 + 人工登录**（不代填密码、不绕验证，§13）
//  * 返回：0 = 登录成功；1 = 超时 / 失败（带可操作提示）；2 = 参数 / 依赖问题
int pydoll_login(const std::string& provider_id, int timeout_seconds);

} // namespace aiwrite::web::channel
