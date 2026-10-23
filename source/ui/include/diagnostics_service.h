#pragma once

// ============================================================================
//  诊断服务（ARCH-A1：应用级服务收口 → AppContext · 六个服务之一）
//
//  架构落点：ARCHITECTURE.md §1.8 `AppContext` / 决策 ARCH-A1
//   * 封装 `utils::diagnostics`（首异常计数 / 内存采样），供主循环每帧调用；
//   * 承载「工作线程投递诊断结论 → 主线程回收」的互斥通道（PB2-07 / PB2-30②）：
//     工作线程只写（post_status），主线程每帧取一次（take_status），
//     未取走时后来的结论**覆盖**前者。
//   * 文件名避让：ui 层不再新增 `diagnostics.h`（与 utils 层同名头冲突），
//     故命名为 `diagnostics_service.h`。
// ============================================================================

#include <mutex>
#include <string>

namespace aiwrite::ui {

class Diagnostics {
public:
    // 安装首异常计数器（进程内只安装一次；启动时调用）
    void install_exception_counter();

    // 输出异常汇总（仅当计数变化时输出；内部限流）
    void log_exception_summary();

    // 内存采样：工作集 / 私有字节 / 峰值（MB），tag 为业务侧补充信息
    void log_memory_sample(const std::string& tag);

    // 工作线程投递结论（覆盖式；互斥保护）
    void post_status(const std::string& text);
    // 主线程回收：有未取走的结论 → 写入 *out 并返回 true（取走后清空标记）
    bool take_status(std::string* out);

private:
    std::mutex  mutex_;
    std::string status_;
    bool        pending_ = false;
};

} // namespace aiwrite::ui
