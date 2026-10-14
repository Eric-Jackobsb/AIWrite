#include "recent_files.h"

#include "log.h"
#include "paths.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace aiwrite::engine {
namespace {

constexpr const char* kRecentVersion = "1.0";

std::string now_string()
{
    const std::time_t now = std::time(nullptr);
    std::tm           local{};
    localtime_s(&local, &now);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return buffer;
}

bool same_path(const std::string& left, const std::string& right)
{
    std::error_code   ec_left;
    std::error_code   ec_right;
    const std::filesystem::path norm_left  = std::filesystem::weakly_canonical(left, ec_left);
    const std::filesystem::path norm_right = std::filesystem::weakly_canonical(right, ec_right);
    if (!ec_left && !ec_right) {
        return norm_left == norm_right;
    }
    return left == right;
}

void save_recent(const std::vector<RecentEntry>& entries)
{
    nlohmann::json json;
    json["version"] = kRecentVersion;
    json["entries"] = nlohmann::json::array();
    for (const RecentEntry& entry : entries) {
        json["entries"].push_back(nlohmann::json{
            {"path", entry.path}, {"name", entry.name}, {"openedAt", entry.opened_at}});
    }

    const std::filesystem::path file = paths::recent_file();
    std::error_code             ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        log::warn("最近列表写入失败: " + file.string());
        return;
    }
    stream << json.dump(2);
}

} // namespace

std::vector<RecentEntry> load_recent_files()
{
    std::vector<RecentEntry> result;

    const std::filesystem::path file = paths::recent_file();
    std::ifstream               stream(file, std::ios::binary);
    if (!stream) {
        return result; // 尚未创建过：空列表
    }

    try {
        const nlohmann::json json = nlohmann::json::parse(stream);
        for (const nlohmann::json& item : json.value("entries", nlohmann::json::array())) {
            RecentEntry entry;
            entry.path      = item.value("path", std::string());
            entry.name      = item.value("name", std::string());
            entry.opened_at = item.value("openedAt", std::string());
            if (!entry.path.empty()) {
                result.push_back(std::move(entry));
            }
            if (result.size() >= kMaxRecentFiles) {
                break;
            }
        }
    }
    catch (const std::exception& ex) {
        log::warn("recent.json 解析失败（按空列表处理）: " + std::string(ex.what()));
        result.clear();
    }
    return result;
}

void push_recent_file(const std::string& path, const std::string& name)
{
    if (path.empty()) {
        return;
    }

    std::vector<RecentEntry> entries = load_recent_files();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&path](const RecentEntry& entry) {
                                     return same_path(entry.path, path);
                                 }),
                  entries.end());

    RecentEntry entry;
    entry.path = path;
    entry.name = name.empty() ? std::filesystem::path(path).stem().string() : name;
    entry.opened_at = now_string();
    entries.insert(entries.begin(), std::move(entry));

    if (entries.size() > kMaxRecentFiles) {
        entries.resize(kMaxRecentFiles);
    }
    save_recent(entries);
}

void clear_recent_files()
{
    save_recent({});
}

// ---- M_patchAB §4（PC-03 / FEA-M2-02）：最近列表治理 ----

std::vector<RecentStatus> recent_with_status()
{
    std::vector<RecentStatus> result;
    for (const RecentEntry& entry : load_recent_files()) {
        RecentStatus status;
        status.entry  = entry;
        std::error_code ec;
        status.exists = std::filesystem::is_regular_file(entry.path, ec) && !ec;
        result.push_back(std::move(status));
    }
    return result;
}

std::size_t count_missing_recent()
{
    std::size_t missing = 0;
    for (const RecentStatus& status : recent_with_status()) {
        if (!status.exists) {
            ++missing;
        }
    }
    return missing;
}

std::size_t prune_missing_recent()
{
    const std::vector<RecentEntry>  entries = load_recent_files();
    std::vector<RecentEntry>        kept;
    std::size_t                     removed = 0;
    for (const RecentEntry& entry : entries) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(entry.path, ec) && !ec) {
            kept.push_back(entry);
        }
        else {
            ++removed;
            log::info("[最近列表] 清理失效条目: " + entry.path);
        }
    }
    if (removed > 0) {
        save_recent(kept); // 写盘（失败时 save_recent 自己记 warn）
    }
    return removed;
}

} // namespace aiwrite::engine
