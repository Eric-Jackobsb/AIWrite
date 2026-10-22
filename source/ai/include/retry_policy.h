#pragma once

// ============================================================================
//  重试策略（M_patchAB §2.6 / `PB-08` 残项 · 把 `config.error.*` 真正接上线）
//
//  * **纯函数**：只回答两个问题 —— 「这类失败值得重试吗」「下一次等多久」；
//    * 重试次数 / 开关 / 间隔来自 `config.toml [error]`（`error.retry_enabled` /
//      `retry_count` / `retry_interval_ms`），由调用方（节点）读取后传进来。
//  * 口径（与 `classify_http_error` 的类别对齐）：
//      - **可重试**：`429`（限流）/ `5xx`（服务端）/ `0`（网络或超时，没有 HTTP 状态）
//      - **不可重试**：`400/401/402/403/404`（请求不合法 / 凭证 / 余额 / 风控 —— 重试无意义，
//        而且盲目重试会加剧风控）
//  * `retry_delay_ms`：线性退避（`base * (attempt+1)`），上限 30 s —— 避免把「重试」写成忙等。
// ============================================================================

namespace aiwrite::ai {

// 该 HTTP 状态（0 = 网络/超时类失败）是否值得重试
bool httpStatusRetryable(int http_status);

// 第 `attempt` 次失败后（attempt 从 0 开始）重试前的等待毫秒；`base_ms<=0` 取 2000
int retryDelayMs(int base_ms, int attempt);

// 重试总次数（`retry_enabled=false` ⇒ 0；负数按 0；上限 5 —— 防呆）
int clampRetryCount(bool retry_enabled, int retry_count);

} // namespace aiwrite::ai
