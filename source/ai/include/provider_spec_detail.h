// provider_spec_detail.h —— Provider 配置表的**模块内**实现细节（不进公共 API）
//
// ARCH-Z1（2026-10-23）：`provider_spec.cpp`（1743 行）按职责拆成 5 个 TU 后，
// 解析层（provider_spec_parse.cpp）与加载层（provider_spec.cpp）之间**只留 3 个内部入口**；
// 公共 API 全部仍由 `provider_spec.h` 声明（本批未增删一条签名）。
//
//   职责分工（同 ai 层，禁止跨层包含）：
//     * provider_spec.cpp           加载 / 合并 / 缓存（merge_provider_specs · loadProviderSpecs · 快照）
//     * provider_spec_parse.cpp     JSON 读取 / 密钥扫描 / 允许字段 / 条目解析 / 收尾校验 / 兜底表
//     * provider_spec_web.cpp       网页版站点派生 · 不可用文案 · 登录态判定 · 探测适用性
//     * provider_spec_query.cpp     纯查询（模型/协议/默认值/resolve_*）
//     * provider_spec_selftest.cpp  离线断言（`--provider-selftest` / `--exec-selftest` 共用）
//
//  口径：解析层内部的 `parse_*` / `is_implemented_*` 等辅助函数**不在本头文件声明**
//        （仅同一 TU 使用；跨 TU 可见的只有下列 3 个入口），因此本头文件即「内部契约全文」。
#pragma once

#include "provider_spec.h"

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace aiwrite::ai::provider_detail {

// 表 schema 版本（`schema_version` 必须逐字匹配；不匹配 → 该层整层忽略 + error）
constexpr int kSchemaVersion = 1;

// 前缀判定（逐字节比较；空 prefix 恒真）。原先在 `provider_spec.cpp` 的匿名命名空间里，
// 拆分后有 3 个 TU 都要用 → 收成内联单一来源（CLN-05 的同一口径：勿复制）。
inline bool startsWith(const std::string& text, const std::string& prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// 中文顿号拼接（字段名 / Cookie 名 / 已实现清单的界面与警告文案共用）。
// 原先有两份逐字相同的实现（`joinNames` / `join_list`）→ 拆批合并为一份。
inline std::string joinCn(const std::vector<std::string>& items)
{
    std::string joined;
    for (const std::string& item : items) {
        if (!joined.empty()) {
            joined += "、";
        }
        joined += item;
    }
    return joined;
}

// ---- 解析层（provider_spec_parse.cpp）→ 加载层（provider_spec.cpp）----

// 把一层里的一个条目应用到 spec（已存在 → 字段级覆盖；不存在 → 新建）
// 返回 false = 致命错误（调用方跳过该条；其余条目与已有表不受影响）
bool applyEntry(const nlohmann::json& entry, const std::string& layer, ProviderSpec* out,
                SpecLoadReport* report);

// 收尾校验（全部层合并完之后跑：必填 / 协议 / adapter / 端点 / 视觉声明）
void validateFinal(std::vector<ProviderSpec>* items, SpecLoadReport* report);

// 最小兜底表（C++ 内联的唯一例外，2 条；仅 ①〜② 都缺失时使用）
std::vector<ProviderSpec> fallbackSpecs();

} // namespace aiwrite::ai::provider_detail
