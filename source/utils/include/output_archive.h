#pragma once

// ============================================================================
//  运行输出归档（PC-05 / PC-06 最小集；为 M5 §11 输出归档打底）
//
//  * 运行结束写出  <archive_dir>/<yyyyMMdd-HHmmss>-<工作流名>/
//      - <node_id>-<Type>.txt   每个有输出的节点一份（文件头含元信息）
//      - run.json               统计 + 逐节点明细（含耗时/错误/文件名/字节数）
//  * 保留策略：keep_history=false → 仅保留最近 1 份；true → 保留 max_history 份
//  * TTL：ttl_days>0 时清理超期目录（只清理符合命名规则的目录；先统计后删除并写日志）
//  * 失败（目录不可写等）只返回 error，不抛异常、不影响运行流程
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>

namespace aiwrite::utils {

// 单个节点的归档条目
struct ArchiveNode {
    std::string node_id;
    std::string type;
    std::string state;       // done / error / skipped …
    double      duration_ms = 0.0;
    std::string error;
    std::string text;        // 结果正文（空则只进 run.json，不写 .txt）
};

struct ArchiveRequest {
    std::string workflow_name;        // 工作流名（空 → "未命名"）
    std::string archive_dir;          // 空 → paths::outputs_dir()；支持 "~/" 前缀
    bool        keep_history = false; // 保留历史（false → 只留最近 1 份）
    int         max_history  = 10;    // keep_history=true 时的上限
    int         ttl_days     = 0;     // >0 → 清理超期目录
};

struct ArchiveResult {
    bool        ok      = false;
    std::string dir;           // 本次归档目录
    std::size_t files   = 0;   // 写出文件数（含 run.json）
    std::string error;
};

// 执行归档；run_summary 为 Executor::summary()（写入 run.json 与文件头）
ArchiveResult archive_run(const ArchiveRequest& request, const std::vector<ArchiveNode>& nodes,
                          const std::string& run_summary);

} // namespace aiwrite::utils
