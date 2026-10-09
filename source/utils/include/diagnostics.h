#pragma once

#include <string>

// ============================================================================
//  诊断工具（Debug/Release 均可编译，仅用于定位问题；不参与业务逻辑）
//
//  用途：
//   1) 首异常（first-chance exception）记录：用 VEH 统计异常次数与**抛出模块**，
//      用于定位第三方依赖（COM/WIL 等）反复抛异常的问题。
//      注意：VEH 内**只做原子计数**（不分配、不加锁、不写日志），避免二次异常/死锁；
//            具体信息由 log_exception_summary() 在主循环里输出（带限流）。
//   2) 周期性内存采样：工作集 / 私有字节 / 峰值，用于判断是否存在内存泄漏。
// ============================================================================

namespace aiwrite::utils::diagnostics {

// 安装首异常计数器（进程内只安装一次）
void install_exception_counter();

// 异常计数（自安装以来累计）
long exception_total();

// 输出异常汇总（仅当计数变化时输出；内部限流，例如首条 + 每 100 条）
void log_exception_summary();

// 内存采样：工作集 / 私有字节 / 峰值（MB），tag 为业务侧补充信息（如节点/连线数）
void log_memory_sample(const std::string& tag);

} // namespace aiwrite::utils::diagnostics
