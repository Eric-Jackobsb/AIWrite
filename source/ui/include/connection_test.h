#pragma once

// ============================================================================
//  面板诊断入口（M_patchAB §3.2 `PB2-07` / §3.4 `PB2-30②` / §2.2 `PB-07`）
//
//  三件事、一个文件（都是「界面按钮 → 只读诊断」）：
//   ① `request_connection_check(id)`  = 面板「测试连接」（`PB2-07`）
//      · 离线：`ai::provider_spec_selftest()`（**与 CLI 同一实现**，不重写第二套）
//      · API 条目：按表解析 Key（env → 凭据库）；有 Key 时发一条 `ping`（「请只回复 pong」）
//      · 网页版条目：登录态 + 生效端点一致性；**不发送任何提示词**（避免误触发站点调用与风控）
//      · 码语义与 CLI 一致：0 全通过 / 1 失败 / 2 无 Key 或未登录
//   ② `request_selector_check(id)`    = 面板「测试选择器」（`PB2-30②`）
//      · 复用只读探测 `ai::dom_site_probe()`（不写入、不发送、不读任何值）
//      · 把**命中数 / 登录判据 / 建议**同时写进 Console 与面板结果区
//      · 码语义与 CLI `--web-adapter-selftest` 一致：0 全命中 / 1 有缺项 / 2 条目不可用 / 3 需人工登录
//   ③ `open_web_login_window(id)`     = 错误条 / 面板的「重新登录」（`PB-07`）
//      · 严格按条目解析站点（**不回落**内置默认站点 —— `D-22②` / `I14`）
//
//  线程模型：①② 都在**工作线程**执行（界面不阻塞）；细节走 Console（`log::info`），
//            结论经 `EditorState::post_background_status()` 投递给主线程写状态栏。
//  不变量：`I15`（判据不看 `userToken`）/ `I16`（不注入站点内部端点）/ `R20`（脱敏）。
// ============================================================================

#include <string>

namespace aiwrite::ui {

// 正在跑的后台诊断（面板据此显示「正在…」并禁用按钮）
enum class DiagnosticKind {
    None,       // 空闲
    Connection, // 「测试连接」
    Selector,   // 「测试选择器」
};

DiagnosticKind diagnosticRunningKind();
bool           diagnosticRunning();

// 最近一次结果：`report` = 多行明细（面板结果区）；`summary` = 一行结论（状态栏）
std::string diagnosticReport();
std::string diagnosticSummary();
int         diagnosticCode(); // 最近一次的码（0/1/2/3；无记录 = -1）
std::string diagnosticTitle();// 最近一次是谁的结果（面板标题用）

// ① 面板「测试连接」（`PB2-07`）—— 异步；`provider_id` 空 ⇒ 只跑离线部分
void requestConnectionCheck(const std::string& provider_id);

// ② 面板「测试选择器」（`PB2-30②`）—— 异步；只读探测（不写入、不发送）
void requestSelectorCheck(const std::string& provider_id);

// ③ 打开该条目的登录窗口（`PB-07`）；成功/失败都写 Console 与状态栏
void openWebLoginWindow(const std::string& provider_id);

// 退出前收尾：等待后台诊断结束（最长 wait_ms；超时则分离线程并如实记日志）
//  * 由 `main.cpp` 在 `log::shutdown()` **之前**调用（避免退出期写日志竞态）
void stopDiagnostics(int wait_ms = 4000);

} // namespace aiwrite::ui
