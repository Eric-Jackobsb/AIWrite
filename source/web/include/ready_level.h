#pragma once

// ============================================================================
//  就绪三级（M_patchC §4.2 · 不变量 I25 · PW-03）—— **纯函数**部分
//
//  背景（真机 2026-10-06 · 三次运行实证）：
//    * `113 ms`「登录窗口未就绪」= 就绪判据只到**凭证级**（Cookie 非空），
//      `ensure_session` 在窗口 controller 尚未创建时就 `return true`；
//    * `41 ms`「0 个 input[type=file]」= 页面不在**交互态**（附件区懒加载未挂载），
//      却被**错误归因**为「该站点可能是拖拽 / 粘贴入口」（`G6`）。
//
//  三级（逐级包含，只增不改）：
//    L1 窗口级      controller / webview 已创建（`g_window` && `g_webview`）
//    L2 页面级      `document.readyState` 完成 **且** 实时 URL 属于目标站点
//    L3 交互态级    composer 命中（可输入） **且**（上传场景）file input 命中
//
//  本文件**纯函数**（无 Win32 / WebView2 / IO）⇒ 可被 `api_probe --selftest` 断言。
// ============================================================================

#include <string>

namespace aiwrite::web {

// 就绪需求位（可组合）
enum ReadyNeed : int {
    kReadyNone      = 0,
    kReadyWindow    = 1, // L1
    kReadyPage      = 2, // L2
    kReadyComposer  = 4, // L3-a：输入框可交互（文字生成 / 选择器探测）
    kReadyFileInput = 8, // L3-b：附件入口（`input[type=file]`）已挂载（图片上传）
};

// 常用组合
constexpr int kReadyScript = kReadyWindow | kReadyPage | kReadyComposer; // 文字生成 / 探测
constexpr int kReadyUpload = kReadyScript | kReadyFileInput;             // 图片上传（**先上传**）
constexpr int kReadyFull   = kReadyUpload;

// 已达掩码是否覆盖需求（纯位运算）
inline bool ready_satisfied(int reached, int need) { return (reached & need) == need; }

// 单个位的名字（日志 / 报告）
inline std::string ready_bit_name(int bit)
{
    switch (bit) {
    case kReadyWindow:    return "窗口级";
    case kReadyPage:      return "页面级";
    case kReadyComposer:  return "交互态（输入框）";
    case kReadyFileInput: return "交互态（附件入口）";
    default:              return "?";
    }
}

// 已达级别的人类可读串（例：「窗口级 + 页面级」；一个都没到 → 「未就绪」）
inline std::string ready_reached_text(int reached)
{
    if (reached == kReadyNone) {
        return "未就绪";
    }
    std::string text;
    for (const int bit : {kReadyWindow, kReadyPage, kReadyComposer, kReadyFileInput}) {
        if ((reached & bit) == 0) {
            continue;
        }
        text += (text.empty() ? std::string() : " + ") + ready_bit_name(bit);
    }
    return text;
}

// 缺哪一级（第一个未满足的位；全满足 → 空串）
inline std::string ready_first_missing(int need, int reached)
{
    for (const int bit : {kReadyWindow, kReadyPage, kReadyComposer, kReadyFileInput}) {
        if ((need & bit) != 0 && (reached & bit) == 0) {
            return ready_bit_name(bit);
        }
    }
    return {};
}

// 失败文案（**归因分离** · `W-D6`）：只说「哪一级没过 + 下一步」，**不得**把
// 「页面未就绪」说成「形态不支持（拖拽 / 粘贴入口）」—— 后者只在 L3 已通过、
// 仍取不到候选时才允许出现（见 `resolve_file_input_selector`）。
inline std::string ready_missing_text(int need, int reached)
{
    const std::string missing = ready_first_missing(need, reached);
    if (missing.empty()) {
        return {};
    }
    std::string text = "页面未就绪（缺 " + missing + "；已达：" + ready_reached_text(reached) + "）";
    if ((need & kReadyWindow) != 0 && (reached & kReadyWindow) == 0) {
        text += " —— 登录窗口尚未创建完成：稍后自动重试，或在界面「打开登录窗口」确认窗口可用";
    }
    else if ((need & kReadyPage) != 0 && (reached & kReadyPage) == 0) {
        text += " —— 页面仍在加载 / 当前页面不是目标站点：等待自动重试，或先在该站点窗口登录一次";
    }
    else if ((need & kReadyFileInput) != 0 && (reached & kReadyFileInput) == 0) {
        text += " —— 附件入口（input[type=file]）尚未挂载：**这不是**「该站点用拖拽 / 粘贴入口」；"
                "请确认窗口尺寸（大视口）与登录态后重试（本判据只读，不点击页面）";
    }
    else {
        text += " —— 输入框尚未出现：等待自动重试，或用 --web-dom-dump --provider <id> 取选择器";
    }
    return text;
}

} // namespace aiwrite::web
