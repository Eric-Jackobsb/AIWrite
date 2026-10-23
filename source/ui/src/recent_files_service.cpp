#include "recent_files_service.h"

namespace aiwrite::ui {

std::vector<engine::RecentEntry> RecentFiles::load() const
{
    return engine::loadRecentFiles();
}

void RecentFiles::push(const std::string& path, const std::string& name)
{
    engine::pushRecentFile(path, name);
}

void RecentFiles::clear() const
{
    engine::clearRecentFiles();
}

std::vector<engine::RecentStatus> RecentFiles::with_status() const
{
    return engine::recentWithStatus();
}

std::size_t RecentFiles::count_missing() const
{
    return engine::countMissingRecent();
}

std::size_t RecentFiles::prune_missing() const
{
    return engine::pruneMissingRecent();
}

} // namespace aiwrite::ui
