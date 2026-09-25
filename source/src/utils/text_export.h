#pragma once

// ============================================================================
//  文本导出为文档（docs/actionPlan/M_textio.md P2）
//
//  * 只做“非 UI 核心”：文件名渲染 / 正文拼接 / 原子写 / 组合导出
//  * UI（参数面板 / 输出面板）只负责取保存路径
//  * 正文与预览同源：调用方传入的 text 就是 node_output_text 的结果
// ============================================================================

#include <string>

namespace aiwrite::utils {

struct DocumentMeta {
    std::string workflow;
    std::string node_id;
    std::string state;
    double      duration_ms = 0.0;
    std::string stamp; // 人类可读时间（如 2026-09-25 21:30:00）
};

struct ExportRequest {
    std::string text;             // 正文（与预览同源）
    std::string label;            // TextOutput 的「标签」（可空）
    DocumentMeta meta;
    bool        include_meta = false;
    std::string path;             // 保存对话框给出的完整路径；为空则用 dir + render_document_name
    std::string dir;              // path 为空时使用的目录（默认 outputs/）
    std::string ext      = "md"; // md | txt（归一化后小写）
    bool        overwrite = false;
};

struct ExportResult {
    bool        ok = false;
    std::string path;  // 最终写入路径（同名改写过则为新路径）
    std::string error;
};

// 文件名主干清洗：非法字符 → _，去掉首尾空白与点，限制长度（不切坏 UTF-8）
std::string sanitize_file_stem(const std::string& stem);

// <清洗后的工作流名>-<stamp>.<ext>
std::string render_document_name(const std::string& workflow, const std::string& stamp,
                                 const std::string& ext);

// 正文拼接（include_meta=true 时加标题与元信息块）
std::string build_document_body(const std::string& text, const std::string& label,
                                const DocumentMeta& meta, bool include_meta);

// 原子写（临时文件 + rename）；overwrite=false 且同名存在时自动加 -1/-2…
bool write_document(const std::string& path, const std::string& body, bool overwrite,
                    std::string* final_path, std::string* error);

ExportResult export_text_document(const ExportRequest& request);

// 文件名用时间戳 yyyyMMdd-HHmmss
std::string document_stamp_now();

// 人类可读时间（yyyy-MM-dd HH:mm:ss）：用于文档元信息头
std::string document_time_text();

// P2 的 7 条离线断言；返回失败条数（跑完清理自己的临时目录）
int text_export_selftest();

} // namespace aiwrite::utils
