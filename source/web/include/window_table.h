#pragma once

// ============================================================================
//  窗口表 + 复位计划（M_patchC §4.1 / §4.4 · 不变量 `I24` · `PW-02` / `PW-06` / `PW-08`）
//  —— **纯数据 + 纯函数**部分
//
//  背景（真机 2026-10-06 · 三次运行 · `M_patchC` §2 / §3）：
//    网页通道**没有「节点 / 运行 ↔ 窗口」的关系表** —— 全进程只有一套「按站点键控」的
//    全局单例，于是：同站点多节点互相认领同一页面、跨站点互相关窗
//    （`n3(deepseek)` 一开口就把 `n7(doubao)` 的窗口关掉：日志 19:39:12
//    「切换站点…串行复用：先关闭旧窗口」）。
//
//  本表回答三个问题（都可在 `api_probe --selftest` 离线断言，**不开窗口、不联网**）：
//    ① 取窗：给定 `(owner, site_key)` ⇒ **复用** 还是 **新建**（`WindowTable::plan`）；
//    ② 收尾：关窗顺序 / 某 owner 名下有哪些窗口（`keys_in_close_order` / `keys_of_owner`）；
//    ③ 复位：`D-35③`（**首次 + 失败时** · 单次重试）⇒ 该不该复位、复位到哪一级（`plan_reset`）。
//
//  语义（`D-33②` 拍板 · **真并行**）：
//    * 同 `owner` + 同站点 ⇒ **复用**（今天的「幂等快路」保留，语义不变）；
//    * **同站点、不同 owner ⇒ 各自独立窗口**（不再排队、不再互相关窗）；
//    * 跨站点 ⇒ **不关旧窗**：换站点只是**取用该站点自己的**窗口；
//    * 「谁调用谁关」**一律禁止**（`I24`：关窗只能由**窗口表**决策，或显式收尾）。
//
//  真实的窗口线程 / controller / 槽位在 `web/webview_host.cpp`（与本表**同一决策口径**）：
//  该文件用本表做「复用 / 新建」判定，并把每条窗口的键登记进本表。
// ============================================================================

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "web_owner.h"

namespace aiwrite::web {

// 窗口键：`(owner, 站点)` 二元组 —— **不是**只按站点（真机错配的根因，`G1`）
//  * 例：`run1#n7@https://www.doubao.com` / `界面#登录窗口@https://chat.deepseek.com`
//  * 未标注 owner ⇒ `未标注@<站点>`（CLI / 自检等没有节点上下文的调用点仍可安全取窗）
inline std::string window_key(const WebOwner& owner, const std::string& site_key)
{
    const std::string ok = owner_key(owner);
    return (ok.empty() ? std::string("未标注") : ok) + "@" +
           (site_key.empty() ? std::string("(空站点)") : site_key);
}

// 取窗决策（**只有两种**：复用 / 新建。没有「关掉别人的窗口再开」这一项 —— 那就是 `I24` 要禁的隐式抢占）
enum class WindowPlan {
    Reuse,  // 该 `(owner, 站点)` 已有窗口 ⇒ 复用它
    Create, // 没有 ⇒ 新开**该 owner 自己的**窗口（同站点不同 owner 各开各的）
};

inline std::string window_plan_name(WindowPlan plan)
{
    switch (plan) {
    case WindowPlan::Reuse:  return "复用";
    case WindowPlan::Create: return "新建";
    }
    return "?";
}

// 窗口表条目（诊断 / 断言用）
struct WindowEntry {
    std::string   key;       // `window_key`：`owner#node@站点`
    std::string   owner_key; // `owner_key(owner)`（空 = 未标注）
    std::string   site_key;  // origin
    std::uint64_t seq = 0;   // 创建序号（越大越新 ⇒ 收尾时越先关）
};

// 窗口表（**决策模型**）：只维护「谁开着哪个窗口」，不碰线程 / COM / Win32
class WindowTable {
public:
    // ① 取窗：**唯一**的「复用 / 新建」判据
    WindowPlan plan(const WebOwner& owner, const std::string& site_key) const
    {
        return find(owner, site_key) == nullptr ? WindowPlan::Create : WindowPlan::Reuse;
    }

    // 登记「该窗口已开」（返回 true = 新插入；false = 已存在，无副作用）
    bool note_opened(const WebOwner& owner, const std::string& site_key)
    {
        const std::string key = window_key(owner, site_key);
        if (entries_.find(key) != entries_.end()) {
            return false;
        }
        WindowEntry entry;
        entry.key       = key;
        entry.owner_key = owner_key(owner);
        entry.site_key  = site_key;
        entry.seq       = ++seq_;
        entries_.emplace(key, entry);
        return true;
    }

    bool note_closed(const WebOwner& owner, const std::string& site_key)
    {
        return entries_.erase(window_key(owner, site_key)) != 0;
    }

    const WindowEntry* find(const WebOwner& owner, const std::string& site_key) const
    {
        const auto it = entries_.find(window_key(owner, site_key));
        return it == entries_.end() ? nullptr : &it->second;
    }

    bool has(const WebOwner& owner, const std::string& site_key) const
    {
        return find(owner, site_key) != nullptr;
    }

    std::size_t size() const { return entries_.size(); }

    // ② 某 owner 名下的窗口键（一个 owner 可能在各站点各有一个窗口）
    std::vector<std::string> keys_of_owner(const WebOwner& owner) const
    {
        const std::string        ok = owner_key(owner);
        std::vector<std::string> out;
        for (const auto& kv : entries_) {
            if (kv.second.owner_key == ok) {
                out.push_back(kv.first);
            }
        }
        return out;
    }

    // ② 收尾顺序：**后开的先关**（LIFO）—— 与「谁调用谁关」相对：收尾由表决定
    std::vector<std::string> keys_in_close_order() const
    {
        std::vector<const WindowEntry*> ordered;
        ordered.reserve(entries_.size());
        for (const auto& kv : entries_) {
            ordered.push_back(&kv.second);
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const WindowEntry* a, const WindowEntry* b) { return a->seq > b->seq; });
        std::vector<std::string> out;
        out.reserve(ordered.size());
        for (const WindowEntry* entry : ordered) {
            out.push_back(entry->key);
        }
        return out;
    }

    // 可 grep 的一行（日志 / 诊断）：`2 个窗口 ｜ #1 run1#n7@https://… ｜ #2 界面#登录窗口@https://…`
    std::string report() const
    {
        std::vector<const WindowEntry*> ordered;
        ordered.reserve(entries_.size());
        for (const auto& kv : entries_) {
            ordered.push_back(&kv.second);
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const WindowEntry* a, const WindowEntry* b) { return a->seq < b->seq; });
        std::string text = std::to_string(ordered.size()) + " 个窗口";
        for (const WindowEntry* entry : ordered) {
            text += " ｜ #" + std::to_string(entry->seq) + " " + entry->key;
        }
        return text;
    }

    void clear() { entries_.clear(); }

private:
    std::map<std::string, WindowEntry> entries_;
    std::uint64_t                      seq_ = 0;
};

// ---- `PW-08`（`D-35③`）：**运行级复位**计划（纯决策）----
//
//  拍板口径 = **首次 + 失败时**（单次重试、有上限）：
//    * 首次：本轮第一次用到该站点 ⇒ 复位到「新会话页」并等 L2 / L3；
//    * 失败：上一次使用以「未就绪」失败 ⇒ **再复位一次**，仍失败则按归因报错（不空转）。
//
//  复位动作（`D-35③` 修订 2026-10-17 —— 实现事实变化，见 `M_patchC` §4.4）：
//    干净 profile 上确定性复现：关窗后**同一毫秒**重开 ⇒ 第二个 `CreateCoreWebView2Controller`
//    同步返回 `0x802A000C`（浏览器侧瞬时竞态）⇒ 「关窗重开」从首选降级为**兜底**；
//    首选改为 **同一控制器 `navigate_and_wait` 回新会话页**（§4.4 预留的 `navigate_and_wait()`
//    原语于 2026-10-17 落地，`login_url` 天然就是新会话页）；
//    交互态级仍只做 `wait_ready(L1→L2→L3)` —— 不重开、不动用户会话。
//  **不允许**：在用户可见的共享窗口里静默改导航（`location.href=…` 式）。
enum class ResetAction {
    None,              // 不复位
    NavigateToSession, // 窗口级：同一控制器「导航回新会话页」（login_url 即新会话页；首选）
    CloseAndReopen,    // 兜底：关窗重开（仅当导航不可行；需带 `kReopenSettleMs` 避让竞态）
    WaitReady,         // 交互态级：不重开、只等就绪
};

inline constexpr int kMaxResetsPerWindow = 1; // 单次重试（重试用尽 ⇒ 按归因报错）

inline std::string reset_action_name(ResetAction action)
{
    switch (action) {
    case ResetAction::None:              return "不复位";
    case ResetAction::NavigateToSession: return "导航回新会话页（同控制器）";
    case ResetAction::CloseAndReopen:    return "关窗重开（兜底）";
    case ResetAction::WaitReady:         return "等就绪（交互态级）";
    }
    return "?";
}

//  * opened_now      = 本次调用**新建**了窗口
//    （新开窗口 = 已在 `login_url` 新会话页 ⇒ **无需**再复位，避免重复导航）
//  * reused_external = 复用的是**早于本轮**存在的窗口（用户手开 / 上一轮遗留）⇒ 首次使用要复位
//  * failed_before   = 上一次使用以「未就绪」失败
//  * resets_done     = 本窗口已复位次数（上限 `kMaxResetsPerWindow`）
inline ResetAction plan_reset(bool opened_now, bool reused_external, bool failed_before,
                              int resets_done)
{
    if (opened_now) {
        return ResetAction::None;
    }
    if (reused_external) {
        return ResetAction::NavigateToSession;
    }
    if (failed_before) {
        return resets_done < kMaxResetsPerWindow ? ResetAction::NavigateToSession : ResetAction::None;
    }
    return ResetAction::None;
}

} // namespace aiwrite::web
