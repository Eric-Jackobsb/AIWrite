#pragma once

// ============================================================================
//  协作式取消（M_patchAB §2.1 `PB-02` 真取消）—— 纯逻辑 + 线程
//
//  两个东西：
//   ① `CancelToken`：**跨线程共享的置位标志**（`std::atomic<bool>`）；
//      由 `engine::Executor::cancel() / requestStop()` 置位，长耗时调用（HTTP）只读它。
//   ② `CancelWatch`：在**同步阻塞调用期间**轮询令牌 —— 置位即调用 `abort`（调用方给「怎么中断」，
//      例如 `httplib::Client::stop()` ⇒ 主动关 socket，让正在读的线程立刻出错返回）。
//
//  设计要点（对齐 `M_patchAB` §2.1 风险对策）：
//   * 取消是**协作式**：令牌只置位，不强制杀线程；中断动作由调用方显式给出（可断言、可替换）；
//   * 轮询间隔默认 **50 ms** ⇒ `PB-02` 验收「点停止 1 秒内生效」有量化余量；
//   * `aborted()` 只表示「因取消而中断了一次调用」（不代表调用本身失败的原因）；
//   * 本文件**不依赖 httplib / Win32**：`abort` 是 `std::function<void()>` ⇒ `api_probe` 可离线断言。
// ============================================================================

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace aiwrite::ai {

class CancelToken {
public:
    CancelToken() = default;
    CancelToken(const CancelToken&)            = delete;
    CancelToken& operator=(const CancelToken&) = delete;

    void request_cancel() noexcept {
        cancelled_.store(true, std::memory_order_release);
    }
    bool cancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> cancelled_{false};
};

using CancelTokenPtr = std::shared_ptr<CancelToken>;

// 取消监视（同步调用期间）：置位 ⇒ 调用一次 `abort`（幂等：只调一次）
class CancelWatch {
public:
    // `token` 为空 ⇒ 只做占位（`aborted()` 恒 false、不开线程）
    CancelWatch(const CancelToken* token, std::function<void()> abort, int poll_ms = 50);
    ~CancelWatch();

    CancelWatch(const CancelWatch&)            = delete;
    CancelWatch& operator=(const CancelWatch&) = delete;

    // 本次监视期间是否**因取消**触发过中断（供调用方把失败归因为「已取消」）
    bool aborted() const noexcept { return aborted_.load(std::memory_order_acquire); }
    // 取消令牌当前是否已置位（调用方在返回前复核）
    bool cancelled() const noexcept { return token_ != nullptr && token_->cancelled(); }

private:
    const CancelToken*    token_ = nullptr;
    std::function<void()> abort_;
    std::atomic<bool>     aborted_{false};
    std::atomic<bool>     stop_{false};
    std::thread           worker_;
};

// ---- 可取消的**同步调用**执行器（PB-02 的主路径）----
//  实测结论（2026-10-11 · httplib 0.15.3 · `AB-01` 的离线复现）：`httplib::Client::stop()`
//  **不能**唤醒另一线程中阻塞的读（`shutdown(SD_BOTH)` 后读仍等到服务端响应才返回）
//  ⇒ 仅靠 socket 层中断无法满足「点停止 1 秒内生效」。因此主路径改用本函数：
//    * `work` 在**工作线程**执行；调用方每 `poll_ms` 轮询一次取消令牌；
//    * 取消 ⇒ **立即返回 true**（调用方据此归因「已取消」），工作线程**自行跑完并收尾**；
//    * 线程结束时把结果写入调用方传入的 shared_ptr 槽（调用方取消后不再读取它）。
//  ⚠️ 契约（重要）：`work` 用到的**所有对象必须由 work 自己持有**（值捕获 / `shared_ptr`），
//     因为取消后 work 可能仍在使用它们 —— 栈上引用一旦返回就会悬垂。
bool runCancellable(const CancelToken* token, std::function<void()> work, int poll_ms = 50);

// 可中断等待（重试间隔 / 退避用）：置位令牌 ⇒ **立即返回 true**（调用方据此放弃等待）
//  * `token == nullptr` ⇒ 普通睡眠后返回 false（与改造前行为一致）
bool sleepInterruptible(const CancelToken* token, int ms, int poll_ms = 50);

} // namespace aiwrite::ai
