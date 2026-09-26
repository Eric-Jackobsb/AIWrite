#pragma once

// ============================================================================
//  网页版会话存储（设计 §8.5）
//
//  * Cookie 只存在于内存，程序退出即销毁（不写盘、不进日志）
//  * 由 WebView2 登录窗口（web/webview_host）在提取后写入，UI 线程每帧读快照
//  * 线程安全：WebView2 回调线程写，UI 线程读
// ============================================================================

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace aiwrite::web {

struct Cookie {
    std::string name;
    std::string value;                 // 仅内存；日志/界面一律用 mask_value() 脱敏
    bool        http_only = false;
    bool        session   = true;
    double      expires   = 0.0;       // Unix 秒；session=true 时无意义
};

// 网页版协议探测结果（M4-06/M4-08 逆向用）
//  * 全部字段都是**非敏感结构**：Token 只存脱敏值，不含 Cookie 明文
//  * 由 web::request_protocol_probe() 在已登录页面里执行 JS 探测后写入
struct ProbeResult {
    bool        ok = false;
    std::string ran_at;
    std::string local_storage_keys;  // localStorage 键名列表（逗号分隔）
    bool        has_user_token = false;
    std::string user_token_masked;   // userToken（JSON 包装内 value）的脱敏值
    std::string settings_jwt_masked; // settingsJwt 的脱敏值
    std::string challenge_json;      // POST /api/v0/chat/create_pow_challenge 的原始响应
    std::string endpoints_report;    // 候选端点探测（状态码 + 脱敏片段）
    std::string error;

    // 真 Token（仅用于进程内发起网页版请求）：存入 Session 后立即清空，绝不写日志/文件
    std::string token_raw;

    std::string summary() const;     // 一行摘要（供日志/界面）
};

// M_patchB L1 续（PB2-18）：**站点键**（origin）
//  * 取 `scheme://host[:port]`：忽略路径 / 查询 / 末尾斜杠 / 大小写差异（域名不区分大小写）
//  * 无法解析（无 `://`）时**原样返回**（空串 → 空串）
//  * 用途：会话归档键、登录窗口归属判定、按站点注销 —— 与浏览器 cookie/localStorage 的
//    「按 origin 隔离」语义一致（决策 D-19）
std::string site_key_of(const std::string& url);

struct Session {
    bool                logged_in = false;
    std::string         url;            // 会话来源（登录页地址）
    std::string         updated_at;     // "YYYY-MM-DD HH:MM:SS"
    std::vector<Cookie> cookies;
    ProbeResult         probe;          // 协议探测结果（可空）

    // 网页版接口凭证（**仅内存**，程序退出即销毁；日志/文件一律不落）
    std::string         user_token;     // localStorage.userToken（Bearer Token）

    // ---- M_patchB L1 续（PB2-18）：会话归属（多站点并存）----
    std::string         site;           // 站点键（origin）；空 = 默认槽（legacy 单站点路径）
    std::string         provider_id;    // 配置表条目 id（诊断 / 界面显示）

    std::size_t    cookie_count() const { return cookies.size(); }
    bool           has(const std::string& name) const;
    const Cookie*  find(const std::string& name) const;
};

// 脱敏：≤8 位全星号 + 长度；>8 位保留前 4 后 4（与 webview2_login 工具一致）
std::string mask_value(const std::string& value);

// 多站点会话存储（M_patchB L1 续 / PB2-18）
//
//  * 内部按**站点键**（origin）归档：`std::map<site, Session>` —— 多个网页版站点
//    （deepseek-web + 用户自建站点）的凭证**互不覆盖**
//  * **兼容**：不传站点的旧 API 操作「默认槽」（`current_`，改造前的“那一个会话”语义），
//    因此 `--login-selftest` / `--web-probe` / `--web-chat` / `--web-session-selftest`
//    的行为与结果不变（不变量 I2）
class SessionStore {
public:
    static SessionStore& instance();

    // ---- 兼容 API（默认槽）----
    void    set(Session session);   // 覆盖写入（按 session.site 归档；空 = 默认槽）
    void    set_probe(ProbeResult probe); // 只更新协议探测结果（保留 Cookie 会话）
    void    clear();                // 注销默认槽：清空并销毁
    Session snapshot() const;       // 拷贝快照（线程安全）
    bool    logged_in() const;
    std::size_t cookie_count() const;

    // ---- 多站点 API（PB2-18）----
    void        set_current_site(const std::string& site); // 指定默认槽指向（空 = 默认槽本身）
    std::string current_site() const;                      // 默认槽当前指向的站点
    Session     snapshot(const std::string& site) const;
    void        set_probe(ProbeResult probe, const std::string& site);
    bool        has_token(const std::string& site) const;  // 该站点是否已有内存凭证
    bool        logged_in(const std::string& site) const;
    std::size_t cookie_count(const std::string& site) const;
    void        clear(const std::string& site);            // 按站点注销（不动其他站点）
    void        clear_all();                               // 清全部（高级：删除整个 profile 时）
    std::vector<std::string> sites() const;                // 已归档站点键（按字典序稳定）

private:
    SessionStore() = default;
    SessionStore(const SessionStore&)            = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    // 站点键归一：空串 → 默认槽当前指向
    std::string resolve_key(const std::string& site) const;

    mutable std::mutex             mutex_;
    std::map<std::string, Session> sessions_;   // 站点键 → 会话
    std::string                    current_;    // 默认槽键（"" = 未指定站点）
};

} // namespace aiwrite::web
