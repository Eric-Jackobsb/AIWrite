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

struct Session {
    bool                logged_in = false;
    std::string         url;            // 会话来源（登录页地址）
    std::string         updated_at;     // "YYYY-MM-DD HH:MM:SS"
    std::vector<Cookie> cookies;

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
