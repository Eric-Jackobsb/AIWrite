#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

// 日志模块（设计文档 12 节）
//   * 控制台 + ~/.brain-ai/logs/app.log（单文件 10MB x 5）双 sink
//   * 同时维护内存环形缓冲，供 Console 面板（M4）使用
//   * 格式：[2026-09-21 10:30:45.123] [INFO] 文本
namespace aiwrite::log {

enum class Level { Info, Warn, Error };

struct Entry {
    std::chrono::system_clock::time_point time;
    Level level = Level::Info;
    std::string text; // 已格式化的一整行
};

// 初始化日志系统；attach_console = true 时在 GUI 程序里额外分配控制台窗口
void init(bool attach_console = false);
void shutdown();

void info(const std::string& message);
void warn(const std::string& message);
void error(const std::string& message);

// 内存环形缓冲（设计文档 12.4：上限 10000 条）
constexpr std::size_t kMaxEntries = 10000;

const std::deque<Entry>& entries();
// use_level_filter = false 时忽略 min_level（既显示全部级别）
std::vector<Entry> filter(Level min_level, bool use_level_filter, const std::string& search);
void clear_entries();

// 日志文件路径（用于 UI 展示）
std::string file_path_string();

} // namespace aiwrite::log
