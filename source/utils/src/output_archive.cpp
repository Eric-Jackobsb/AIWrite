#include "utils/output_archive.h"

#include "utils/log.h"
#include "utils/paths.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

namespace aiwrite::utils {
namespace {

namespace fs = std::filesystem;

// 文件名安全化（去掉路径分隔符与 Windows 非法字符）
std::string sanitize_name(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        const bool bad = ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' ||
                         ch == '"' || ch == '<' || ch == '>' || ch == '|' ||
                         static_cast<unsigned char>(ch) < 0x20;
        out.push_back(bad ? '_' : ch);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.size() > 60) {
        out.resize(60);
    }
    return out.empty() ? std::string("未命名") : out;
}

std::string timestamp_for_dir()
{
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y%m%d-%H%M%S");
    return stream.str();
}

std::string timestamp_for_header()
{
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

// 展开 "~" / "~/" 前缀与空值（空 → paths::outputs_dir()）
fs::path resolve_archive_root(const std::string& archive_dir)
{
    if (archive_dir.empty()) {
        return paths::outputs_dir();
    }
    if (archive_dir == "~") {
        return paths::data_root().parent_path();
    }
    if (archive_dir.rfind("~/", 0) == 0 || archive_dir.rfind("~\\", 0) == 0) {
        return paths::data_root().parent_path() / archive_dir.substr(2);
    }
    return fs::path(archive_dir);
}

// 目录名形如 20260924-120605-工作流名（用于保留策略与 TTL 判定）
bool is_archive_dir_name(const std::string& name)
{
    if (name.size() < 15) {
        return false;
    }
    for (std::size_t i = 0; i < 15; ++i) {
        const char ch = name[i];
        if (i == 8) {
            if (ch != '-') {
                return false;
            }
            continue;
        }
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

// 从目录名解析时间戳（失败返回 time_t(-1)）
std::time_t archive_dir_time(const std::string& name)
{
    if (!is_archive_dir_name(name)) {
        return static_cast<std::time_t>(-1);
    }
    std::tm tm{};
    std::istringstream stream(name.substr(0, 15));
    stream >> std::get_time(&tm, "%Y%m%d-%H%M%S");
    if (stream.fail()) {
        return static_cast<std::time_t>(-1);
    }
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::string node_file_name(const ArchiveNode& node)
{
    return sanitize_name(node.node_id) + "-" + sanitize_name(node.type) + ".txt";
}

} // namespace

ArchiveResult archive_run(const ArchiveRequest& request, const std::vector<ArchiveNode>& nodes,
                          const std::string& run_summary)
{
    ArchiveResult result;

    const fs::path    root          = resolve_archive_root(request.archive_dir);
    const std::string workflow_name = sanitize_name(request.workflow_name);
    const std::string stamp         = timestamp_for_dir();

    // 同一秒内多次运行：追加序号，避免覆盖上一份
    fs::path        dir = root / (stamp + "-" + workflow_name);
    std::error_code ec;
    for (int suffix = 2; fs::exists(dir, ec) && suffix < 100; ++suffix) {
        dir = root / (stamp + "-" + workflow_name + "-" + std::to_string(suffix));
    }

    fs::create_directories(dir, ec);
    if (ec) {
        result.error = "创建归档目录失败: " + dir.string() + "（" + ec.message() + "）";
        return result;
    }

    const std::string archived_at = timestamp_for_header();
    nlohmann::json    json;
    json["archived_at"] = archived_at;
    json["workflow"]    = workflow_name;
    json["summary"]     = run_summary;
    json["archive_dir"] = dir.string();
    json["nodes"]       = nlohmann::json::array();

    std::size_t files = 0;
    for (const ArchiveNode& node : nodes) {
        nlohmann::json entry;
        entry["node_id"]     = node.node_id;
        entry["type"]        = node.type;
        entry["state"]       = node.state;
        entry["duration_ms"] = node.duration_ms;
        entry["error"]       = node.error;
        entry["bytes"]       = node.text.size();

        if (!node.text.empty()) {
            const std::string file_name = node_file_name(node);
            std::ofstream     stream(dir / file_name, std::ios::binary | std::ios::trunc);
            if (!stream) {
                log::warn("[归档] 无法写入节点文件: " + file_name);
            }
            else {
                stream << "# AIwrite 运行输出\n"
                       << "节点: " << node.node_id << "（" << node.type << "）\n"
                       << "状态: " << node.state << "\n"
                       << "耗时: " << std::fixed << std::setprecision(2) << node.duration_ms
                       << " ms\n";
                if (!node.error.empty()) {
                    stream << "错误: " << node.error << "\n";
                }
                stream << "时间: " << archived_at << "\n"
                       << "统计: " << run_summary << "\n"
                       << "----------------------------------------\n"
                       << node.text << "\n";
                if (stream.good()) {
                    entry["file"] = file_name;
                    ++files;
                }
                else {
                    log::warn("[归档] 节点文件写入不完整: " + file_name);
                }
            }
        }
        json["nodes"].push_back(std::move(entry));
    }

    {
        std::ofstream stream(dir / "run.json", std::ios::binary | std::ios::trunc);
        if (!stream) {
            result.error = "无法写入 run.json：" + dir.string();
            return result;
        }
        stream << json.dump(2) << "\n";
        if (!stream.good()) {
            result.error = "run.json 写入不完整：" + dir.string();
            return result;
        }
        ++files;
    }

    result.ok    = true;
    result.dir   = dir.string();
    result.files = files;

    // ---- 保留策略与 TTL（只处理符合命名规则的目录，避免误删用户其它文件）----
    const int keep = request.keep_history ? std::max(1, request.max_history) : 1;

    std::vector<std::pair<fs::path, std::time_t>> dirs;
    std::error_code                              list_ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(root, list_ec)) {
        if (!entry.is_directory(list_ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (is_archive_dir_name(name)) {
            dirs.emplace_back(entry.path(), archive_dir_time(name));
        }
    }
    if (list_ec) {
        log::warn("[归档] 读取归档目录失败：" + root.string() + "（" + list_ec.message() + "）");
        return result;
    }

    std::sort(dirs.begin(), dirs.end(), [&dir](const auto& a, const auto& b) {
        const bool a_current = (a.first == dir);
        const bool b_current = (b.first == dir);
        if (a_current != b_current) {
            return a_current; // 本次归档恒排最前（不会被保留策略删除）
        }
        return a.second != b.second ? a.second > b.second
                                    : a.first.filename().string() > b.first.filename().string();
    });

    const std::time_t now        = std::time(nullptr);
    const std::time_t ttl_cutoff =
        request.ttl_days > 0 ? now - static_cast<std::time_t>(request.ttl_days) * 86400
                             : static_cast<std::time_t>(0);

    std::size_t removed = 0;
    for (std::size_t i = 0; i < dirs.size(); ++i) {
        if (dirs[i].first == dir) {
            continue; // 本次归档永不删除
        }
        const bool over_history = i >= static_cast<std::size_t>(keep);
        const bool expired = ttl_cutoff > 0 && dirs[i].second > 0 && dirs[i].second < ttl_cutoff;
        if (!over_history && !expired) {
            continue;
        }
        std::error_code rm_ec;
        const auto      removed_items = fs::remove_all(dirs[i].first, rm_ec);
        if (!rm_ec) {
            ++removed;
            log::workflow("[归档-清理] " + dirs[i].first.string() + "（" +
                          (expired ? "超过 ttl_days" : "超过保留份数") + "，删除 " +
                          std::to_string(removed_items) + " 项）");
        }
        else {
            log::warn("[归档-清理失败] " + dirs[i].first.string() + "（" + rm_ec.message() + "）");
        }
    }
    if (removed > 0) {
        log::info("[归档] 已清理 " + std::to_string(removed) + " 个旧归档目录（保留 " +
                  std::to_string(keep) + " 份，ttl_days=" + std::to_string(request.ttl_days) + "）");
    }
    return result;
}

} // namespace aiwrite::utils
