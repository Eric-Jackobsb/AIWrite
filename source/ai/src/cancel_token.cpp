#include "cancel_token.h"

namespace aiwrite::ai {

CancelWatch::CancelWatch(const CancelToken* token, std::function<void()> abort, int poll_ms)
    : token_(token), abort_(std::move(abort))
{
    if (token_ == nullptr || !abort_) {
        return; // 无令牌 / 无中断动作 ⇒ 纯占位（不开线程、恒不中断）
    }
    const int interval_ms = poll_ms > 0 ? poll_ms : 50;
    worker_ = std::thread([this, interval_ms] {
        while (!stop_.load(std::memory_order_acquire)) {
            if (token_->cancelled()) {
                aborted_.store(true, std::memory_order_release);
                abort_(); // 幂等由调用方负责（httplib Client::stop() 本身可重复调用）
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
    });
}

CancelWatch::~CancelWatch()
{
    stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool runCancellable(const CancelToken* token, std::function<void()> work, int poll_ms)
{
    if (!work) {
        return false;
    }
    if (token == nullptr) {
        work(); // 不可取消：就地执行（与改造前行为一致）
        return false;
    }
    if (token->cancelled()) {
        return true; // 已经取消：**不执行**
    }

    // `done` 与 `work` 都按值进入线程 ⇒ 取消后**分离**也不会悬垂（见头文件契约）
    auto        done   = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([work, done] {
        work();
        done->store(true, std::memory_order_release);
    });

    const int interval_ms = poll_ms > 0 ? poll_ms : 50;
    while (!done->load(std::memory_order_acquire)) {
        if (token->cancelled()) {
            worker.detach(); // 后台自行收尾（结果槽由调用方持有，取消后不再被读取）
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    worker.join();
    return false;
}

bool sleepInterruptible(const CancelToken* token, int ms, int poll_ms)
{
    const int total = ms > 0 ? ms : 0;
    if (token == nullptr) {
        if (total > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(total));
        }
        return false;
    }
    const int interval_ms = poll_ms > 0 ? poll_ms : 50;
    int       slept       = 0;
    while (slept < total) {
        if (token->cancelled()) {
            return true;
        }
        const int slice = (total - slept) < interval_ms ? (total - slept) : interval_ms;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        slept += slice;
    }
    return token->cancelled();
}

} // namespace aiwrite::ai
