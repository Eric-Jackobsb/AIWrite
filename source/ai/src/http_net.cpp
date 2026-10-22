#include "http_net.h"

#include "log.h"
#include "string_util.h"   // CLN-05：trim/toLower 收敛

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace aiwrite::ai {
namespace {

// 逗号分隔切分（空项丢弃）
std::vector<std::string> split_commas(const std::string& text)
{
    std::vector<std::string> items;
    std::size_t              begin = 0;
    while (begin <= text.size()) {
        const std::size_t comma = text.find(',', begin);
        const std::string part   = utils::trim(text.substr(
            begin, comma == std::string::npos ? std::string::npos : comma - begin));
        if (!part.empty()) {
            items.push_back(part);
        }
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return items;
}

// `host:port` 形式（去端口，返回主机；`has_port` 回传是否显式带端口）
std::string strip_port(const std::string& host, bool* has_port)
{
    if (has_port != nullptr) {
        *has_port = false;
    }
    if (host.empty() || host.front() == '[') { // IPv6 字面量（`[::1]:8080`）
        const std::size_t close = host.find(']');
        if (close != std::string::npos) {
            if (has_port != nullptr) {
                *has_port = host.find(':', close) != std::string::npos;
            }
            return host.substr(0, close + 1);
        }
        return host;
    }
    const std::size_t colon = host.find(':');
    if (colon == std::string::npos) {
        return host;
    }
    if (has_port != nullptr) {
        *has_port = true;
    }
    return host.substr(0, colon);
}

} // namespace

EnvLookup defaultEnvLookup()
{
    return [](const char* name) {
        if (name == nullptr) {
            return std::string();
        }
        const char* value = std::getenv(name);
        return (value != nullptr) ? std::string(value) : std::string();
    };
}

std::string hostOfUrl(const std::string& url)
{
    std::string       rest   = url;
    const std::size_t scheme = rest.find("://");
    if (scheme != std::string::npos) {
        rest = rest.substr(scheme + 3);
    }
    const std::size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        rest = rest.substr(0, slash);
    }
    const std::size_t at = rest.rfind('@'); // userinfo
    if (at != std::string::npos) {
        rest = rest.substr(at + 1);
    }
    return utils::toLower(strip_port(rest, nullptr));
}

bool noProxyMatches(const std::string& url, const std::string& no_proxy)
{
    const std::string host = hostOfUrl(url);
    if (host.empty() || utils::trim(no_proxy).empty()) {
        return false;
    }
    for (const std::string& raw : split_commas(no_proxy)) {
        const std::string item = utils::toLower(raw);
        if (item == "*") {
            return true;
        }
        bool              item_has_port = false;
        const std::string item_host     = strip_port(item, &item_has_port);
        if (item_host.empty()) {
            continue;
        }
        // `host:port` 形态：必须**连端口一起**匹配（curl 口径）
        if (item_has_port) {
            std::string       url_host = url;
            const std::size_t scheme   = url_host.find("://");
            if (scheme != std::string::npos) {
                url_host = url_host.substr(scheme + 3);
            }
            const std::size_t slash = url_host.find('/');
            if (slash != std::string::npos) {
                url_host = url_host.substr(0, slash);
            }
            const std::size_t at = url_host.rfind('@');
            if (at != std::string::npos) {
                url_host = url_host.substr(at + 1);
            }
            if (utils::toLower(url_host) == item) {
                return true;
            }
            continue;
        }
        if (item_host.front() == '.') { // 后缀匹配（显式点前缀：只匹配子域）
            if (host.size() >= item_host.size() &&
                host.compare(host.size() - item_host.size(), item_host.size(), item_host) == 0) {
                return true;
            }
            continue;
        }
        if (host == item_host) {
            return true;
        }
        // 不带点前缀的写法（`example.com`）也按「含子域」处理（与本项目其余「主域」口径一致）
        const std::string suffix = "." + item_host;
        if (host.size() > suffix.size() &&
            host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return true;
        }
    }
    return false;
}

std::string resolve_proxy(const std::string& url, const std::string& explicit_proxy,
                          bool proxy_from_env, const std::string& explicit_no_proxy,
                          const EnvLookup& env, std::string* source)
{
    const auto set_source = [source](const std::string& text) {
        if (source != nullptr) {
            *source = text;
        }
    };
    const EnvLookup lookup = env ? env : defaultEnvLookup();

    // `[network].no_proxy` 非空 ⇒ 覆盖环境变量（口径：显式配置 > 环境）
    const std::string no_proxy = !utils::trim(explicit_no_proxy).empty() ? utils::trim(explicit_no_proxy)
                                                                  : utils::trim(lookup("NO_PROXY"));
    if (!no_proxy.empty() && noProxyMatches(url, no_proxy)) {
        set_source("NO_PROXY");
        return {};
    }
    if (!utils::trim(explicit_proxy).empty()) {
        set_source("配置");
        return utils::trim(explicit_proxy);
    }
    if (!proxy_from_env) {
        set_source("无");
        return {};
    }
    const bool         https_target = url.rfind("https://", 0) == 0;
    const char* const  names[]      = {https_target ? "HTTPS_PROXY" : "HTTP_PROXY",
                                       https_target ? "https_proxy" : "http_proxy", "ALL_PROXY",
                                       "all_proxy"};
    for (const char* name : names) {
        const std::string value = utils::trim(lookup(name));
        if (!value.empty()) {
            set_source(name);
            return value;
        }
    }
    set_source("无");
    return {};
}

HttpNetOptions resolveHttpNet(const std::string& url, const Config& config, const EnvLookup& env)
{
    HttpNetOptions options;
    options.proxy = resolve_proxy(url, config.network.proxy, config.network.proxy_from_env,
                                  config.network.no_proxy, env, &options.proxy_source);
    options.verify_tls = !config.network.insecure_tls;
    if (options.proxy.empty() && options.proxy_source.empty()) {
        options.proxy_source = "无";
    }
    return options;
}

HttpNetOptions resolveHttpNet(const std::string& url)
{
    return resolveHttpNet(url, appConfig(), defaultEnvLookup());
}

void applyHttpNet(httplib::Client& client, const HttpNetOptions& options)
{
    // 证书校验：显式设置（默认严格；只有 `insecure_tls=true` 才关）
    client.enable_server_certificate_verification(options.verify_tls);
    if (options.proxy.empty()) {
        return;
    }
    std::string       proxy  = options.proxy;
    const std::size_t scheme = proxy.find("://");
    if (scheme != std::string::npos) {
        proxy = proxy.substr(scheme + 3);
    }
    const std::size_t slash = proxy.find('/');
    if (slash != std::string::npos) {
        proxy = proxy.substr(0, slash);
    }
    std::string       user;
    std::string       password;
    const std::size_t at = proxy.rfind('@');
    if (at != std::string::npos) {
        const std::string userinfo = proxy.substr(0, at);
        proxy                      = proxy.substr(at + 1);
        const std::size_t colon    = userinfo.find(':');
        user                       = userinfo.substr(0, colon);
        password = (colon == std::string::npos) ? std::string() : userinfo.substr(colon + 1);
    }
    const std::size_t colon = proxy.rfind(':');
    std::string       host  = proxy;
    int               port  = 8080;
    if (colon != std::string::npos) {
        host                = proxy.substr(0, colon);
        const std::string p = proxy.substr(colon + 1);
        try {
            port = std::stoi(p);
        }
        catch (const std::exception&) {
            port = 8080; // 端口写坏 ⇒ 退回常见默认值（配置校验已拦下明显错误）
        }
    }
    if (host.empty()) {
        log::warn("[网络] 代理地址解析失败，已忽略：" + describeHttpNet(options));
        return;
    }
    client.set_proxy(host, port);
    if (!user.empty()) {
        client.set_proxy_basic_auth(user, password);
    }
}

std::string describeHttpNet(const HttpNetOptions& options)
{
    std::string proxy_text = "直连（未走代理）";
    if (!options.proxy.empty()) {
        std::string       masked = options.proxy;
        const std::size_t scheme = masked.find("://");
        const std::size_t begin  = (scheme == std::string::npos) ? 0 : scheme + 3;
        const std::size_t at     = masked.find('@', begin);
        const std::size_t colon  = masked.find(':', begin);
        if (at != std::string::npos && colon != std::string::npos && colon < at) {
            masked = masked.substr(0, colon + 1) + "***" + masked.substr(at); // **不打口令**
        }
        proxy_text = masked;
    }
    return "代理 = " + proxy_text + "（来源 " + options.proxy_source + "）；证书校验 = " +
           (options.verify_tls ? "严格" : "已关闭（自签证书兼容；仅建议内网使用）");
}

} // namespace aiwrite::ai
