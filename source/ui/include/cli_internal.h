#pragma once

// ============================================================================
//  CLI / 自检内部共享声明（CLN-14 拆分）
//
//  * 仅供 main.cpp / cli_selftest.cpp / cli_diag.cpp 内部使用；不进公共头。
//  * 原 main.cpp 匿名块（L53 `namespace {` … L1756 `} // namespace`）内的
//    CLI 分派函数与通用辅助，拆分后提升为**全局命名空间**（跨 TU 共享），
//    与 aiwrite::ai / aiwrite::engine 等模块的同名函数（如 join_names）
//    命名空间不同，不冲突。
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>

#include "provider_spec.h" // aiwrite::ai::ProviderSpecs

// ---- 通用辅助（定义于 cli_diag.cpp / cli_selftest.cpp）----
std::string reportTimestamp();
bool readTextFile(const std::string& path, std::string* out, std::string* error);
bool writeTextFile(const std::string& path, const std::string& text, std::string* error);
std::size_t utf8_length(const std::string& text);
std::string utf8_prefix(const std::string& text, std::size_t max_chars);
std::string joinNames(const std::vector<std::string>& items);
void printUsage();

// ---- CLI 自检 / 诊断分派（定义于 cli_selftest.cpp / cli_diag.cpp）----
int officialGenerateSelftest(const std::string& provider_id);
int runSelftest(bool use_web, const std::string& provider_id);
int webChatSelftest(const std::string& prompt);
int pipeSelftest(int timeout_ms);
int webSessionSelftest();
int vlm_selftest(const std::string& image_path, const std::string& api_base,
                 const std::string& model, const std::string& key_ref);
int webSessionList(const std::string& json_path);
void printSpecList(const aiwrite::ai::ProviderSpecs& specs);
int providerDump();
int provider_selftest(const std::string& provider_id, const std::string& api_base,
                      const std::string& model, const std::string& key_ref);
