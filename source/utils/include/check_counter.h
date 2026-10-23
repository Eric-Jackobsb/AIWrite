#pragma once

// ============================================================================
//  自检计数助手（去重专项 · 原 3 处逐字复制）
//
//  * 出处：`ai/provider_spec_selftest.cpp` / `ai/site_template.cpp`（自检段）/
//          `ai/official_template.cpp`（自检段）—— 三份一字不差
//  * 语义不变：逐项打印 `PASS/FAIL`，失败带原因；`passed` / `failed` 供调用方汇总
//  * 只依赖 <cstdio> / <string>，任何层都可 include（utils 为最低层，无反向依赖）
// ============================================================================

#include <cstdio>
#include <string>

namespace aiwrite {

struct CheckCounter {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& name, const std::string& detail = {})
    {
        if (condition) {
            ++passed;
            std::printf("   PASS  %s\n", name.c_str());
            return;
        }
        ++failed;
        std::printf("   FAIL  %s%s%s\n", name.c_str(), detail.empty() ? "" : "  ->  ",
                    detail.c_str());
    }
};

// ----------------------------------------------------------------------------
//  「✓ / ✗」风格自检 lambda（去重专项 · 原 7 处内联复制）
//  用法（与原内联式逐字等价）：
//      int passed = 0;
//      int failed = 0;
//      const auto check = make_selftest_check(passed, failed);
// ----------------------------------------------------------------------------
inline auto make_selftest_check(int& passed, int& failed)
{
    return [&passed, &failed](bool ok, const std::string& what) {
        if (ok) {
            ++passed;
            std::printf("  ✓ %s\n", what.c_str());
        }
        else {
            ++failed;
            std::printf("  ✗ %s\n", what.c_str());
        }
    };
}

}  // namespace aiwrite

