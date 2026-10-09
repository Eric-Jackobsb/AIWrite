#include "utils/paths.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace aiwrite::paths {
namespace {

std::filesystem::path compute_home()
{
    if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
        return std::filesystem::path(profile);
    }

    const char* drive = std::getenv("HOMEDRIVE");
    const char* rest  = std::getenv("HOMEPATH");
    if (drive != nullptr && rest != nullptr) {
        return std::filesystem::path(std::string(drive) + rest);
    }

    return std::filesystem::current_path();
}

std::filesystem::path compute_exe_dir()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return std::filesystem::current_path();
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
}

} // namespace

const std::filesystem::path& home()
{
    static const std::filesystem::path value = compute_home();
    return value;
}

const std::filesystem::path& data_root()
{
    static const std::filesystem::path value = home() / ".brain-ai";
    return value;
}

const std::filesystem::path& outputs_dir()
{
    static const std::filesystem::path value = data_root() / "outputs";
    return value;
}

const std::filesystem::path& workflows_dir()
{
    static const std::filesystem::path value = data_root() / "workflows";
    return value;
}

const std::filesystem::path& snapshots_dir()
{
    static const std::filesystem::path value = data_root() / "snapshots";
    return value;
}

const std::filesystem::path& logs_dir()
{
    static const std::filesystem::path value = data_root() / "logs";
    return value;
}

const std::filesystem::path& config_file()
{
    static const std::filesystem::path value = data_root() / "config.toml";
    return value;
}

const std::filesystem::path& app_log_file()
{
    static const std::filesystem::path value = logs_dir() / "app.log";
    return value;
}

const std::filesystem::path& workflow_log_file()
{
    static const std::filesystem::path value = logs_dir() / "workflow.log";
    return value;
}

const std::filesystem::path& recent_file()
{
    static const std::filesystem::path value = data_root() / "recent.json";
    return value;
}

const std::filesystem::path& webview2_profile()
{
    static const std::filesystem::path value = data_root() / "webview2";
    return value;
}

// M7B：Pydoll 单 profile（与 Python `browsers.PROFILE` / `driver.PROFILE` 同值）
const std::filesystem::path& pydoll_profile()
{
    static const std::filesystem::path value = data_root() / "pydoll-profile";
    return value;
}

// MB-D0-8 L2：登录态加密快照目录（**≠ snapshots_dir()**：那个是工作流执行快照）
//  * 目录由 Python `session.py` 按需创建（`SnapshotStore.save` 里 mkdir）；C++ 侧**不建**空目录
const std::filesystem::path& session_snapshot_dir()
{
    static const std::filesystem::path value = data_root() / "session";
    return value;
}

// P7a-04：统一资源目录（图片归档根）
const std::filesystem::path& assets_images_dir()
{
    static const std::filesystem::path value = data_root() / "assets" / "images";
    return value;
}

const std::filesystem::path& exe_dir()
{
    static const std::filesystem::path value = compute_exe_dir();
    return value;
}

std::filesystem::path assets_dir()
{
    return exe_dir() / "assets";
}

std::filesystem::path providers_asset_file()
{
    return assets_dir() / "providers.json"; // 随程序发布的内置配置表（PB2-02）
}

std::filesystem::path user_providers_dir()
{
    return data_root() / "providers.d"; // 用户新增条目（probe 表 / API 表通用）
}

std::filesystem::path user_providers_file()
{
    return data_root() / "providers.json"; // 用户字段级覆盖
}

std::filesystem::path expand(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    if (value == "~") {
        return home();
    }
    if (value.rfind("~/", 0) == 0 || value.rfind("~\\", 0) == 0) {
        return home() / std::filesystem::path(value.substr(2));
    }
    return std::filesystem::path(value);
}

int ensure_data_dirs()
{
    int failures = 0;
    const std::filesystem::path dirs[] = {data_root(),         outputs_dir(),     workflows_dir(),
                                          snapshots_dir(),     logs_dir(),
                                          assets_images_dir(), // P7a-04：统一资源目录
                                          user_providers_dir()}; // 配置表用户目录（PB2-02）

    for (const auto& dir : dirs) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            ++failures;
        }
    }
    return failures;
}

// ---------------------------------------------- 多值路径串（P7a-02 / M7）------
std::vector<std::string> split_path_list(const std::string& value)
{
    std::vector<std::string> paths;
    std::string              current;

    const auto flush = [&paths, &current]() {
        const std::size_t begin = current.find_first_not_of(" \t\r");
        if (begin != std::string::npos) {
            const std::size_t end  = current.find_last_not_of(" \t\r");
            const std::string item = current.substr(begin, end - begin + 1);
            if (!item.empty() && std::find(paths.begin(), paths.end(), item) == paths.end()) {
                paths.push_back(item);
            }
        }
        current.clear();
    };

    for (const char ch : value) {
        if (ch == '\n') {
            flush();
            continue;
        }
        current += ch;
    }
    flush();
    return paths;
}

std::string join_path_list(const std::vector<std::string>& paths)
{
    std::string text;
    for (const std::string& path : paths) {
        if (path.empty()) {
            continue;
        }
        if (!text.empty()) {
            text += '\n';
        }
        text += path;
    }
    return text;
}

} // namespace aiwrite::paths
