#include "retry_policy.h"

#include <algorithm>

namespace aiwrite::ai {

bool http_status_retryable(int http_status)
{
    if (http_status == 0) {
        return true; // 网络 / 超时 / DNS：没有 HTTP 状态，值得重试
    }
    if (http_status == 429) {
        return true;
    }
    return http_status >= 500 && http_status <= 599;
}

int retry_delay_ms(int base_ms, int attempt)
{
    const int base = base_ms > 0 ? base_ms : 2000;
    const int step = attempt < 0 ? 0 : attempt;
    return std::min(30000, base * (step + 1));
}

int clamp_retry_count(bool retry_enabled, int retry_count)
{
    if (!retry_enabled) {
        return 0;
    }
    if (retry_count < 0) {
        return 0;
    }
    return std::min(5, retry_count);
}

} // namespace aiwrite::ai
