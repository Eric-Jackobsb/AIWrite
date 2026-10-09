#pragma once

// ============================================================================
//  M8 供料探针（Phase 2 · `M8-10` / `M8-12`）—— 方案 E 的第一个 Gate
//
//  要回答的问题：**WebView2 会话能否为 C++ 原生 HTTP 供料**，以及站点上传链的签名材料
//  （`s=` / `device_id`）**从哪来**。
//
//  已到手的侦察物证（`source/python/_probe/out/m7b28_doubao_recon*.json`，豆包 `doubao-web`）：
//    ① GET  /alice/resource/prepare_upload?<公共查询串>&device_id=<...>      （`device_id` = **客户端参数**）
//    ② GET  /top/v1?Action=ApplyImageUpload&...&s=<11 字符短票据>            （`s=` 的产出处**未知**）
//    ③ POST https://tos-hl-x.snssdk.com/upload/v1/<bucket>/<hash>.png        （TOS 直传 = 文件字节）
//    ④ POST /top/v1?Action=CommitImageUpload&...
//  ⇒ 若 `s=` 由 ① 的响应下发 ⇒ 方案 E **无需页面 JS 签名**；若由前端计算 ⇒ 仍需页面内供料
//    （`I23` 允许 —— 只要物料来自**本机 WebView2 会话**的实测产出）。
//
//  本探针（**站点无关**：不内置任何厂商常量；站点查询串由 `--get` 传参）：
//    1) 按条目 `interactive_login_request` → `web::ensure_session`（有头 WebView2 窗口；需人工登录）
//    2) 页面内只读取样（`web::run_script_sync`）：`device_id` 候选（localStorage / sessionStorage）
//       + `document.cookie` 名清单；**不写入、不点击、不发送**
//    3) 会话取样（`web::SessionStore`）：Cookie 条数 + 条目 `cookie_names` 命中（值一律脱敏）
//    4) 仅当给了 `--get`：用 2)3) 的物料发**一次 GET**（同一 origin），打印状态码 + 响应体前 4 KiB，
//       并扫描响应里是否出现签名类键名 ⇒ 直接给出「服务端下发 / 需页面 JS」的判定
//    5) 全过程写 `~/.brain-ai/logs/m8_supply_probe_<时间戳>.json`（供人工核对与外发前脱敏）
//
//  退出码：0 = 完成 · 1 = 页面 / 会话未就绪（含未登录）· 2 = 条目或站点不可用
// ============================================================================

#include <string>

namespace aiwrite::ai {

// provider_id 空 = 配置表里第一个 `kind=web` 条目
// timeout_s  < 60 视为「用默认 300 秒」（人工登录需要时间）
// get_path   "路径?查询串"（例：`/alice/xxx?a=1&b=2`）；**空 = 只读模式**（不碰站点接口）
int upload_supply_probe(const std::string& provider_id, int timeout_s, const std::string& get_path);

} // namespace aiwrite::ai
