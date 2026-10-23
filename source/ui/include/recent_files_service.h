#pragma once

// ============================================================================
//  最近打开列表服务（ARCH-A1：应用级服务收口 → AppContext · 六个服务之一）
//
//  架构落点：ARCHITECTURE.md §1.8 `AppContext` / §1.8.9 `WorkflowFile`（持有本服务）
//   * 薄封装 `engine/recent_files.h` 纯函数（load/push/clear/status/prune），
//     供「文件 → 最近打开」菜单与保存 / 打开回调使用；语义与拆出前逐字一致
//   * 文件名避让：ui 层不再新增 `recent_files.h`（与 engine 层同名头冲突），
//     故命名为 `recent_files_service.h`。
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>

#include "recent_files.h" // engine::RecentEntry / RecentStatus

namespace aiwrite::ui {

class RecentFiles {
public:
    std::vector<engine::RecentEntry> load() const; // ~/.brain-ai/recent.json（上限 10 条）
    void push(const std::string& path, const std::string& name); // 去重置顶 + 立即落盘
    void clear() const;

    // ---- PC-03 / FEA-M2-02：最近列表治理（校验存在性 / 一键清理失效条目）----
    std::vector<engine::RecentStatus> with_status() const; // 打开前校验存在性（灰显判据）
    std::size_t count_missing() const;                     // 失效条目数
    std::size_t prune_missing() const;                     // 清理失效条目（写盘）
};

} // namespace aiwrite::ui
