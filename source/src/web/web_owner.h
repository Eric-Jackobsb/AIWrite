#pragma once

// ============================================================================
//  WebOwner：网页通道的「归属」标识（M_patchC §4.1 · 不变量 I24 / I26 · PW-01）
//
//  * 目的：让「窗口 / 脚本槽 / 上传槽 / 网络回执 / 答案基线」都能回答
//    「**这是哪个运行、哪个节点要的**」—— 真机实证（2026-10-06 · 三次运行）
//    的错误根因正是「没有归属表，只有按站点键控的全进程单例」。
//  * 本文件**纯数据 + 纯函数**：不含 Win32 / WebView2 / 网络 / 文件依赖，
//    因此可被 `api_probe --selftest` 离线断言。
//  * 归属语义（随 `PW-02` 窗口表落地）：
//      - 同 owner + 同站点 ⇒ **复用**同一窗口；
//      - 同站点、不同 owner ⇒ **各自独立窗口 / profile**（决策 `D-33②` · 真并行）；
//      - 跨 owner 一律**禁止隐式抢占**（`I24`）。
//    `PW-01` 阶段只做「贯穿 + 日志 + 断言」，**不改变**任何窗口行为。
// ============================================================================

#include <string>

namespace aiwrite::web {

// 一次「节点级网页调用」的归属
struct WebOwner {
    std::string run_id;   // 运行会话 id（同一次「运行」内相同；空 = 未标注）
    std::string node_id;  // 节点 id（例：n7；空 = 未标注）
    std::string site_key; // 站点键（origin；空 = 未定）
};

// 同一归属？（比 run_id + node_id —— 同一节点只服务一个站点，site_key 不参与）
inline bool owner_same(const WebOwner& a, const WebOwner& b)
{
    return a.run_id == b.run_id && a.node_id == b.node_id;
}

// 归属键（`run#node`）；两者都空 → 空串（= 未标注）
inline std::string owner_key(const WebOwner& owner)
{
    if (owner.run_id.empty() && owner.node_id.empty()) {
        return {};
    }
    return owner.run_id + "#" + owner.node_id;
}

// 日志前缀（**永不为空** —— 便于 grep「谁动的手」）
inline std::string owner_tag(const WebOwner& owner)
{
    const std::string key = owner_key(owner);
    return key.empty() ? std::string("[owner 未标注]") : ("[owner " + key + "]");
}

// 归属是否已标注（`PW-02` 起：未标注 ⇒ 运行期警告；今天只用于日志 / 断言）
inline bool owner_known(const WebOwner& owner) { return !owner_key(owner).empty(); }

} // namespace aiwrite::web
