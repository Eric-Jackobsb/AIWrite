#include "migration.h"

#include "log.h"
#include "workflow_io.h"   // kWorkflowVersion（权威位）

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace aiwrite::engine {

namespace {

// 取主版本号（`"1.0"` → 1；空 / 非数字 → -1）
int major_of(const std::string& version)
{
    if (version.empty()) {
        return -1;
    }
    std::size_t end = 0;
    while (end < version.size() && version[end] >= '0' && version[end] <= '9') {
        ++end;
    }
    if (end == 0) {
        return -1;
    }
    return std::atoi(version.substr(0, end).c_str());
}

int current_major()
{
    return major_of(kWorkflowVersion);
}

} // namespace

std::string currentWorkflowVersion() { return kWorkflowVersion; }

// 生产迁移表：**当前为空** —— 历史上没有「必须改文件才能载入」的破坏性变更。
//  * 新增迁移步骤时：填 `from_version` / `to_version` / `note` / `apply` 四项，
//    `apply` 只改文档（返回 false 即整体拒绝，reason 进 error）；
//  * 机制本身由 `migration_selftest()` 的**夹具**覆盖（不留死代码）。
const std::vector<WorkflowMigrationStep> kWorkflowMigrations{};

WorkflowMigrationOutcome migrate_workflow_with(const std::vector<WorkflowMigrationStep>& steps,
                                               nlohmann::json* document)
{
    WorkflowMigrationOutcome outcome;
    outcome.to = kWorkflowVersion;
    if (document == nullptr) {
        outcome.ok    = false;
        outcome.error = "迁移失败：文档为空（内部调用错误）";
        return outcome;
    }
    if (!document->is_object()) {
        outcome.ok    = false;
        outcome.error = "工作流 JSON 不是对象，无法迁移";
        return outcome;
    }

    outcome.from = document->value("version", std::string());
    const int from_major = major_of(outcome.from);
    const int to_major   = current_major();

    // ---- ① 缺版本号：老文件 ⇒ 按当前版本处理（如实记一条）----
    if (outcome.from.empty()) {
        outcome.notes.emplace_back("文件没有 version 字段（老文件）⇒ 按 " + outcome.to + " 处理");
        return outcome;
    }
    // ---- ② 未来版本：**明确拒绝**（绝不猜、不静默降级）----
    if (from_major > to_major) {
        outcome.ok    = false;
        outcome.error = "工作流文件版本 " + outcome.from + " 比本程序支持的 " + outcome.to +
                        " 更新：请升级 AIwrite 后再打开（本程序不会猜新格式，也不会降级改写你的文件）";
        return outcome;
    }
    if (from_major == to_major) {
        return outcome; // 同主版本（含次版本差异）⇒ 原样通过
    }

    // ---- ③ 旧主版本：逐级走迁移表 ----
    int         major  = from_major;
    int         guard  = 0;
    std::string reason;
    while (major < to_major && guard++ < 64) {
        const WorkflowMigrationStep* step = nullptr;
        for (const WorkflowMigrationStep& candidate : steps) {
            if (major_of(candidate.from_version) == major) {
                step = &candidate;
                break;
            }
        }
        if (step == nullptr) {
            // 表里没有该级的迁移 ⇒ 如实记「无需迁移」（当前生产表就是这种情况）
            outcome.notes.emplace_back("版本 " + std::to_string(major) +
                                       " 到当前版本之间没有需要改文件的变更（表为空）");
            break;
        }
        reason.clear();
        if (!step->apply(document, &reason)) {
            outcome.ok    = false;
            outcome.error = "迁移步骤 " + step->from_version + " → " + step->to_version +
                            " 失败：" + (reason.empty() ? std::string("未给出原因") : reason);
            return outcome;
        }
        outcome.notes.push_back(step->note);
        outcome.migrated = true;
        major            = major_of(step->to_version);
        if (major <= 0) {
            break; // 步表写坏：不猜，按「无需迁移」收场（上面 notes 已记录）
        }
    }
    if (outcome.migrated) {
        (*document)["version"] = kWorkflowVersion;
    }
    return outcome;
}

WorkflowMigrationOutcome migrateWorkflow(nlohmann::json* document)
{
    return migrate_workflow_with(kWorkflowMigrations, document);
}

// ---------------------------------------------------------------------------
//  离线自检（`api_probe --exec-selftest`）
//  * 生产迁移表当前为空 ⇒ 机制用**夹具表**覆盖（见下方 ③），不留死代码
// ---------------------------------------------------------------------------
int migrationSelftest(int* passed_out)
{
    int passed = 0;
    int failed = 0;
    const auto check = [&passed, &failed](bool ok, const std::string& what) {
        if (ok) {
            ++passed;
            std::printf("  ✓ %s\n", what.c_str());
        }
        else {
            ++failed;
            std::printf("  ✗ %s\n", what.c_str());
        }
    };

    // ---- ① 工作流：当前版本 / 缺版本 / 未来版本 ----
    nlohmann::json current_doc = {{"version", kWorkflowVersion}, {"nodes", nlohmann::json::array()}};
    const WorkflowMigrationOutcome current_outcome = migrateWorkflow(&current_doc);
    check(current_outcome.ok && !current_outcome.migrated && current_outcome.notes.empty(),
          "工作流：同主版本 ⇒ 原样通过（不改文档、不产生说明）");
    check(current_doc["version"].get<std::string>() == kWorkflowVersion,
          "工作流：原样通过时 version 字段不变");

    nlohmann::json legacy_doc = {{"nodes", nlohmann::json::array()}};
    const WorkflowMigrationOutcome legacy_outcome = migrateWorkflow(&legacy_doc);
    check(legacy_outcome.ok && !legacy_outcome.migrated && legacy_outcome.notes.size() == 1,
          "工作流：缺 version（老文件）⇒ 按当前版本处理 + 一条说明");
    nlohmann::json future_doc = {{"version", "9.0"}, {"nodes", nlohmann::json::array()}};
    const WorkflowMigrationOutcome future_outcome = migrateWorkflow(&future_doc);
    check(!future_outcome.ok && future_outcome.error.find("更新") != std::string::npos &&
              future_outcome.error.find("升级") != std::string::npos,
          "工作流：未来主版本 ⇒ **明确拒绝** + 可操作错误（不猜、不降级）");
    check(future_doc["version"].get<std::string>() == "9.0",
          "工作流：拒绝时**不改动**调用方文档（原文件不被降级写坏）");

    nlohmann::json minor_doc = {{"version", "1.7"}, {"nodes", nlohmann::json::array()}};
    check(migrateWorkflow(&minor_doc).ok && !migrateWorkflow(&minor_doc).migrated,
          "工作流：同主版本、更高次版本 ⇒ 通过（次版本不拦）");

    nlohmann::json bad_doc = nlohmann::json::array();
    check(!migrateWorkflow(&bad_doc).ok, "工作流：非对象文档 ⇒ 拒绝并给原因");

    // ---- ② 生产迁移表为空时的「旧主版本」口径 ----
    nlohmann::json old_doc = {{"version", "0.9"}, {"nodes", nlohmann::json::array()}};
    const WorkflowMigrationOutcome old_outcome = migrateWorkflow(&old_doc);
    check(old_outcome.ok && !old_outcome.migrated && old_outcome.notes.size() == 1,
          "工作流：旧主版本且表为空 ⇒ 通过 + 「无需迁移」说明（如实登记，不假装迁移过）");

    // ---- ③ 迁移**机制**夹具（生产表为空 ⇒ 用夹具表覆盖机制本身）----
    std::vector<WorkflowMigrationStep> fixture;
    WorkflowMigrationStep              step;
    step.from_version = "0";
    step.to_version   = kWorkflowVersion;
    step.note         = "夹具：把旧节点类型 OldNode 改名为 NewNode";
    step.apply        = [](nlohmann::json* document, std::string* failure) {
        if (document == nullptr || !document->contains("nodes") || !(*document)["nodes"].is_array()) {
            if (failure != nullptr) {
                *failure = "缺少 nodes 数组";
            }
            return false;
        }
        for (nlohmann::json& node : (*document)["nodes"]) {
            if (node.value("type", std::string()) == "OldNode") {
                node["type"] = "NewNode";
            }
        }
        return true;
    };
    fixture.push_back(step);

    nlohmann::json fixture_doc = {{"version", "0.9"},
                                  {"nodes", nlohmann::json::array({nlohmann::json{{"id", "n1"}, {"type", "OldNode"}}})}};
    const WorkflowMigrationOutcome migrated = migrate_workflow_with(fixture, &fixture_doc);
    check(migrated.ok && migrated.migrated && !migrated.notes.empty(),
          "机制：夹具迁移被应用（migrated=true + 说明进 notes）");
    check(fixture_doc["nodes"][0]["type"].get<std::string>() == "NewNode",
          "机制：迁移步骤真的改了文档（节点类型 OldNode → NewNode）");
    check(fixture_doc["version"].get<std::string>() == kWorkflowVersion,
          "机制：迁移后 version 被升级到当前版本");

    std::vector<WorkflowMigrationStep> failing = fixture;
    failing[0].apply = [](nlohmann::json* document, std::string* failure) {
        (void)document;
        if (failure != nullptr) {
            *failure = "夹具故意失败";
        }
        return false;
    };
    nlohmann::json failing_doc = {{"version", "0.9"}, {"nodes", nlohmann::json::array()}};
    const WorkflowMigrationOutcome failed_outcome = migrate_workflow_with(failing, &failing_doc);
    check(!failed_outcome.ok && failed_outcome.error.find("夹具故意失败") != std::string::npos,
          "机制：迁移步骤失败 ⇒ **整体拒绝**且原因透传");

    // ---- ④ 配置迁移（实现归 `utils/config.cpp`；此处调用以一起断言）----
    Config fresh;
    fresh.config_version                   = 1;
    const ConfigMigrationOutcome fresh_out = migrateConfig(fresh);
    check(fresh_out.ok && !fresh_out.future && !fresh.version_newer_than_supported,
          "配置：当前版本 ⇒ 通过且不标记「未来版本」");

    Config legacy_config;
    legacy_config.config_version = 0; // 模拟老文件（无版本键）
    const ConfigMigrationOutcome legacy_cfg = migrateConfig(legacy_config);
    check(legacy_cfg.ok && legacy_cfg.migrated && legacy_cfg.from == 0 &&
              legacy_config.config_version == 1,
          "配置：缺版本（老文件）⇒ 补齐为 1 + 说明");

    Config future_config;
    future_config.config_version = 99;
    const ConfigMigrationOutcome future_cfg = migrateConfig(future_config);
    check(future_cfg.ok && future_cfg.future && future_config.version_newer_than_supported,
          "配置：未来版本 ⇒ **标记拒绝降级写回**（加载继续用已知字段）");
    check(future_cfg.notes.size() == 1 &&
              future_cfg.notes[0].find("拒绝覆盖写回") != std::string::npos,
          "配置：未来版本给出说明（可在 Console 看到）");

    // ---- ⑤ 端到端（**临时目录**，不碰用户配置）：load → 标记 → save 被拒 ----
    {
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "aiwrite_migration_selftest";
        std::error_code cleanup_ec;
        std::filesystem::remove_all(dir, cleanup_ec);
        std::filesystem::create_directories(dir, cleanup_ec);
        const std::filesystem::path future_file   = dir / "config_future.toml";
        const std::filesystem::path normal_file   = dir / "config_normal.toml";

        {
            std::ofstream out(future_file, std::ios::binary | std::ios::trunc);
            out << "config_version = 99\n\n[ui]\nshow_grid = true\n";
        }
        Config loaded_future;
        const bool load_ok = aiwrite::loadConfig(future_file, loaded_future);
        check(load_ok && loaded_future.version_newer_than_supported &&
                  loaded_future.config_version == 99,
              "端到端：读到未来版本配置 ⇒ 加载成功（按已知字段）且标记「未来版本」");
        const bool refused = !aiwrite::saveConfig(future_file, loaded_future);
        check(refused, "端到端：未来版本配置 ⇒ `save_config()` **拒绝写回**");
        {
            std::ifstream     in(future_file, std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            check(buffer.str().find("config_version = 99") != std::string::npos,
                  "端到端：被拒绝后**原文件一字未改**（仍是 99）");
        }

        Config normal;
        normal.ui.grid_size = 24;
        check(aiwrite::saveConfig(normal_file, normal) &&
                  aiwrite::loadConfig(normal_file, normal) &&
                  normal.ui.grid_size == 24 && normal.config_version == 1,
              "端到端：正常配置 ⇒ 保存 / 回读一致（版本补齐为 1）");

        std::filesystem::remove_all(dir, cleanup_ec);
    }

    if (passed_out != nullptr) {
        *passed_out = passed;
    }
    return failed;
}

} // namespace aiwrite::engine
