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

} // namespace aiwrite::engine
