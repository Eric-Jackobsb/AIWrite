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
    // PC-01/02：自动保存节流阈值与历史份数（M_patchAB §4）
    if (const std::string error = range_error("自动保存间隔（秒）", config.ui.autosave_idle_sec, 1, 600);
        !error.empty()) {
        return error;
    }
    if (const std::string error = range_error("自动保存历史份数", config.ui.autosave_keep, 0, 50);
        !error.empty()) {
        return error;
    }
    // ---- M_patchAB §2.3（`PB-06` 剩余）：网络（代理 / 自签证书）与凭据库 ----
    //  * 代理地址必须带 scheme（否则 httplib 无法解析 ⇒ 报错要**说清怎么写**）
    //  * `insecure_tls` **合法**（是兼容开关）⇒ 不拦，只在界面上按危险项提示
    if (!config.network.proxy.empty()) {
        const std::string& proxy = config.network.proxy;
        const bool has_scheme = proxy.rfind("http://", 0) == 0 || proxy.rfind("https://", 0) == 0 ||
                                proxy.rfind("socks5://", 0) == 0;
        if (!has_scheme) {
            return "代理地址需以 http:// 或 https:// 开头（当前：" + proxy +
                   "；例 http://127.0.0.1:7890；不需要代理就把该项留空）";
        }
        if (proxy.size() <= proxy.find("://") + 3) {
            return "代理地址缺少主机名（当前：" + proxy + "；例 http://127.0.0.1:7890）";
        }
    }
    if (const std::string error = range_error("凭据保留天数（0 = 不清理）", config.credentials.ttl_days,
                                              0, 3650);
        !error.empty()) {
        return error;
    }
    if (config.credentials.backend != "dpapi") {
        return "凭据后端暂只支持 dpapi（当前：" + config.credentials.backend +
               "；换后端要先实现对应的加解密，见 utils/credential.cpp）";
    }
    return std::string();
}

} // namespace aiwrite::utils
