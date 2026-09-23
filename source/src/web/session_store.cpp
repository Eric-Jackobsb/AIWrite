#include "web/session_store.h"

#include "utils/log.h"

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

SessionStore& SessionStore::instance()
{
    static SessionStore store;
    return store;
}

void SessionStore::set(Session session)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session.logged_in = !session.cookies.empty();
        if (session.updated_at.empty()) {
            session.updated_at = now_string();
        }
        session_ = std::move(session);
    }
    log::info("[网页版会话] 已更新：Cookie " + std::to_string(cookie_count()) +
              " 条（仅内存，程序退出即销毁）");
}

void SessionStore::set_probe(ProbeResult probe)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (probe.ran_at.empty()) {
            probe.ran_at = now_string();
        }
        if (!probe.token_raw.empty()) {
            session_.user_token = probe.token_raw; // 真 Token 只留在这一个内存字段
            probe.token_raw.clear();               // 探测结果本身可安全打印
        }
        session_.probe = std::move(probe);
    }
    log::info("[网页版会话] 协议探测结果已更新：" + snapshot().probe.summary());
}

void SessionStore::clear()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_ = Session{};
    }
    log::info("[网页版会话] 已注销：Cookie 已从内存销毁");
}

Session SessionStore::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return session_;
}

bool SessionStore::logged_in() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return session_.logged_in;
}

std::size_t SessionStore::cookie_count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return session_.cookies.size();
}

} // namespace aiwrite::web
