#pragma once

// ============================================================================
//  通用网页版适配器（DOM 驱动）—— M_patchB L3（PB2-13）
//
//  设计目标：把「任意站点」降级为**纯数据**（配置表 `web` 段的选择器 + 轮询参数），
//  程序里不出现任何站点专有常量。
//
//  执行链（全部在**已登录窗口**的页面上下文里完成；技术基础 = webview_host 的 ExecuteScript）：
//    1) 按站点 ensure_session（凭证按 origin 隔离；未登录 → 给可操作指引，不代填密码、不绕过验证）
//    2) 注入提示词：contenteditable → insertText（触发 input 事件）；input/textarea → 原生 setter + input/change
//    3) 触发发送：send.kind = key（默认 Enter → keydown/keypress/keyup）/ click（点击 send_value 选择器）
//    4) 轮询 answer_selector 的 innerText（**最后一个**匹配节点 = 本轮回答）：
//         · done_when = selector_present / selector_gone → 条件成立即完成
//         · 未配 done_when → 「文本连续 stable_rounds 轮不变」视为完成
//         · 到达上限 → **如实返回已取文本 + 明确警告**（R13：不假装成功、不无限等待）
//
//  纯函数（离线可断言，见 `VB2-22`）：clamp_poll_params / dom_cfg_json /
//  dom_kickoff_script / dom_poll_script / dom_probe_script
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

} // namespace aiwrite::ai
