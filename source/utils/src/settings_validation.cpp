#include "settings_validation.h"

namespace aiwrite::utils {
namespace {

std::string range_error(const char* label, long long value, long long low, long long high)
{
    if (value < low || value > high) {
        return std::string(label) + " 需在 " + std::to_string(low) + " ~ " + std::to_string(high) +
               " 之间（当前 " + std::to_string(value) + "）";
    }
    return std::string();
}

} // namespace

std::string validate_settings(const Config& config)
{
    if (config.output.archive_dir.empty()) {
        return "归档目录不能为空（填一个目录，或用右侧「选择目录…」按钮选一个）";
    }

    if (const std::string error =
            range_error("logs 保留天数", config.advanced.log_ttl_days, 0, 3650);
        !error.empty()) {
        return error;
    }
    if (const std::string error = range_error("Console 高度", config.ui.console_height, 60, 400);
        !error.empty()) {
        return error;
    }
    if (const std::string error = range_error("网格间距", config.ui.grid_size, 4, 200);
        !error.empty()) {
        return error;
    }
    if (const std::string error = range_error("归档清理阈值（天）", config.output.ttl_days, 0, 3650);
        !error.empty()) {
        return error;
    }
    if (const std::string error = range_error("归档保留份数", config.output.max_history, 1, 100);
        !error.empty()) {
        return error;
    }
    if (const std::string error =
            range_error("连接超时（毫秒）", config.timeout.connect_ms, 1000, 600000);
        !error.empty()) {
        return error;
    }
    if (const std::string error =
            range_error("首字节超时（毫秒）", config.timeout.first_byte_ms, 1000, 600000);
        !error.empty()) {
        return error;
    }
    if (const std::string error =
            range_error("流空闲超时（毫秒）", config.timeout.stream_idle_ms, 1000, 3600000);
        !error.empty()) {
        return error;
    }
    if (const std::string error = range_error("重试次数", config.error.retry_count, 0, 10);
        !error.empty()) {
        return error;
    }
    if (const std::string error =
            range_error("重试间隔（毫秒）", config.error.retry_interval_ms, 0, 60000);
        !error.empty()) {
        return error;
    }
    return std::string();
}

} // namespace aiwrite::utils
