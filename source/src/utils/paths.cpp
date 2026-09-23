#include "utils/paths.h"

#include <windows.h>

#include <cstdlib>
#include <string>

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

const std::filesystem::path& webview2_profile()
{
    static const std::filesystem::path value = data_root() / "webview2";
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
    const std::filesystem::path dirs[] = {
        data_root(), outputs_dir(), workflows_dir(), snapshots_dir(), logs_dir()};

    for (const auto& dir : dirs) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            ++failures;
        }
    }
    return failures;
}

} // namespace aiwrite::paths
