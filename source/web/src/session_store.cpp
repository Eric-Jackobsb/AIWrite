#include "web/session_store.h"

#include "ai/provider_spec.h"   // L4（PB2-27）：站点无关登录证据（ai::WebSessionEvidence）
#include "utils/log.h"

#include <cctype>
#include <ctime>

namespace aiwrite::web {
namespace {

std::string now_string()
{
    const std::time_t now = std::time(nullptr);
    std::tm           local{};
    localtime_s(&local, &now);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return buffer;
}

} // namespace

const Cookie* Session::find(const std::string& name) const
{
    for (const Cookie& cookie : cookies) {
        if (cookie.name == name) {
            return &cookie;
        }
    }
    return nullptr;
}

bool Session::has(const std::string& name) const
{
    return find(name) != nullptr;
}

std::string ProbeResult::summary() const
{
    if (error.empty() && user_token_masked.empty() && challenge_json.empty()) {
        return "未探测";
    }
    std::string text = ok ? "已探测" : "探测失败";
    text += std::string("（Token ") + (has_user_token ? user_token_masked : "未获取") + "）";
    if (!error.empty()) {
        text += " 错误: " + error;
    }
    return text;
}

std::string mask_value(const std::string& value)
{
    const std::size_t length = value.size();
    if (length == 0) {
        return "(空)";
    }
    if (length <= 8) {
        return std::string(length, '*') + "(len=" + std::to_string(length) + ")";
    }
    return value.substr(0, 4) + "****" + value.substr(length - 4) +
           "(len=" + std::to_string(length) + ")";
}

// M_patchB L1 续（PB2-18）：站点键（origin；决策 D-19）
std::string site_key_of(const std::string& url)
{
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url; // 非标准 URL（如 about:blank / 相对路径）→ 原样
    }
    std::size_t end = url.find_first_of("/?#", scheme + 3);
    if (end == std::string::npos) {
        end = url.size();
    }
    std::string origin = url.substr(0, end);
    for (char& ch : origin) { // 域名不区分大小写（路径已被截掉，全小写安全）
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return origin;
}
// M_patchB L4（PB2-27）：把内存会话转成「站点无关」的登录证据
//  * 只取 Cookie 维度（是否读过 / 条数 / 名字）——**不取** user_token（不变量 I15）
ai::WebSessionEvidence web_session_evidence(const Session& session)
{
    ai::WebSessionEvidence evidence;
    evidence.cookies_known = !session.url.empty(); // 写过会话快照（登录窗口提取过 Cookie）
    evidence.cookie_count  = session.cookies.size();
    evidence.cookie_names.reserve(session.cookies.size());
    for (const Cookie& cookie : session.cookies) {
        evidence.cookie_names.push_back(cookie.name);
    }
    return evidence;
}



SessionStore& SessionStore::instance()
{
    static SessionStore store;
    return store;
}

std::string SessionStore::resolve_key(const std::string& site) const
{
    // 空串 = 默认槽（`current_`）—— 未指定站点时退化为改造前的“那一个会话”
    return site.empty() ? current_ : site;
}

void SessionStore::set(Session session)
{
    std::string key;
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        key               = resolve_key(session.site);
        session.site      = key; // 归档后回填（便于快照读出归属）
        session.logged_in = !session.cookies.empty();
        if (session.updated_at.empty()) {
            session.updated_at = now_string();
        }
        count          = session.cookies.size();
        sessions_[key] = std::move(session);
        current_       = key;
    }
    log::info("[网页版会话] 已更新：" + (key.empty() ? std::string() : "站点 " + key + " / ") +
              "Cookie " + std::to_string(count) + " 条（仅内存，程序退出即销毁）");
}

void SessionStore::set_probe(ProbeResult probe, const std::string& site)
{
    std::string key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        key              = resolve_key(site);
        Session& session = sessions_[key];
        if (session.url.empty()) {
            session.url = key; // 只有探测结果时，用站点键兜底显示
        }
        if (session.updated_at.empty()) {
            session.updated_at = now_string();
        }
        if (probe.ran_at.empty()) {
            probe.ran_at = now_string();
        }
        if (!probe.token_raw.empty()) {
            session.user_token = probe.token_raw; // 真 Token 只留在这一个内存字段
            probe.token_raw.clear();              // 探测结果本身可安全打印
        }
        session.probe     = std::move(probe);
        session.logged_in = !session.cookies.empty();
        current_          = key;
    }
    log::info("[网页版会话] 协议探测结果已更新（站点 " + (key.empty() ? std::string("(默认)") : key) +
              "）：" + snapshot(key).probe.summary());
}

void SessionStore::set_probe(ProbeResult probe)
{
    set_probe(std::move(probe), std::string());
}

void SessionStore::clear(const std::string& site)
{
    std::string key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        key = resolve_key(site);
        sessions_.erase(key);
    }
    log::info("[网页版会话] 已注销（站点 " + (key.empty() ? std::string("(默认)") : key) +
              "）：Cookie 已从内存销毁");
}

void SessionStore::clear()
{
    clear(std::string());
}

void SessionStore::clear_all()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.clear();
        current_.clear();
    }
    log::info("[网页版会话] 已注销**全部站点**：内存会话已清空");
}

Session SessionStore::snapshot(const std::string& site) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto                  it = sessions_.find(resolve_key(site));
    return it == sessions_.end() ? Session{} : it->second;
}

Session SessionStore::snapshot() const
{
    return snapshot(std::string());
}

bool SessionStore::has_token(const std::string& site) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto                  it = sessions_.find(resolve_key(site));
    return it != sessions_.end() && !it->second.user_token.empty();
}

bool SessionStore::logged_in(const std::string& site) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto                  it = sessions_.find(resolve_key(site));
    return it != sessions_.end() && it->second.logged_in;
}

bool SessionStore::logged_in() const
{
    return logged_in(std::string());
}

std::size_t SessionStore::cookie_count(const std::string& site) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto                  it = sessions_.find(resolve_key(site));
    return it == sessions_.end() ? 0 : it->second.cookies.size();
}

std::size_t SessionStore::cookie_count() const
{
    return cookie_count(std::string());
}

void SessionStore::set_current_site(const std::string& site)
{
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = site;
}

std::string SessionStore::current_site() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

std::vector<std::string> SessionStore::sites() const
{
    std::lock_guard<std::mutex>            lock(mutex_);
    std::vector<std::string>               list;
    list.reserve(sessions_.size());
    for (const auto& item : sessions_) {
        if (item.first.empty()) {
            continue; // 默认槽（legacy）不计入站点列表
        }
        list.push_back(item.first);
    }
    return list; // std::map 天然按字典序 → 顺序稳定
}

} // namespace aiwrite::web
