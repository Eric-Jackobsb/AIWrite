// 登录态快照（L2）只读视图 —— 实现（`MB-D0-8` L2 · M7B 批 2）
//
//  * **只碰元数据**：`exists` / `file_size` / `last_write_time` —— **永不打开文件读内容**（`I23③`）
//  * 任何失败都**如实回报**（`exists=false`），不假装、不抛异常到 UI 线程

#include "web/session_snapshot.h"

#include <chrono>
#include <ctime>
#include <system_error>

#include "utils/paths.h"

namespace aiwrite::web {
namespace {

std::string format_time(std::filesystem::file_time_type value)
{
    // file_time → system_clock（两时钟纪元不同，先取 now 的差值再平移）
    const auto        sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t seconds = std::chrono::system_clock::to_time_t(sys);
    std::tm           local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char buffer[32] = {0};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return buffer;
}

} // namespace

std::filesystem::path snapshot_file()
{
    return paths::session_snapshot_dir() / kSnapshotFileName;
}

SnapshotInfo inspect_snapshot()
{
    return inspect_snapshot_at(snapshot_file());
}

SnapshotInfo inspect_snapshot_at(const std::filesystem::path& path)
{
    SnapshotInfo    info;
    std::error_code ec;

    info.exists = std::filesystem::exists(path, ec);
    if (ec || !info.exists) {
        info.exists = false;  // 拿不到就当不存在（如实回报，不假装）
        return info;
    }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (!ec) {
        info.bytes         = size;
        info.in_size_limit = size <= kSnapshotMaxBytes;
    }
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(path, ec);
    if (!ec) {
        info.modified_at = format_time(stamp);
    }
    return info;
}

std::string snapshot_summary(const SnapshotInfo& info)
{
    if (!info.exists) {
        return "登录态快照：尚无（首次登录并干净退出后生成）";
    }
    std::string text =
        "登录态快照：" + std::to_string(info.bytes) + " 字节 · 更新于 " + info.modified_at;
    if (!info.in_size_limit) {
        text += "（⚠️ 超过 1 MiB 上限：Python 侧按「损坏」处理并引导重新登录）";
    }
    return text;
}

std::string snapshot_trigger_note()
{
    return "写入时机：定时 " + std::to_string(static_cast<int>(kSnapshotRefreshPeriod)) +
           " s（< 10 s 硬上限）· 判到「已登录」即写 · 守护进程关闭前再写一次；"
           "下次启动自动回灌（失败会显式提示重新登录）";
}

} // namespace aiwrite::web
