#include "utils/file_dialog.h"

#include "utils/log.h"

#include <nfd.hpp>

#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h> // ShellExecuteW（打开资源管理器）
#endif

namespace aiwrite::utils {
namespace {

// 每次弹窗前后 Init/Quit：NFD 要求配对调用，重复调用是安全的
class NfdSession {
public:
    NfdSession()
    {
        const nfdresult_t result = NFD::Init();
        ok_                      = (result == NFD_OKAY);
        if (!ok_) {
            log::error(std::string("[文件对话框] 初始化失败: ") + NFD::GetError());
        }
    }

    ~NfdSession()
    {
        if (ok_) {
            NFD::Quit();
        }
    }

    NfdSession(const NfdSession&)            = delete;
    NfdSession& operator=(const NfdSession&) = delete;

    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

std::string to_string(const NFD::UniquePathU8& path)
{
    return (path != nullptr) ? std::string(path.get()) : std::string();
}

} // namespace

bool file_dialog_available()
{
    NfdSession session;
    return session.ok();
}

std::string open_file(const std::vector<FileFilter>& filters, const std::string& default_path)
{
    NfdSession session;
    if (!session.ok()) {
        return {};
    }

    std::vector<nfdu8filteritem_t> items;
    items.reserve(filters.size());
    for (const FileFilter& filter : filters) {
        items.push_back(nfdu8filteritem_t{filter.label.c_str(), filter.pattern.c_str()});
    }

    NFD::UniquePathU8 out_path;
    const nfdresult_t result =
        NFD::OpenDialog(out_path, items.empty() ? nullptr : items.data(),
                        static_cast<int>(items.size()),
                        default_path.empty() ? nullptr : default_path.c_str());

    if (result == NFD_ERROR) {
        log::error(std::string("[文件对话框] 打开文件失败: ") + NFD::GetError());
        return {};
    }
    if (result == NFD_CANCEL) {
        return {};
    }

    const std::string path = to_string(out_path);
    log::info("[文件对话框] 选择文件: " + path);
    return path;
}

std::string pick_folder(const std::string& default_path)
{
    NfdSession session;
    if (!session.ok()) {
        return {};
    }

    NFD::UniquePathU8 out_path;
    const nfdresult_t result =
        NFD::PickFolder(out_path, default_path.empty() ? nullptr : default_path.c_str());

    if (result == NFD_ERROR) {
        log::error(std::string("[文件对话框] 选择目录失败: ") + NFD::GetError());
        return {};
    }
    if (result == NFD_CANCEL) {
        return {};
    }

    const std::string path = to_string(out_path);
    log::info("[文件对话框] 选择目录: " + path);
    return path;
}

std::string save_file(const std::vector<FileFilter>& filters, const std::string& default_path,
                      const std::string& default_name)
{
    NfdSession session;
    if (!session.ok()) {
        return {};
    }

    std::vector<nfdu8filteritem_t> items;
    items.reserve(filters.size());
    for (const FileFilter& filter : filters) {
        items.push_back(nfdu8filteritem_t{filter.label.c_str(), filter.pattern.c_str()});
    }

    NFD::UniquePathU8 out_path;
    const nfdresult_t result =
        NFD::SaveDialog(out_path, items.empty() ? nullptr : items.data(),
                        static_cast<int>(items.size()),
                        default_path.empty() ? nullptr : default_path.c_str(),
                        default_name.empty() ? nullptr : default_name.c_str());

    if (result == NFD_ERROR) {
        log::error(std::string("[文件对话框] 保存文件失败: ") + NFD::GetError());
        return {};
    }
    if (result == NFD_CANCEL) {
        return {};
    }

    const std::string path = to_string(out_path);
    log::info("[文件对话框] 保存为: " + path);
    return path;
}

// M5-03：在资源管理器中定位文件（文件不存在时打开其所在目录）
bool open_in_explorer(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
#if defined(_WIN32)
    std::error_code                     code;
    const std::filesystem::path         target = std::filesystem::path(path).lexically_normal();
    std::wstring                        argument;
    if (std::filesystem::exists(target, code)) {
        argument = L"/select,\"" + target.wstring() + L"\"";
    }
    else if (std::filesystem::exists(target.parent_path(), code)) {
        argument = L"\"" + target.parent_path().wstring() + L"\"";
    }
    else {
        log::error("[文件对话框] 打开位置失败（路径不存在）: " + path);
        return false;
    }
    const HINSTANCE instance =
        ShellExecuteW(nullptr, L"open", L"explorer.exe", argument.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(instance) <= 32) {
        log::error("[文件对话框] 打开位置失败（ShellExecuteW）: " + path);
        return false;
    }
    log::info("[文件对话框] 已在资源管理器中定位: " + path);
    return true;
#else
    log::error("[文件对话框] open_in_explorer 仅支持 Windows: " + path);
    return false;
#endif
}

} // namespace aiwrite::utils
