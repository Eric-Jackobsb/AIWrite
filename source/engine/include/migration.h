#pragma once

// ============================================================================
//  版本迁移框架（M_patchAB §4 · `PC-04` / `DBG-M6-02`）
//
//  目标：**跨版本文件不再只能「warn 后拒绝 / 忽略」** —— 给工作流 JSON 与 `config.toml`
//        一个**表驱动**的迁移入口，并且对「未来版本」**明确拒绝**（不猜、不静默降级，`I21`）。
//
//  三件事（全部**纯函数**，可离线断言）：
//    ① `migrate_workflow()`：按 `version` 字段决定「原样 / 逐级迁移 / 明确拒绝」；
//    ② `migrate_config()`：`config_version` 补齐 + 未来版本标记（拒绝被降级写回）；
//    ③ `migration_selftest()`：机制夹具（生产迁移表当前为**空** ⇒ 用夹具覆盖机制本身）。
//
//  判定口径（`workflow` · 与 `kWorkflowVersion = "1.0"` 对齐）：
//    * 缺 `version`（老文件）= 按 **1.0** 处理（迁移说明如实记一条）；
//    * 主版本 = 当前 ⇒ 原样通过（次版本差异不拦）；
//    * 主版本 < 当前 ⇒ 走迁移表（**表为空 = 无历史不兼容变更**，如实记一条「无需迁移」）；
//    * 主版本 > 当前 ⇒ **拒绝载入** + 可操作错误（「文件由更新版本写入 ⇒ 请升级 AIwrite」）。
// ============================================================================

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"   // `Config`（`aiwrite::Config`）—— 配置迁移的输入

namespace aiwrite::engine {

// 当前工作流格式版本（权威位 = `workflow_io.h` 的 `kWorkflowVersion`，此处只做迁移判定）
std::string currentWorkflowVersion();

// 一次工作流迁移的结果（`ok=false` ⇒ **拒绝载入**，`error` 给可操作原因）
struct WorkflowMigrationOutcome {
    bool                     ok       = true;
    bool                     migrated = false;  // 是否真的改动了文档
    std::string              from;              // 原版本（缺字段 = 空串）
    std::string              to;                // 目标版本（= 当前版本）
    std::vector<std::string> notes;             // 逐条说明（写日志 / 状态栏）
    std::string              error;             // 拒绝原因（ok=false 时非空）
};

// 迁移表：一个「旧版本 → 新版本」的步骤（生产表当前为空；机制由 `migration_selftest` 夹具覆盖）
struct WorkflowMigrationStep {
    std::string from_version;   // 需要迁移的版本（主版本号，如 "1"）
    std::string to_version;     // 迁移到的版本
    std::string note;           // 人类可读说明（进 notes）
    // 变换：就地改文档；返回 false ⇒ 该步失败（整体拒绝，error 取 `failure`）
    std::function<bool(nlohmann::json* document, std::string* failure)> apply;
};

// 用**指定**迁移表迁移（生产入口传 `kWorkflowMigrations`；自检夹具传自己的表）
WorkflowMigrationOutcome migrate_workflow_with(const std::vector<WorkflowMigrationStep>& steps,
                                               nlohmann::json* document);

// 生产入口（用内置迁移表）
WorkflowMigrationOutcome migrateWorkflow(nlohmann::json* document);

// ---------------------------------------------------------------------------
//  配置迁移（`config.toml`）**不在本文件**：它由 `utils/config.{h,cpp}` 实现
//  （`aiwrite::ConfigMigrationOutcome` / `aiwrite::migrate_config()`）——
//  理由：`utils` 是更底层模块（`engine` 依赖 utils，反向包含会造成分层倒挂）。
//  本文件的自检仍会调用它（`engine` → `utils` 方向合法），保证两套迁移一起被断言。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  离线自检（`api_probe --exec-selftest` 调用）；返回失败项数
// ---------------------------------------------------------------------------
int migrationSelftest(int* passed_out = nullptr);

} // namespace aiwrite::engine
