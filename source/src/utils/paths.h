#pragma once

#include <filesystem>
#include <string>

// 路径工具：统一管理 ~/.brain-ai 数据目录（设计文档 11.1 节）
namespace aiwrite::paths {

// 用户主目录（%USERPROFILE%）
const std::filesystem::path& home();

// 展开以 "~" 开头的路径；其他路径原样返回
std::filesystem::path expand(const std::string& value);

// 数据根目录 ~/.brain-ai
const std::filesystem::path& data_root();
const std::filesystem::path& outputs_dir();       // ~/.brain-ai/outputs
const std::filesystem::path& workflows_dir();     // ~/.brain-ai/workflows
const std::filesystem::path& snapshots_dir();     // ~/.brain-ai/snapshots
const std::filesystem::path& logs_dir();          // ~/.brain-ai/logs
const std::filesystem::path& config_file();       // ~/.brain-ai/config.toml
const std::filesystem::path& app_log_file();      // ~/.brain-ai/logs/app.log
const std::filesystem::path& workflow_log_file(); // ~/.brain-ai/logs/workflow.log
const std::filesystem::path& recent_file();       // ~/.brain-ai/recent.json
const std::filesystem::path& webview2_profile();  // ~/.brain-ai/webview2

// 可执行文件所在目录
const std::filesystem::path& exe_dir();
std::filesystem::path assets_dir();               // <exe_dir>/assets

// 创建全部数据目录；返回失败个数（0 = 全部就绪）
int ensure_data_dirs();

} // namespace aiwrite::paths
