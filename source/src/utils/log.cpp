#include "utils/log.h"

#include "utils/paths.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>

namespace aiwrite::log {
namespace {

std::deque<Entry>& mutable_entries()
{
    static std::deque<Entry> buffer;
    return buffer;
}

std::mutex& entries_mutex()
{
    static std::mutex mutex;
    return mutex;
}

const char* level_name(Level level)
{
    switch (level) {
    case Level::Warn:
        return "WARN";
    case Level::Error:
        return "ERROR";
    case Level::Info:
    default:
        return "INFO";
    }
}

Level to_level(spdlog::level::level_enum level)
{
    if (level >= spdlog::level::err) {
        return Level::Error;
    }
    if (level >= spdlog::level::warn) {
        return Level::Warn;
    }
    return Level::Info;
}

// 固定格式（设计文档 12.3）
class AppFormatter final : public spdlog::formatter {
public:
    void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override
    {
        const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(msg.time);
        const auto millis =
            std::chrono::duration_cast<std::chrono::milliseconds>(msg.time - seconds).count();

        const std::time_t raw = std::chrono::system_clock::to_time_t(seconds);
        std::tm local{};
        ::localtime_s(&local, &raw);

        char stamp[80] = {};
        std::snprintf(stamp, sizeof(stamp), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] ",
                      local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
                      local.tm_min, local.tm_sec, static_cast<int>(millis),
                      level_name(to_level(msg.level)));

        dest.append(stamp, stamp + std::strlen(stamp));
        dest.append(msg.payload.begin(), msg.payload.end());
    }

    std::unique_ptr<spdlog::formatter> clone() const override
    {
        return std::make_unique<AppFormatter>(*this);
    }
};

// 写入内存环形缓冲的 sink
class RingBufferSink final : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        spdlog::memory_buf_t formatted;
        this->formatter_->format(msg, formatted);

        Entry entry;
        entry.time  = msg.time;
        entry.level = to_level(msg.level);
        entry.text.assign(formatted.data(), formatted.size());

        std::lock_guard<std::mutex> lock(entries_mutex());
        auto& buffer = mutable_entries();
        buffer.push_back(std::move(entry));
        while (buffer.size() > kMaxEntries) {
            buffer.pop_front();
        }
    }

    void flush_() override {}
};

std::shared_ptr<spdlog::logger> g_logger;

} // namespace

void init(bool attach_console)
{
    if (attach_console && ::GetConsoleWindow() == nullptr) {
        if (::AllocConsole() != 0) {
            FILE* stream = nullptr;
            (void)freopen_s(&stream, "CONOUT$", "w", stdout);
            (void)freopen_s(&stream, "CONOUT$", "w", stderr);
            ::SetConsoleOutputCP(CP_UTF8);
        }
    }

    paths::ensure_data_dirs();
    const std::filesystem::path log_file = paths::app_log_file();

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<RingBufferSink>());

    if (::GetConsoleWindow() != nullptr) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    try {
        // 设计文档 12.5：单文件 10MB，滚动 5 个
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file.string(), 10 * 1024 * 1024, 5));
    }
    catch (const spdlog::spdlog_ex& ex) {
        std::fprintf(stderr, "[log] 无法创建日志文件 %s: %s\n", log_file.string().c_str(), ex.what());
    }

    for (auto& sink : sinks) {
        sink->set_formatter(std::make_unique<AppFormatter>());
    }

    g_logger = std::make_shared<spdlog::logger>("app", sinks.begin(), sinks.end());
    g_logger->set_level(spdlog::level::info);
    g_logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(g_logger);

    // 周期性落盘，便于用外部工具 tail app.log
    spdlog::flush_every(std::chrono::seconds(2));
}

void shutdown()
{
    if (g_logger) {
        g_logger->flush();
    }
    spdlog::shutdown();
}

void info(const std::string& message)
{
    spdlog::info("{}", message);
}

void warn(const std::string& message)
{
    spdlog::warn("{}", message);
}

void error(const std::string& message)
{
    spdlog::error("{}", message);
}

const std::deque<Entry>& entries()
{
    return mutable_entries();
}

std::vector<Entry> filter(Level min_level, bool use_level_filter, const std::string& search)
{
    std::vector<Entry> result;
    std::lock_guard<std::mutex> lock(entries_mutex());
    for (const auto& entry : mutable_entries()) {
        if (use_level_filter && entry.level < min_level) {
            continue;
        }
        if (!search.empty() && entry.text.find(search) == std::string::npos) {
            continue;
        }
        result.push_back(entry);
    }
    return result;
}

void clear_entries()
{
    std::lock_guard<std::mutex> lock(entries_mutex());
    mutable_entries().clear();
}

std::string file_path_string()
{
    return paths::app_log_file().string();
}

} // namespace aiwrite::log
