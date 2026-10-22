#include "paths.h"

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

const std::filesystem::path& dataRoot()
{
    static const std::filesystem::path value = home() / ".brain-ai";
    return value;
}

const std::filesystem::path& outputsDir()
{
    static const std::filesystem::path value = dataRoot() / "outputs";
    return value;
}

const std::filesystem::path& workflowsDir()
{
    static const std::filesystem::path value = dataRoot() / "workflows";
    return value;
}

const std::filesystem::path& snapshotsDir()
{
    static const std::filesystem::path value = dataRoot() / "snapshots";
    return value;
}

const std::filesystem::path& logsDir()
{
    static const std::filesystem::path value = dataRoot() / "logs";
    return value;
}

// CN-实施（Composition_node）：上级节点独立文件存储目录
const std::filesystem::path& compositesDir()
{
    static const std::filesystem::path value = dataRoot() / "composites";
    return value;
}

// PS-D9：上级节点存储双源
//  内置源：<source>/workflows/compositeNodes（仓库内 · 随分发 · 只读）
//    * 开发 / 源码树直跑 → AIWRITE_SOURCE_DIR（CMake 注入 aiwrite_core）
//    * 打包部署（无该宏）→ <exe>/assets/workflows/compositeNodes
const std::filesystem::path& builtinCompositesDir()
{
    static const std::filesystem::path value = []() {
#ifdef AIWRITE_SOURCE_DIR
        return std::filesystem::path(AIWRITE_SOURCE_DIR) / "workflows" / "compositeNodes";
#else
        return assetsDir() / "workflows" / "compositeNodes";
#endif
    }();
    return value;
}

// 用户源：~/.brain-ai/workflows/compositeNodes（用户工作流同级 · 可读写）
const std::filesystem::path& userCompositesDir()
{
    static const std::filesystem::path value = workflowsDir() / "compositeNodes";
    return value;
}

const std::filesystem::path& configFile()
{
    static const std::filesystem::path value = dataRoot() / "config.toml";
    return value;
}

const std::filesystem::path& appLogFile()
{
    static const std::filesystem::path value = logsDir() / "app.log";
    return value;
}

const std::filesystem::path& workflowLogFile()
{
    static const std::filesystem::path value = logsDir() / "workflow.log";
    return value;
}

const std::filesystem::path& recentFile()
{
    static const std::filesystem::path value = dataRoot() / "recent.json";
    return value;
}

const std::filesystem::path& webview2_profile()
{
    static const std::filesystem::path value = dataRoot() / "webview2";
    return value;
}

// M7B：Pydoll 单 profile（与 Python `browsers.PROFILE` / `driver.PROFILE` 同值）
const std::filesystem::path& pydollProfile()
{
    static const std::filesystem::path value = dataRoot() / "pydoll-profile";
    return value;
}

// MB-D0-8 L2：登录态加密快照目录（**≠ snapshots_dir()**：那个是工作流执行快照）
//  * 目录由 Python `session.py` 按需创建（`SnapshotStore.save` 里 mkdir）；C++ 侧**不建**空目录
const std::filesystem::path& sessionSnapshotDir()
{
    static const std::filesystem::path value = dataRoot() / "session";
    return value;
}

// P7a-04：统一资源目录（图片归档根）
const std::filesystem::path& assetsImagesDir()
{
    static const std::filesystem::path value = dataRoot() / "assets" / "images";
    return value;
}

const std::filesystem::path& exeDir()
{
    static const std::filesystem::path value = compute_exe_dir();
    return value;
}

std::filesystem::path assetsDir()
{
    return exeDir() / "assets";
}

std::filesystem::path providersAssetFile()
{
    return assetsDir() / "providers.json"; // 随程序发布的内置配置表（PB2-02）
}

std::filesystem::path userProvidersDir()
{
    return dataRoot() / "providers.d"; // 用户新增条目（probe 表 / API 表通用）
}

std::filesystem::path userProvidersFile()
{
    return dataRoot() / "providers.json"; // 用户字段级覆盖
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

int ensureDataDirs()
{
    int failures = 0;
    const std::filesystem::path dirs[] = {dataRoot(),         outputsDir(),     workflowsDir(),
                                          snapshotsDir(),     logsDir(),
                                          assetsImagesDir(), // P7a-04：统一资源目录
                                          userProvidersDir(), // 配置表用户目录（PB2-02）
                                          userCompositesDir()}; // PS-D9：上级节点用户源目录

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
std::vector<std::string> splitPathList(const std::string& value)
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

std::string joinPathList(const std::vector<std::string>& paths)
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
