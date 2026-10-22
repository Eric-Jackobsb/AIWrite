#pragma once

// ============================================================================
//  用户自建**官方 API 提供商**条目的模板与落盘（自定义 AI 收口 · 冻结前最后一个功能）
//
//  用途：界面「新建 API 提供商…」→ 填一份（可**照抄内置官方条目**）模板 → 写进
//        `~/.brain-ai/providers.d/<id>.json`（**必须带 `schema_version` 信封**）
//        → 「重新加载配置表」→ 提供商下拉**立即可用**（无需重启）
//
//  设计约束（沿用 provider_spec.h / site_template.h 的口径）：
//   * 生成 / 校验 / 落盘是**纯函数 + 单文件写**：无网络、无 GUI 即可断言
//     （`api_probe --official-assert`；与 `--site-assert` 同风格）
//   * 只生成**解析器白名单内**的字段（未知字段会被解析器记「警告」）
//   * **不写密钥本体**：只写 `env_names` / `key_ref_default` 的**引用名**；
//     Key 由 `utils/credential`（Windows DPAPI）管理，界面填一次即入库
//   * id 校验与原子落盘**复用 `site_template`**（`providerIdError` / `writeUserProviderFile`
//     —— providers.d 用户条目的**唯一**实现，站点与官方 API 共用一套，避免重复）
//   * 已存在同名文件 ⇒ **默认拒绝写**（要覆盖必须显式 `overwrite=true`）——守 `R8`
// ============================================================================

#include <filesystem>
#include <string>

#include "provider_spec.h"

namespace aiwrite::ai {

// --------------------------------------------------------------------- 模板 --

struct OfficialProviderTemplate {
    std::string id;            // 条目 id（= 文件名；须过 `officialIdError`）
    std::string display;       // 界面显示名（如「我的中转站」）
    std::string protocol;      // openai（默认）| anthropic | gemini
    std::string api_base;      // API 地址（完整前缀，含 /v1 等；必填）
    std::string chat_path;     // 聊天路径（留空 = 按 protocol 默认）
    std::string auth_style;    // 认证方式（留空 = 按 protocol 默认）
    std::string models;        // 模型清单（逗号分隔；空 = 模型自由填写）
    std::string vision_model;  // 视觉默认模型（可空）
    bool        vision      = false; // capabilities.vision
    bool        seed        = true;  // capabilities.seed
    bool        system_role = true;  // capabilities.system_role
    std::string env_name;    // 环境变量名（可空；Key 也可直接入凭据库）
    std::string key_ref;     // 凭据库引用名（留空 = brain-ai/<id>）
    std::string notes;       // 写进条目 `notes`（如实标注「是否实测」）
};

// 内置最小模板（不依赖磁盘表；OpenAI 兼容，api_base 留空待填）
OfficialProviderTemplate officialTemplateDefault();

// 从表内条目**照抄**模板（R18：照抄也能建出提供商）
//  * 源条目是 `kind=official` ⇒ 身份 / 地址 / 模型 / 能力一起抄（改 id 即可用）
//  * `spec` 为空 / 非 `kind=official` ⇒ 退回 `officialTemplateDefault()`
OfficialProviderTemplate officialTemplateFromSpec(const ProviderSpec* spec);

// --------------------------------------------------------------- 校验 / 落盘 --

// 提供商 id 合法性（**可操作**错误文案；空串 = 合法）
//  * 与站点同一规则（`providerIdError(id, "提供商")`）：`[A-Za-z0-9._-]`、长度 1〜64、
//    首字符须为字母或数字（防路径穿越：`/` `\` `..` 一律拒）
std::string officialIdError(const std::string& id);

// 模板整体校验（空串 = 可写盘）：id + api_base（必填且 http/https）+ protocol 必须是
// 本版本已实现的官方协议（openai / anthropic / gemini）
std::string officialTemplateError(const OfficialProviderTemplate& tpl);

// 生成的**用户层文件全文**（带 `schema_version` 信封；纯函数 ⇒ 离线断言）
std::string officialFileJson(const OfficialProviderTemplate& tpl);

// 写 `dir/<id>.json`（目录不存在则创建；原子落盘 = 先写 `.tmp` 再改名）
//  * 同名文件已存在且 `overwrite=false` ⇒ 失败 + 可操作文案
//  * `written` 可为 nullptr；失败时给出 `error`
bool writeUserOfficialFile(const OfficialProviderTemplate& tpl, const std::filesystem::path& dir,
                           bool overwrite, std::string* error,
                           std::filesystem::path* written = nullptr);

// ---- 自检（离线断言；返回失败项数；`api_probe --official-assert` 调用）----
// 覆盖：id 合法性（含路径穿越）/ api_base 必填与 http(s) / 协议必须是已实现官方协议 /
//       生成的 JSON 带信封且零错误合并 / 坏 id 不落盘 / 已存在文件默认拒写 / 覆盖写成功 /
//       转义与中文往返 / 照抄内置条目
int officialTemplateSelftest(int* passed_out = nullptr);

} // namespace aiwrite::ai
