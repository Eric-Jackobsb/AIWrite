// string_util.h —— 字符串通用工具（单一来源 · 各模块共用，勿复制）
//
// CLN-05（先导批）：把散落在各 TU 的重复辅助函数收敛到此单一来源：
//   * find_ci / ce_find_ci（node_library / composite_editor）   → findCi
//   * trim ×5（deepseek_web_client / http_net / provider_spec / site_template / graph）
//   * now_string ×3（autosave / recent_files / session_store）  → nowString
//   * lower ×2（http_net / provider_spec）                      → toLower
//   * narrow ×2（diagnostics_report / webview2_runtime）        → narrow
// 命名统一为驼峰（全库驼峰化 CLN-06 的先导样板）。
#pragma once

#include <cstddef>
#include <string>

namespace aiwrite::utils {

// 大小写不敏感子串匹配（ASCII 折叠；中文原样字节比较）。
// 空 needle 返回 0；needle 长于 haystack 返回 std::string::npos。
std::size_t findCi(const std::string& haystack, const std::string& needle);

// 去首尾空白（" \t\r\n"）；全空返回空串。
std::string trim(const std::string& text);

// 本地时间戳 "%Y-%m-%d %H:%M:%S"（localtime_s + strftime）。
std::string nowString();

// 整串转小写（按 unsigned char 调用 std::tolower，规避负值 UB）。
std::string toLower(std::string text);

// std::wstring → UTF-8（空串 / 转换失败返回空串）。需 <windows.h>。
std::string narrow(const std::wstring& wide);

} // namespace aiwrite::utils
