#pragma once

// ============================================================================
//  三类错误文案（M_patchC §4.4 · 不变量 `I21` / `I25` / `I26` · `PW-05`）
//  —— **纯函数**部分
//
//  原则（本文 §0.3 第③条「**归因必须可分辨**」）：
//    「未就绪 / 无入口 / 被占用」三类错误**不得混写** —— 混写正是真机实证的两类误判：
//      * 19:40:11.487「0 个 `input[type=file]`」⇒ 被写成「该站点可能是拖拽 / 粘贴入口」
//        （**状态问题**被说成**形态问题**，`G6`）；
//      * 19:40:01.271「窗口还没建好」⇒ 被写成「请先在界面「打开登录窗口」登录一次」
//        （**就绪问题**被说成**登录问题**，`G2`）。
//
//  统一形状（**可 grep**）：`[owner <归属键>] <类别>（站点 <origin>）：<细节> —— 下一步：<可操作动作>`
//    * 开头**永远是** owner 标签（`owner_tag` 永不为空）⇒ 日志里一眼看出「谁要的」；
//    * 类别 = 三选一，**互不重叠**（`web_error_kind_name`）；
//    * 结尾**必须**有可操作下一步（`I21`：报错必须可操作）。
//
//  本文件**纯函数**（无 Win32 / WebView2 / IO）⇒ `api_probe --selftest` 可断言三类可分辨。
// ============================================================================

#include <string>

#include "web_owner.h"

namespace aiwrite::web {

enum class WebErrorKind {
    NotReady,  // ① 页面未就绪（窗口级 / 页面级 / 交互态级）—— 可等待 / 复位重试
    NoEntry,   // ② 站点无上传入口（形态不支持）—— **仅**在 L3 已通过、仍无候选时才允许下此结论
    Preempted, // ③ 该站点窗口被占用（同 owner 重入 / 显式共享模式下的排队）
};

inline std::string web_error_kind_name(WebErrorKind kind)
{
    switch (kind) {
    case WebErrorKind::NotReady:  return "页面未就绪";
    case WebErrorKind::NoEntry:   return "站点无上传入口";
    case WebErrorKind::Preempted: return "窗口被占用";
    }
    return "?";
}

// 可操作下一步（**每类各一句，互不复制** —— 复制就等于混写）
inline std::string web_error_next_step(WebErrorKind kind)
{
    switch (kind) {
    case WebErrorKind::NotReady:
        return "下一步：等自动重试；若连续两次都未就绪，本通道会**关窗重开**再试一次（复位由窗口表决策，不动别的窗口）";
    case WebErrorKind::NoEntry:
        return "下一步：先确认窗口尺寸（大视口）与登录态后重试；若确实只有拖拽 / 粘贴入口，"
               "请在 providers.json 该条目填实测 web.attach_selector（本判据只读，不点击页面）";
    case WebErrorKind::Preempted:
        return "下一步：同一站点的窗口**串行执行**一次调用 —— 等上一次调用结束后重试；"
               "跨 owner 一律**取用自己的窗口**（不再关别人的窗口，I24）";
    }
    return "下一步：重试";
}

// 三类错误文案（**唯一**构造入口 —— 调用方不要再手拼前缀，避免混写）
inline std::string web_error_text(WebErrorKind kind, const WebOwner& owner,
                                  const std::string& site_key, const std::string& detail)
{
    std::string text = owner_tag(owner) + " " + web_error_kind_name(kind) + "（站点 " +
                       (site_key.empty() ? std::string("(未定)") : site_key) + "）";
    if (!detail.empty()) {
        text += "：" + detail;
    }
    text += " —— " + web_error_next_step(kind);
    return text;
}

} // namespace aiwrite::web
