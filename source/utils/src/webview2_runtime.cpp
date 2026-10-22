#include "webview2_runtime.h"

#include "paths.h"   // M6：bundled_bootstrapper_path() 定位 <exe>/webview2/...
#include "string_util.h"   // CLN-05：narrow 收敛

#include <windows.h>

// 说明（实测 2026-10-10 编译报错 C2146 后定位）：本工程全局定义 `WIN32_LEAN_AND_MEAN`，
// 此时 `<windows.h>` 不带 MIDL/COM 基础定义 ⇒ 必须先引 WRL 再引 `<WebView2.h>`。
// 写法与既有文件 `web/webview_host.cpp`、`tools/webview2_login.cpp` 保持一致。
#include <wrl/client.h> // Microsoft::WRL::ComPtr
#include <wrl/event.h>  // Microsoft::WRL::Callback

#include <WebView2.h>

#include <cstdlib>
#include <string>

namespace aiwrite::utils {
namespace {

// ---- 通用小工具：窄字符（ANSI）→ 宽字符 ----
std::wstring widen_ansi(const char* text)
{
    if (text == nullptr || *text == '\0') {
        return {};
    }
    const int need = ::MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (need <= 1) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), need);
    if (!wide.empty() && wide.back() == L'\0') {
        wide.pop_back(); // 去掉 MultiByteToWideChar 计入的结尾 '\0'
    }
    return wide;
}

// ---- 通用小工具：宽字符 → UTF-8 ----

// 读注册表字符串值（REG_SZ / REG_EXPAND_SZ；两种视图都试）；失败返回空串
std::string read_reg_string(HKEY root, const wchar_t* subkey, const wchar_t* name)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS &&
        ::RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS) {
        return {};
    }

    wchar_t    buffer[512] = {};
    DWORD      type        = 0;
    DWORD      size        = sizeof(buffer) - sizeof(wchar_t);
    const LONG status = ::RegQueryValueExW(key, name, nullptr, &type,
                                          reinterpret_cast<LPBYTE>(buffer), &size);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    return utils::narrow(std::wstring(buffer));
}

// 取文件版本（4 段；取不到返回空串）
std::string file_version_of(const std::wstring& file)
{
    DWORD       handle = 0;
    const DWORD size   = ::GetFileVersionInfoSizeW(file.c_str(), &handle);
    if (size == 0) {
        return {};
    }

    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    if (::GetFileVersionInfoW(file.c_str(), 0, size, buffer.data()) == FALSE) {
        return {};
    }

    VS_FIXEDFILEINFO* info   = nullptr;
    UINT              length = 0;
    if (::VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) == FALSE ||
        info == nullptr) {
        return {};
    }

    const DWORD major = (info->dwFileVersionMS >> 16) & 0xFFFFu;
    const DWORD minor = info->dwFileVersionMS & 0xFFFFu;
    const DWORD build = (info->dwFileVersionLS >> 16) & 0xFFFFu;
    const DWORD patch = info->dwFileVersionLS & 0xFFFFu;
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(build) + "." +
           std::to_string(patch);
}

// EdgeUpdate 客户端键（官方固定 GUID；`pv` = 版本号）
constexpr const wchar_t* kEdgeUpdateClientKey32 =
    L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\"
    L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
constexpr const wchar_t* kEdgeUpdateClientKeyNative =
    L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";

} // namespace

WebView2RuntimeInfo detectWebview2Runtime()
{
    WebView2RuntimeInfo info;

    // ① 固定版本部署（环境变量）
    if (const char* fixed = std::getenv("WEBVIEW2_BROWSER_EXECUTABLE_FOLDER");
        fixed != nullptr && *fixed != '\0') {
        const std::wstring dir = widen_ansi(fixed);
        const std::wstring exe = dir + L"\\msedgewebview2.exe";
        if (!dir.empty() && ::GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
            info.installed = true;
            info.source    = "fixed(env)";
            info.version   = file_version_of(exe);
            return info;
        }
        info.error =
            "环境变量 WEBVIEW2_BROWSER_EXECUTABLE_FOLDER 指向的目录里没有 msedgewebview2.exe";
    }

    // ② 加载器 API（优先采信）
    LPWSTR        version = nullptr;
    const HRESULT status  = ::GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    if (SUCCEEDED(status) && version != nullptr) {
        info.installed = true;
        info.source    = "api";
        info.version   = utils::narrow(version);
        ::CoTaskMemFree(version);
        return info;
    }
    if (SUCCEEDED(status)) {
        info.error = "加载器 API 返回成功但未给出运行时版本（系统未安装 WebView2 运行时）";
    }
    else {
        info.error = "加载器 API 调用失败（HRESULT 0x" +
                     std::to_string(static_cast<unsigned long>(status)) + "）";
    }

    // ③ 注册表兜底（API 不可用 / 返回空时）
    if (const std::string pv = read_reg_string(HKEY_LOCAL_MACHINE, kEdgeUpdateClientKey32, L"pv");
        !pv.empty()) {
        info.installed = true;
        info.source    = "registry:hklm32";
        info.version   = pv;
        info.error.clear();
        return info;
    }
    if (const std::string pv =
            read_reg_string(HKEY_LOCAL_MACHINE, kEdgeUpdateClientKeyNative, L"pv");
        !pv.empty()) {
        info.installed = true;
        info.source    = "registry:hklm";
        info.version   = pv;
        info.error.clear();
        return info;
    }
    if (const std::string pv = read_reg_string(HKEY_CURRENT_USER, kEdgeUpdateClientKey32, L"pv");
        !pv.empty()) {
        info.installed = true;
        info.source    = "registry:hkcu";
        info.version   = pv;
        info.error.clear();
        return info;
    }

    if (info.error.empty()) {
        info.error = "三路探测（环境变量 / 加载器 API / 注册表）均未找到 WebView2 运行时";
    }
    return info;
}

const char* webview2_download_url()
{
    // Microsoft Edge WebView2「Evergreen Bootstrapper」官方短链（Microsoft 官方文档所用）
    return "https://go.microsoft.com/fwlink/p/?LinkId=2124703";
}

std::string bundledBootstrapperPath()
{
    // 与 packaging\aiwrite.iss 的 DestDir 约定一致：<安装目录>\webview2\MicrosoftEdgeWebview2Setup.exe
    const std::filesystem::path candidate =
        paths::exeDir() / "webview2" / "MicrosoftEdgeWebview2Setup.exe";
    std::error_code code;
    if (std::filesystem::is_regular_file(candidate, code)) {
        return candidate.string();
    }
    return {};
}

std::string webview2_runtime_summary(const WebView2RuntimeInfo& info)
{
    if (info.installed) {
        std::string text = "WebView2 运行时 ";
        text += info.version.empty() ? std::string("（版本未知）") : info.version;
        if (!info.source.empty()) {
            text += "（" + info.source + "）";
        }
        return text;
    }

    std::string text = "WebView2 运行时 未安装";
    if (!info.error.empty()) {
        text += "（" + info.error + "）";
    }
    text += " —— 网页版站点不可用，请安装：" + std::string(webview2_download_url());
    return text;
}

std::string webview2_install_advice()
{
    std::string text;
    text += "本程序用「Microsoft Edge WebView2 运行时」打开网页版模型站点（Kimi / 豆包 / 通义等）。\n";
    text += "安装步骤：\n";
    text += "  1. 打开下载页（点「打开下载页」按钮，或在浏览器里访问）：\n     ";
    text += webview2_download_url();
    text += "\n  2. 运行下载到的 MicrosoftEdgeWebView2Setup.exe（约 2 分钟，无需任何选项）\n";
    text += "  3. 重新启动 AIwrite\n";
    text += "说明：Windows 11 与较新的 Windows 10 通常已自带，无需安装；\n";
    text += "     如果本窗口显示「已安装」，直接忽略上面步骤即可。\n";
    return text;
}

} // namespace aiwrite::utils
