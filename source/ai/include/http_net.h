#pragma once

// ============================================================================
//  网络策略（代理 / 自签证书）—— M_patchAB §2.3 · `PB-06` 剩余
//
//  动机：企业网络里「出不去 / 证书不受信」是最常见的两类环境问题；此前 ① 代理**完全没接**
//  （`HTTPS_PROXY` 形同虚设）② 证书只能靠 `api_probe --insecure` 单命令绕过（界面 / 节点里无入口）。
//
//  本文件把这两件事收敛成**一处纯逻辑 + 一处应用点**：
//    * `resolve_http_net()`：由 `config.toml [network]` + 环境变量解析出「本次调用走不走代理 /
//      校不校验证书」；**纯函数可注入环境**（`EnvLookup`）⇒ `api_probe --selftest` V-16 离线断言
//    * `apply_http_net(client, options)`：把结论应用到 `httplib::Client`（唯一的副作用点）
//
//  优先级（口径与 `curl` 一致，好解释）：
//    ① `NO_PROXY` 命中（含 `*` / 后缀 `.corp.com` / `host:port`）⇒ **直达**，不看任何代理设置
//    ② `[network].proxy`（显式填的）⇒ 用它
//    ③ `[network].proxy_from_env=true` ⇒ 按目标 scheme 取 `HTTPS_PROXY`（https）/ `HTTP_PROXY`（http）
//       ⇒ 再兜底 `ALL_PROXY`（大小写都试，Windows 两种习惯都有）
//    ④ 都没有 ⇒ 不代理
//
//  安全性：
//    * `insecure_tls` 默认 **false**（严格校验）；只有用户在配置里显式打开才关校验
//    * `describe_http_net()` **永远不打印代理口令**（`user:pass@` ⇒ `user:***@`）
// ============================================================================

#include <functional>
#include <string>

#include <httplib.h>

#include "config.h"

namespace aiwrite::ai {

// 环境变量取值（默认 = `std::getenv`；自检可注入假环境 ⇒ 断言不依赖开发机环境）
using EnvLookup = std::function<std::string(const char* name)>;

EnvLookup defaultEnvLookup();

struct HttpNetOptions {
    std::string proxy;         // 空 = 不代理；否则形如 `http://host:port`（可含 `user:pass@`）
    std::string proxy_source;  // 诊断：`配置` / `HTTPS_PROXY` / `HTTP_PROXY` / `ALL_PROXY` / `NO_PROXY` / `无`
    bool        verify_tls = true; // 默认**严格校验**（= 未打开自签兼容开关）
};

// ---- 纯函数（离线断言用）----

// URL 的**主机名**（去 scheme / userinfo / 端口 / 路径；小写）—— `NO_PROXY` 判据与日志共用
std::string hostOfUrl(const std::string& url);

// `NO_PROXY` 是否命中该 URL（`*` / 精确 / 后缀 `.corp.com` / `host:port` / 逗号分隔 / 大小写不敏感）
bool noProxyMatches(const std::string& url, const std::string& no_proxy);

// 解析代理（返回空串 = 不代理）；`source` 回传命中来源（可为 nullptr）
//  * `explicit_no_proxy` 非空 ⇒ **覆盖**环境变量 `NO_PROXY`（否则读环境变量）
std::string resolve_proxy(const std::string& url, const std::string& explicit_proxy,
                          bool proxy_from_env, const std::string& explicit_no_proxy,
                          const EnvLookup& env, std::string* source);

// 由配置 + 环境解析本次调用的网络选项
HttpNetOptions resolveHttpNet(const std::string& url, const Config& config, const EnvLookup& env);
// 便捷重载：进程配置缓存（`app_config()`）+ 真实环境
HttpNetOptions resolveHttpNet(const std::string& url);

// ---- 应用点（唯一副作用处）----

// 把结论应用到客户端：`set_proxy`（含 `user:pass@` 的 basic 认证）+ 证书校验开关
//  * `auth_style` 之类无关；**不**改任何超时（超时归 `PB2-04` 表驱动 / `timeout.*`）
void applyHttpNet(httplib::Client& client, const HttpNetOptions& options);

// 诊断一行（**口令打码**）：例 `代理 = http://127.0.0.1:7890（来源 配置）；证书校验 = 严格`
std::string describeHttpNet(const HttpNetOptions& options);

} // namespace aiwrite::ai
