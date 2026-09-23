#pragma once

// ============================================================================
//  网页版会话存储（设计 §8.5）
//
//  * Cookie 只存在于内存，程序退出即销毁（不写盘、不进日志）
//  * 由 WebView2 登录窗口（web/webview_host）在提取后写入，UI 线程每帧读快照
//  * 线程安全：WebView2 回调线程写，UI 线程读
// ============================================================================

#include <cstddef>
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

struct Session {
    bool                logged_in = false;
    std::string         url;            // 会话来源（登录页地址）
    std::string         updated_at;     // "YYYY-MM-DD HH:MM:SS"
    std::vector<Cookie> cookies;
    ProbeResult         probe;          // 协议探测结果（可空）

    // 网页版接口凭证（**仅内存**，程序退出即销毁；日志/文件一律不落）
    std::string         user_token;     // localStorage.userToken（Bearer Token）

    std::size_t    cookie_count() const { return cookies.size(); }
    bool           has(const std::string& name) const;
    const Cookie*  find(const std::string& name) const;
};

// 脱敏：≤8 位全星号 + 长度；>8 位保留前 4 后 4（与 webview2_login 工具一致）
std::string mask_value(const std::string& value);

class SessionStore {
public:
    static SessionStore& instance();

    void    set(Session session);   // 覆盖写入（线程安全）
    void    set_probe(ProbeResult probe); // 只更新协议探测结果（保留 Cookie 会话）
    void    clear();                // 注销：清空并销毁
    Session snapshot() const;       // 拷贝快照（线程安全）
    bool    logged_in() const;
    std::size_t cookie_count() const;

private:
    SessionStore() = default;
    SessionStore(const SessionStore&)            = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    mutable std::mutex mutex_;
    Session            session_;
};

} // namespace aiwrite::web
