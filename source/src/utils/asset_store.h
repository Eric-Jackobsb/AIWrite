#pragma once

// ============================================================================
//  统一资源目录（P7a-04；M7 第二轮 P7-a）
//
//  * 资源根：`~/.brain-ai/assets/images/`（收在数据目录内；与工作流文件解耦）
//  * 命名：**内容寻址** `<摘要>.<ext>` —— 同内容只存一份（天然去重）
//  * 工作流里存**令牌** `aiwrite-asset:<摘要>` —— 换目录 / 换机后仍能取到图片（P7a-05）
//  * 摘要算法：复用本仓库既有的 **SHA3-256**（`utils/crypto.h`，无新增依赖）。
//    ⚠️ 计划文本 `Q1②` 写的 `<sha1>` 只是「内容摘要」的占位写法，此处用现成实现落地。
//  * 兼容（不变量 `I19`）：**旧绝对路径照旧可用** —— `to_local_path()` 原样返回；
//    迁移必须**显式**（面板「迁移到资源目录」按钮），**不做静默改写**（P7a-06）。
//  * 缺失（P7a-07）：令牌解析不到文件时，文案含**资源目录完整路径 + 预期文件名 + 下一步**。
// ============================================================================

#include <filesystem>
#include <string>
#include <vector>

namespace aiwrite::asset {

inline constexpr const char* kTokenPrefix = "aiwrite-asset:";

// 资源根（= paths::assets_images_dir()）
std::filesystem::path images_root();

// 值是否为资源令牌（前缀 + 64 位十六进制摘要）
bool is_token(const std::string& value);
// 令牌 → 摘要（非令牌 / 格式不合法 → 空）
std::string token_digest(const std::string& value);
// 摘要 → 令牌
std::string make_token(const std::string& digest);

// 文件内容摘要（SHA3-256 小写十六进制；读不到 → 空 + error）
std::string digest_of_file(const std::string& path, std::string* error);

// 规范扩展名（**内容嗅探优先**：内容 WebP 却叫 .png → `.webp`；认不出来回退原扩展名）
std::string canonical_extension(const std::string& path);

// 归档：把外部图片复制进资源根，返回令牌（同内容已存在 → 直接返回既有令牌，不重复存）
std::string import_file(const std::string& path, std::string* error);

// 令牌 → 资源根内的绝对路径
//  * 找到 → 返回路径，*missing = false
//  * 没找到 → 返回空字符串，*missing = true，*error 写出可操作文案（P7a-07）
std::string resolve_token(const std::string& token, bool* missing, std::string* error);

// 值 → 本地可读路径
//  * 令牌 → 解析后的绝对路径（缺失 → 空 + *missing = true + *error）
//  * 普通路径 / 空值 → **原样返回**（旧工作流语义不变，`I19`）
std::string to_local_path(const std::string& value, bool* missing, std::string* error);
std::string to_local_path(const std::string& value);
// 多值：逐项转换（保留顺序；空项忽略；缺失项 → 空串占位并记入 *problems）
std::vector<std::string> to_local_paths(const std::vector<std::string>& values,
                                        std::vector<std::string>* problems);

// P7a-06：是否需要迁移（非空且不是令牌 = 外部路径）
bool needs_migration(const std::string& value);
// P7a-06：迁移提示文案（纯函数；Console 与面板共用同一句话）
std::string migration_hint(const std::string& value);

} // namespace aiwrite::asset