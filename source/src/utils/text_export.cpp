#include "utils/text_export.h"

#include "utils/log.h"
#include "utils/paths.h"

#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace aiwrite::utils {
namespace {

bool is_bad_stem_char(unsigned char ch)
{
    if (ch < 0x20 || ch == 0x7F) {
        return true;
    }
    const std::string bad = "<>:\"/\\|?*";
    return bad.find(static_cast<char>(ch)) != std::string::npos;
}

std::string local_time_text(const char* format)
{
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), format, &tm);
    return buffer;
}

} // namespace

std::string sanitize_file_stem(const std::string& stem)
{
    std::string out;
    for (const unsigned char ch : stem) {
        out += is_bad_stem_char(ch) ? '_' : static_cast<char>(ch);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) {
        out.erase(out.begin());
    }
    if (out.size() > 60) { // 字节上限；避免切坏 UTF-8（丢弃尾部不完整字符）
        out.resize(60);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
            out.pop_back();
        }
    }
    return out.empty() ? std::string("未命名") : out;
}

std::string render_document_name(const std::string& workflow, const std::string& stamp,
                                 const std::string& ext)
{
    std::string normalized = ext.empty() ? std::string("md") : ext;
    if (normalized.front() == '.') {
        normalized.erase(normalized.begin());
    }
    for (char& ch : normalized) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return sanitize_file_stem(workflow) + "-" + (stamp.empty() ? document_stamp_now() : stamp) + "." +
           normalized;
}

std::string build_document_body(const std::string& text, const std::string& label,
                                const DocumentMeta& meta, bool include_meta)
{
    if (!include_meta) {
        return text;
    }
    std::string body;
    body += "# " + (label.empty() ? std::string("输出") : label) + "\n\n";
    if (!meta.workflow.empty()) {
        body += "> 工作流：" + meta.workflow + "\n";
    }
    if (!meta.node_id.empty()) {
        body += "> 节点：" + meta.node_id + "（" + (meta.state.empty() ? "done" : meta.state) + "，" +
                std::to_string(static_cast<long long>(meta.duration_ms + 0.5)) + " ms）\n";
    }
    if (!meta.stamp.empty()) {
        body += "> 时间：" + meta.stamp + "\n";
    }
    body += "\n" + text;
    return body;
}

bool write_document(const std::string& path, const std::string& body, bool overwrite,
                    std::string* final_path, std::string* error)
{
    if (path.empty()) {
        if (error != nullptr) {
            *error = "导出路径为空";
        }
        return false;
    }
    std::error_code     ec;
    const std::filesystem::path target(path);
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        if (error != nullptr) {
            *error = "无法创建目录：" + target.parent_path().string() + "（" + ec.message() + "）";
        }
        return false;
    }

    std::filesystem::path chosen = target;
    if (!overwrite && std::filesystem::exists(chosen, ec)) {
        const std::string stem = target.stem().string();
        const std::string ext  = target.extension().string();
        for (int index = 1; index < 1000; ++index) {
            std::filesystem::path candidate = target.parent_path() /
                                              (stem + "-" + std::to_string(index) + ext);
            if (!std::filesystem::exists(candidate, ec)) {
                chosen = candidate;
                break;
            }
        }
    }

    const std::filesystem::path temp = chosen.string() + ".tmp";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) {
            if (error != nullptr) {
                *error = "无法写入临时文件：" + temp.string();
            }
            return false;
        }
        stream.write(body.data(), static_cast<std::streamsize>(body.size()));
        stream.close();
    }

    std::error_code rename_error;
    std::filesystem::rename(temp, chosen, rename_error);
    if (rename_error) {
        std::error_code remove_error;
        std::filesystem::remove(chosen, remove_error); // 目标已存在（覆盖场景）→ 先删再改名
        std::filesystem::rename(temp, chosen, rename_error);
    }
    if (rename_error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temp, cleanup_error);
        if (error != nullptr) {
            *error = "写入失败：" + rename_error.message();
        }
        return false;
    }
    if (final_path != nullptr) {
        *final_path = chosen.string();
    }
    return true;
}

std::string document_stamp_now()
{
    return local_time_text("%Y%m%d-%H%M%S");
}

ExportResult export_text_document(const ExportRequest& request)
{
    ExportResult result;
    std::filesystem::path target(request.path);
    if (target.empty()) {
        const std::string dir = request.dir.empty() ? (paths::data_root() / "outputs").string() : request.dir;
        target = std::filesystem::path(dir) /
                 render_document_name(request.meta.workflow, document_stamp_now(), request.ext);
    }
    const std::string body = build_document_body(request.text, request.label, request.meta,
                                                request.include_meta);
    std::string       final_path;
    std::string       error;
    if (!write_document(target.string(), body, request.overwrite, &final_path, &error)) {
        result.error = error;
        return result;
    }
    result.ok   = true;
    result.path = final_path;
    log::info("[导出] 已写出文档：" + final_path + "（" + std::to_string(body.size()) + " 字符）");
    return result;
}

int text_export_selftest()
{
    int               failed = 0;
    std::error_code   ec;
    const std::string dir = (std::filesystem::temp_directory_path() / "aiwrite_text_export_selftest").string();
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    const std::string text  = "从前有座山，山里有座庙。";
    const std::string stamp = "20260925-213000";
    DocumentMeta      meta;
    meta.workflow = "我的工作流";
    meta.node_id  = "n4";
    meta.state    = "done";
    meta.stamp    = "2026-09-25 21:30:00";

    // 1) 文件名渲染 + 清洗
    {
        const std::string name = render_document_name(meta.workflow, stamp, ".MD");
        const std::string dirty = sanitize_file_stem("a/b:c*?d " );
        const bool ok = name == "我的工作流-20260925-213000.md" && dirty.find_first_of("<>:\"/\\|?*") == std::string::npos &&
                        dirty.find("_") != std::string::npos;
        if (!ok) { ++failed; log::error("[导出自检] 1 文件名渲染/清洗：失败 name=" + name + " dirty=" + dirty); }
        else { log::info("[导出自检] 1 文件名渲染 + 非法字符清洗：OK"); }
    }

    // 2) 正文（无元信息头 == text；有则包含 label/工作流/节点）
    {
        const std::string plain = build_document_body(text, "最终输出", meta, false);
        const std::string full  = build_document_body(text, "最终输出", meta, true);
        const bool ok = plain == text && full.find("最终输出") != std::string::npos &&
                        full.find(meta.workflow) != std::string::npos && full.find("n4") != std::string::npos &&
                        full.size() > text.size();
        if (!ok) { ++failed; log::error("[导出自检] 2 正文拼接：失败"); }
        else { log::info("[导出自检] 2 正文（含/不含元信息头）：OK"); }
    }

    // 3) 写盘内容 == 内存 body
    ExportRequest request;
    request.text         = text;
    request.label        = "最终输出";
    request.meta         = meta;
    request.include_meta = true;
    request.overwrite    = true;
    request.dir          = dir;
    request.ext          = "md";
    {
        const ExportResult first = export_text_document(request);
        std::string        read_back;
        {
            std::ifstream stream(first.path, std::ios::binary);
            read_back.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        }
        const bool ok = first.ok && read_back == build_document_body(text, request.label, meta, true);
        if (!ok) { ++failed; log::error("[导出自检] 3 写盘回读：失败 " + first.error); }
        else { log::info("[导出自检] 3 写盘内容 == 内存正文：OK"); }
    }

    // 4) 同名策略：不覆盖 → 新路径；覆盖 → 同路径
    {
        ExportRequest      safe     = request;
        safe.overwrite              = false;
        const ExportResult keep     = export_text_document(safe);
        const ExportResult replaced = export_text_document(request);
        const bool ok = keep.ok && replaced.ok && keep.path != replaced.path;
        if (!ok) { ++failed; log::error("[导出自检] 4 同名自动改名：失败"); }
        else { log::info("[导出自检] 4 同名策略（不覆盖→改名）：OK"); }
    }

    // 5) 不可写路径 → 明确错误
    {
        ExportRequest bad = request;
        bad.path          = "Z:\\aiwrite_should_not_exist\\x.md";
        const ExportResult result = export_text_document(bad);
        if (result.ok || result.error.empty()) { ++failed; log::error("[导出自检] 5 不可写路径：失败"); }
        else { log::info("[导出自检] 5 不可写路径返回明确错误：OK"); }
    }

    // 6) 原子写不残留 .tmp
    {
        bool leftover = false;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".tmp") { leftover = true; }
        }
        if (leftover) { ++failed; log::error("[导出自检] 6 残留临时文件：失败"); }
        else { log::info("[导出自检] 6 无 .tmp 残留：OK"); }
    }

    // 7) label 生效（标题行包含标签）
    {
        const std::string body = build_document_body(text, "我的标签", meta, true);
        if (body.find("# 我的标签") == std::string::npos) { ++failed; log::error("[导出自检] 7 label：失败"); }
        else { log::info("[导出自检] 7 label 进入标题：OK"); }
    }

    std::filesystem::remove_all(dir, ec);
    return failed;
}

} // namespace aiwrite::utils
