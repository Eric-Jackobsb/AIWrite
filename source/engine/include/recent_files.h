#pragma once

// ============================================================================
//  最近打开的工作流（设计 §11.1：~/.brain-ai/recent.json，上限 10 条）
//
//  * push_recent_file：去重置顶 + 截断到 10 条 + 立即落盘（文件不存在则忽略）
//  * 文件损坏/非法 JSON：按空列表处理并告警，不影响启动
//  * 路径比较用 weakly_canonical 归一化（同一文件的不同写法视为同一条）
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>

namespace aiwrite::engine {

struct RecentEntry {
    std::string path;
    std::string name;
    std::string opened_at; // "YYYY-MM-DD HH:MM:SS"
};

constexpr std::size_t kMaxRecentFiles = 10;

std::vector<RecentEntry> load_recent_files();
void                     push_recent_file(const std::string& path, const std::string& name);
void                     clear_recent_files();

// ---- M_patchAB §4（`PC-03` / `FEA-M2-02`）：最近列表**治理** ----
//  * 打开前**校验存在性**：失效项在菜单里灰显（不可点）
//  * `count_missing_recent()` / `prune_missing_recent()`：一键「清理失效条目」
//  * 校验是**只读**查询（不写盘）；清理才写盘（写失败如实记日志、返回 0 条）
struct RecentStatus {
    RecentEntry entry;
    bool        exists = false; // 文件当前是否存在（目录不算）
};

std::vector<RecentStatus> recent_with_status();
std::size_t               count_missing_recent();
std::size_t               prune_missing_recent();

} // namespace aiwrite::engine
