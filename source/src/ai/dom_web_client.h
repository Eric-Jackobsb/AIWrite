#pragma once

// ============================================================================
//  通用网页版适配器（DOM 驱动）—— M_patchB L3（PB2-13）
//
//  设计目标：把「任意站点」降级为**纯数据**（配置表 `web` 段的选择器 + 轮询参数），
//  程序里不出现任何站点专有常量。
//
//  执行链（**v4 · step 14 · P4 起**：走新通道的**协议命令**，页面上下文由守护进程持有）：
//    1) 按站点 ensure_session（凭证按 origin 隔离；未登录 → 给可操作指引，不代填密码、不绕过验证）
//    2) `send_prompt`：挑选首个「**命中且可见**」的 `input_selector` → **真打字**（humanize）
//       → 按 `send.kind` 触发（`key` → 按键；`click` → 点 `send.value` 选择器）
//    3) `read_answer`：轮询 `answer_selector` 的 innerText（**最后一个**匹配节点 = 本轮回答）：
//         · done_when = selector_present / selector_gone → 条件成立即完成
//         · 未配 done_when → 「文本连续 N 轮不变」视为完成（Python 侧 `DEFAULT_STABLE_ROUNDS`）
//         · 超 48 KiB → **显式截断**（`truncated=true` ⇒ 写 `warning`；`I21` 不假装完整）
//  * 站点选择器 / 发送方式**由条目下发**（`I14`：本文件不认识任何站点）
//
//  纯函数（离线可断言，见 `VB2-22`）：clamp_poll_params / dom_cfg_json /
//  dom_kickoff_script / dom_poll_script / dom_probe_script
//  ⚠️ **P4 起**：这些脚本常量**保留为诊断资产**（`--web-dom-dump` / `--web-adapter-selftest` 仍用），
//  **生产路径不再使用**（口径：「**纯函数不变 + 生产不再使用**」）
// ============================================================================

#include <string>

#include "ai/provider_spec.h"

namespace aiwrite::ai {

struct DomChatRequest {
    std::string     prompt;              // 提示词
    ProviderWebSpec site;                // 站点（选择器 / done_when / 轮询参数 / 登录页）
    std::string     provider_id;         // 条目 id（日志 / 站点键归属）
    int             timeout_ms = 180000; // 总超时（含 ensure_session）
};

struct DomChatResult {
    bool        ok = false;              // true = 至少取到文本（可能带 warning）
    std::string text;                    // 回答正文
    std::string warning;                 // 已达上限 / 文本未稳定（R13：如实提示）
    std::string error;                   // ok=false 时的可操作原因
    int         polls = 0;               // 实际轮询次数
    long long   elapsed_ms = 0;
    std::string steps;                   // 诊断摘要（input 命中 / send / answer 命中数）
};

// 轮询参数钳制：poll_ms ∈ [200, 2000]、max_polls ∈ [10, 600]（R13：有硬上限）
void clamp_poll_params(const ProviderWebSpec& site, int* poll_ms, int* max_polls);

// 注入给页面的配置 JSON（**转义安全**：提示词含引号 / 换行也不会破坏脚本）
std::string dom_cfg_json(const DomChatRequest& request);
// 常量脚本（读 `window.__aiwriteDom`）：kickoff = 写入提示词 + 触发发送；poll = 取答案 + 判结束
std::string dom_kickoff_script();
std::string dom_poll_script();
// PB2-15：选择器探测脚本（读 `window.__aiwriteDomProbe`；返回各选择器命中数 / 可见性 / done_when 现状 / token 形状）
std::string dom_probe_script();

// 执行（需要已登录窗口；内部完成 ensure_session + 页面交互）
DomChatResult dom_chat(const DomChatRequest& request);

// PB2-15：**选择器探测 / 诊断**（只读，不发送任何内容）—— 按条目诊断 dom 站点
//  * 打印：当前页面 URL/标题、各选择器命中数、`done_when` 现状、`token_expr` 取值形状、Cookie 可读性
//  * 并给**可操作修复建议**（F12 Copy selector / 改 send 方式 / 修 token_expr）
//  * 返回码：0 = 必需选择器全部命中；1 = 有缺项；2 = 站点不可用 / 非 dom 条目
int dom_adapter_selftest(const std::string& provider_id, int timeout_ms);

// M_patchB L4（PB2-29 / v15）：**选择器候选枚举**（只读）—— 让「填选择器」不必靠人肉 F12
//  * 在当前页面枚举候选 input / textarea / contenteditable、发送按钮、回答容器，打印指纹 + **建议选择器**
//  * 返回码：0 = 枚举成功；1 = 页面 / 脚本未就绪；2 = 条目不可用（非网页版条目 / 缺 login_url / 表外 id）
int dom_selector_dump(const std::string& provider_id, int timeout_ms);

} // namespace aiwrite::ai
