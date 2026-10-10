#include "diagnostics_report.h"

#include "app_version.h"
#include "config.h"
#include "credential.h"
#include "log.h"
#include "paths.h"
#include "provider_spec.h"
#include "webview2_runtime.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace aiwrite::utils {
namespace {

std::string narrow(const std::wstring& wide)
{
    if (wide.empty()) {
        return {};
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), need,
                          nullptr, nullptr);
    return out;
}

// 读注册表字符串值（本文件独立的小拷贝：诊断报告只读 OS 版本，
// 不复用 webview2_runtime.cpp 的私有实现——那一个还带 32/64 位视图语义）
std::string read_reg_string(HKEY root, const wchar_t* subkey, const wchar_t* name)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t    buffer[256] = {};
    DWORD      type        = 0;
    DWORD      size        = sizeof(buffer) - sizeof(wchar_t);
    const LONG status =
        ::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) {
        return {};
    }
    return narrow(std::wstring(buffer));
}

std::string local_time_text()
{
    const std::time_t now = std::time(nullptr);
    std::tm         tm{};
    if (::localtime_s(&tm, &now) != 0) {
        return "(时钟不可用)";
    }
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buffer;
}

std::string megabytes_text(unsigned long long bytes)
{
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    char         buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.1f GB", gb);
    return buffer;
}

std::string truncate(const std::string& text, std::size_t max_chars)
{
    if (text.size() <= max_chars) {
        return text;
    }
    return text.substr(0, max_chars) + "…";
}

std::string path_or_missing(const std::filesystem::path& path)
{
    std::error_code code;
    if (std::filesystem::exists(path, code)) {
        return path.string();
    }
    return path.string() + "（尚未创建）";
}

std::string count_files(const std::filesystem::path& dir, const std::string& extension)
{
    std::error_code code;
    if (!std::filesystem::is_directory(dir, code)) {
        return "（目录不存在）";
    }
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, code)) {
        if (!entry.is_regular_file(code)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (extension.empty() ||
            (name.size() >= extension.size() &&
             name.compare(name.size() - extension.size(), extension.size(), extension) == 0)) {
            ++count;
        }
    }
    if (code) {
        return "（读取失败）";
    }
    return std::to_string(count) + " 个";
}

// 最近一次归档目录名（按目录修改时间取最新；无则空串）
std::string newest_archive_dir_name()
{
    const std::filesystem::path dir = paths::expand(app_config().output.archive_dir);
    std::error_code             code;
    if (!std::filesystem::is_directory(dir, code)) {
        return {};
    }
    std::filesystem::path newest;
    std::filesystem::file_time_type newest_time{};
    for (const auto& entry : std::filesystem::directory_iterator(dir, code)) {
        if (!entry.is_directory(code)) {
            continue;
        }
        const auto time = entry.last_write_time(code);
        if (code) {
            continue;
        }
        if (newest.empty() || time > newest_time) {
            newest      = entry.path();
            newest_time = time;
        }
    }
    return newest.empty() ? std::string() : newest.filename().string();
}

} // namespace

std::string DiagnosticsReport::text() const
{
    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

bool DiagnosticsReport::contains(const std::string& needle) const
{
    return text().find(needle) != std::string::npos;
}

std::string DiagnosticsReport::key_value(const std::string& key) const
{
    const std::string prefix = key + " = ";
    for (const std::string& line : lines) {
        if (line.rfind(prefix, 0) == 0) {
            return line.substr(prefix.size());
        }
    }
    return {};
}


DiagnosticsReport collect_diagnostics()
{
    DiagnosticsReport         report;
    std::vector<std::string>& lines = report.lines;

    // ---- [报告] ----
    lines.push_back("[报告] AIwrite 诊断信息");
    lines.push_back("采集时间 = " + local_time_text());
    lines.push_back("程序版本 = " + std::string(kAppVersion));
    lines.push_back("版本标识 = " + std::string(kAppVersionLabel));
#if defined(NDEBUG)
    const char* build_config = "Release";
#else
    const char* build_config = "Debug";
#endif
    lines.push_back("构建配置 = " + std::string(build_config) + "（编译器 MSVC " +
                    std::to_string(_MSC_VER) + " / " + std::to_string(sizeof(void*) * 8) + " 位）");

    // ---- [系统] ----
    const std::string product  = read_reg_string(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
    const std::string display  = read_reg_string(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");
    const std::string build_no = read_reg_string(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber");
    std::string os_text = product.empty() ? std::string("Windows（版本未知）") : product;
    if (!display.empty()) {
        os_text += " " + display;
    }
    if (!build_no.empty()) {
        os_text += "（build " + build_no + "）";
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength         = sizeof(memory);
    std::string memory_text = "（不可用）";
    if (::GlobalMemoryStatusEx(&memory) != FALSE) {
        memory_text = megabytes_text(memory.ullTotalPhys);
    }

    lines.push_back("[系统]");
    lines.push_back("操作系统 = " + os_text);
    lines.push_back("逻辑处理器 = " + std::to_string(::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) +
                    " 核");
    lines.push_back("物理内存 = " + memory_text);

    // ---- [依赖]（网页版站点前置条件；缺失时给出可操作指引）----
    const WebView2RuntimeInfo webview2 = detect_webview2_runtime();
    lines.push_back("[依赖]");
    lines.push_back("WebView2 = " + webview2_runtime_summary(webview2));

    // ---- [路径] ----
    lines.push_back("[路径]");
    lines.push_back("程序目录 = " + paths::exe_dir().string());
    lines.push_back("数据目录 = " + path_or_missing(paths::data_root()));
    lines.push_back("配置文件 = " + path_or_missing(paths::config_file()));
    lines.push_back("日志文件 = " + log::file_path_string());
    lines.push_back("工作流目录 = " + paths::workflows_dir().string());
        // 说明：`paths::expand` 只做 `~` 替换（保留原串里的 `/`）⇒ 报告里再做一次 lexically_normal，
        // 让路径显示与资源管理器一致（不改变 expand 本身的行为，那条路径有自检断言覆盖）
        lines.push_back("输出目录 = " +
                        paths::expand(app_config().output.archive_dir).lexically_normal().string());
    lines.push_back("图片资源目录 = " + paths::assets_images_dir().string());

    // ---- [配置]（只报值与开关，不含任何密钥）----
    const Config&           config = app_config();
    const auto              on_off = [](bool value) { return value ? "开" : "关"; };
    std::string             unwired_text;
    for (const std::string& field : unwired_config_fields()) {
        unwired_text += (unwired_text.empty() ? "" : "、") + field;
    }

    lines.push_back("[配置]");
    lines.push_back("config_version = " + std::to_string(config.config_version));
    lines.push_back("界面可见性 = 节点库=" + std::string(on_off(config.ui.show_node_library)) +
                    " / 参数面板=" + on_off(config.ui.show_property_panel) +
                    " / Console=" + on_off(config.ui.show_console) +
                    " / 输出=" + on_off(config.ui.show_output_window));
    lines.push_back("画布 = 网格=" + std::string(on_off(config.ui.show_grid)) +
                    " / 间距=" + std::to_string(config.ui.grid_size) + "（暂不生效）" +
                    " / 运行动效=" + on_off(config.ui.running_animation));
    lines.push_back("完成后自动打开归档 = " +
                    std::string(on_off(config.output.auto_open_on_complete)));
    lines.push_back("历史归档 = " + std::string(config.output.keep_history ? "保留多份" : "只留最近 1 份") +
                    " / 上限=" + std::to_string(config.output.max_history) +
                    " / 清理阈值=" + std::to_string(config.output.ttl_days) + " 天");
    lines.push_back("日志保留 = " + std::to_string(config.advanced.log_ttl_days) + " 天");
    lines.push_back("未接配置字段 = " + std::to_string(unwired_config_fields().size()) + " 项（" +
                    unwired_text + "）");

    // ---- [站点表]（provider 配置表加载结果；只读）----
    const ai::ProviderSpecs&  specs       = ai::provider_specs();
    const ai::SpecLoadReport& spec_report = specs.report;
    lines.push_back("[站点表]");
    lines.push_back("加载状态 = " + std::string(spec_report.ok ? "正常" : "有错误（已回落到可用表）") +
                    (spec_report.fallback_used ? "｜已用最小兜底表" : ""));
    lines.push_back("条目总数 = " + std::to_string(specs.items.size()) + "（官方 API " +
                    std::to_string(specs.count_kind("official")) + " / 网页版 " +
                    std::to_string(specs.count_kind("web")) + "）");
    lines.push_back("来源层数 = " + std::to_string(spec_report.files.size()));
    std::string first_error;
    for (const std::string& item : spec_report.errors) {
        first_error = truncate(item, 160);
        break;
    }
    std::string first_warning;
    for (const std::string& item : spec_report.warnings) {
        first_warning = truncate(item, 160);
        break;
    }
    lines.push_back("错误数 = " + std::to_string(spec_report.errors.size()) +
                    (first_error.empty() ? "" : "（首条：" + first_error + "）"));
    lines.push_back("警告数 = " + std::to_string(spec_report.warnings.size()) +
                    (first_warning.empty() ? "" : "（首条：" + first_warning + "）"));

    // ---- [凭据]（**只出条数**：既不输出条目名，也不输出密钥 / 密文）----
    lines.push_back("[凭据]");
    lines.push_back("凭据库目录 = " + credentials_dir());
    lines.push_back("已配置凭据数 = " + std::to_string(list_credentials(30).size()) +
                    "（只出条数，不出条目名与密钥）");

    // ---- [归档与工作流] ----
    const std::string newest = newest_archive_dir_name();
    lines.push_back("[归档与工作流]");
    lines.push_back("工作流文件数 = " + count_files(paths::workflows_dir(), ".json"));
    lines.push_back("最近一次归档 = " + (newest.empty() ? std::string("（无）") : newest));

    return report;
}

std::string diagnostics_report_text()
{
    return collect_diagnostics().text();
}

} // namespace aiwrite::utils
