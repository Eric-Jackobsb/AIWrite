#pragma once

// ============================================================================
//  用户自建**网页版站点**条目的模板与落盘（M_patchAB_rest §3.5 / `PB2-24` 要点①）
//
//  用途：界面「新建网页版站点…」→ 填一份**照抄内置条目**的模板 → 写进
//        `~/.brain-ai/providers.d/<id>.json`（**必须带 `schema_version` 信封**）
//        → 「重新加载配置表」→ 提供商下拉**立即可用**（无需重启）
//
//  设计约束（沿用 provider_spec.h 的口径）
//   * 生成 / 校验 / 落盘是**纯函数 + 单文件写**：无网络、无 GUI 即可断言
//     （`api_probe --site-assert`；与 `provider_spec_selftest` 同风格）
//   * 只生成**解析器白名单内**的字段（未知字段会被解析器记「警告」；
//     白名单见 provider_spec.cpp 的 `allowed_entry_fields()` / `allowed_web_fields()`）
//   * **不写密钥本体**：生成的 `token_expr` 是页面内取数表达式、`cookie_names` 是**名字清单**
//     （解析器的明文密钥检测对这两个键名豁免）
//   * 生成文件的 `web.adapter` **恒为 `dom`**（内建适配器 `builtin:*` 不可被用户文件复刻；
//     从 `deepseek-web` 这类内建条目「照抄」时只抄站点身份，不抄内建专属字段 —— 见 §8 附录 D）
//   * 已存在同名文件 ⇒ **默认拒绝写**（要覆盖必须显式 `overwrite=true`）——
//     守 `R8`：坏表绝不静默替换
// ============================================================================

#include <filesystem>
#include <string>
#include <vector>

#include "provider_spec.h"

namespace aiwrite::ai {

// --------------------------------------------------------------------- 模板 --

struct WebSiteTemplate {
    std::string id;             // 条目 id（= 文件名；须过 `site_id_error()`）
    std::string display;        // 界面显示名（如「Kimi（网页版·自建）」）
    std::string login_url;      // 登录页（**站点身份唯一来源**，D-26）
    std::string window_title;   // 登录窗口标题（留空 ⇒ 按 display 生成默认）
    std::string input_selector; // DOM：输入框选择器（留空 ⇒ 生成未就绪，登录仍可用）
    std::string send_kind;      // DOM：key | click
    std::string send_value;     // DOM：键名（Enter）或按钮选择器
    std::string answer_selector;// DOM：回答容器选择器
    std::string done_kind;      // DOM：selector_gone | selector_present（可空）
    std::string done_selector;  // DOM：完成判据选择器
    std::string cookie_names;   // 该站点登录 Cookie 名（**逗号分隔**；可空 = 按 origin 判登录）
    std::string token_expr;     // 页面内取 token 表达式（纯 DOM 站点一般留空）
    int         answer_poll_ms   = 500;  // DOM：轮询间隔
    int         answer_max_polls = 120;  // DOM：最大轮询次数
    std::string notes;          // 写进条目 `notes`（如实标注「是否实测」）
};

// 内置最小模板（不依赖磁盘表；`登录型` 形态：只有站点身份，选择器留给用户按 §3.4 实测填）
WebSiteTemplate siteTemplateDefault();

// 从表内条目**照抄**模板（R18：照抄也能建出站点）
//  * 源条目是 `web.adapter=dom` ⇒ 连选择器一起抄（改 id 即可用）
//  * 源条目是内建站点（`builtin:*` / 空 adapter）⇒ 只抄站点身份（登录页 / 标题 / Cookie 名 /
//    取数表达式），**不抄**内建专属字段 ⇒ 生成「登录型」条目（守 §8 附录 D 的边界）
//  * `spec` 为空 / 非 `kind=web` ⇒ 退回 `site_template_default()`
WebSiteTemplate siteTemplateFromSpec(const ProviderSpec* spec);

// ----------------------------------------------------------------- 校验 / 落盘 --

// 站点 id 合法性（**可操作**错误文案；空串 = 合法）
//  * 允许 `[A-Za-z0-9._-]`，长度 1〜64，首字符须为字母或数字（防路径穿越：`/` `\` `..` 一律拒）
std::string siteIdError(const std::string& id);

// **共享**的条目 id 合法性（站点 / 官方 API 提供商共用，自定义 AI 收口批抽出）
//  * 与 `siteIdError` 同一规则、同一文案：`siteIdError(id)` ≡ `providerIdError(id, "站点")`
//  * `kind_label` = 「站点」/「提供商」（决定报错文案里的前缀）
std::string providerIdError(const std::string& id, const std::string& kind_label);


// 模板整体校验（空串 = 可写盘）：id + 登录页（站点身份的唯一来源）
std::string siteTemplateError(const WebSiteTemplate& tpl);

// 模板**待实测**清单（空串 = 生成字段齐全）：缺 `input_selector` / `send` / `answer_selector`
//  * 缺失 ⇒ 条目仍是**合法的登录型站点**（登录 / 登录态可用；生成在运行期**明确报错**，PB2-26）
std::string siteTemplatePending(const WebSiteTemplate& tpl);

// 生成的**用户层文件全文**（带 `schema_version` 信封；纯函数 ⇒ 离线断言）
std::string siteFileJson(const WebSiteTemplate& tpl);

// 写 `dir/<id>.json`（目录不存在则创建；原子落盘 = 先写 `.tmp` 再改名）
//  * 同名文件已存在且 `overwrite=false` ⇒ 失败 + 可操作文案
//  * `written` 可为 nullptr；失败时给出 `error`
bool write_user_site_file(const WebSiteTemplate& tpl, const std::filesystem::path& dir,
                          bool overwrite, std::string* error,
                          std::filesystem::path* written = nullptr);

// **共享**底层写盘：`dir/<id>.json`（站点 / 官方 API 提供商共用，自定义 AI 收口批抽出）
//  * **不校验** id / 内容 —— 调用方须先过各自的校验（`siteTemplateError` / `officialTemplateError`）
//  * 原子落盘 / 拒覆盖策略 / 文案与 `write_user_site_file` 完全一致
bool writeUserProviderFile(const std::string& id, const std::string& json,
                           const std::filesystem::path& dir, bool overwrite,
                           std::string* error, std::filesystem::path* written = nullptr);


// 逗号分隔串 ↔ Cookie 名清单（去空 / 去重 / 保序；界面与 JSON 共用同一口径）
std::vector<std::string> splitNames(const std::string& text);
std::string              joinNames(const std::vector<std::string>& names);

// ---- 自检（离线断言；返回失败项数；`api_probe --site-assert` 调用）----
// 覆盖：id 合法性（含路径穿越）/ 登录页必填 / 生成的 JSON 带信封 / 未知字段零告警口径 /
//       DOM 字段嵌套形状（`send` / `done_when`）/ 缺信封 ⇒ 整层失效（R8 回归护栏）/
//       坏 id 不落盘 / 已存在文件默认拒写 / 覆盖写成功 / 转义与中文往返
int siteTemplateSelftest(int* passed_out = nullptr);

} // namespace aiwrite::ai
