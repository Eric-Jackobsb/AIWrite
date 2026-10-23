#include "diagnostics.h"

#include "log.h"

// WIN32_LEAN_AND_MEAN / NOMINMAX 已由 CMake 全局定义（见 CMakeLists 的 aiwrite_build_options）
#include <windows.h>

#include <psapi.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace aiwrite::utils::diagnostics {
namespace {

// ARCH-A7：原先 7 条文件级 static ⇒ 归拢为显式状态对象（单一定义点 = `exceptionCounter()`）。
// 语义不变：全部 `relaxed`（这些计数只用于限流输出 / 诊断行，不参与同步判定）。
struct ExceptionCounter {
    std::atomic<long>          exception_total{0};
    std::atomic<void*>         first_address{nullptr};
    std::atomic<void*>         last_address{nullptr};
    std::atomic<unsigned long> first_code{0};
    std::atomic<unsigned long> last_code{0};
    std::atomic<long>          logged_total{0};
    bool                       installed = false; // VEH 只装一次
};

ExceptionCounter& exceptionCounter()
{
    static ExceptionCounter counter;
    return counter;
}

// 把地址解析成"模块名+偏移"（在 VEH 之外调用，避免异常上下文里碰 loader）
std::string describe_address(void* address)
{
    if (address == nullptr) {
        return "<none>";
    }
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &module) ||
        module == nullptr) {
        return "<unknown>";
    }

    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* name = std::strrchr(path, '\\');
    const std::string module_name = (name != nullptr) ? std::string(name + 1) : std::string(path);

    const auto offset = reinterpret_cast<std::uintptr_t>(address) -
                        reinterpret_cast<std::uintptr_t>(module);
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "+0x%llX",
                  static_cast<unsigned long long>(offset));
    return module_name + buffer;
}

// 【VEH 内只做原子操作】不分配内存、不加锁、不写日志 —— 否则在异常上下文中可能二次异常或死锁
LONG WINAPI first_chance_exception_handler(EXCEPTION_POINTERS* info)
{
    if (info == nullptr || info->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const unsigned long code = static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode);
    // 0xE06D7363 = MSVC C++ 异常（wil::ResultException 等）；0xC0000005 = 访问冲突
    if (code == 0xE06D7363UL || code == 0xC0000005UL) {
        void* address = info->ExceptionRecord->ExceptionAddress;
        exceptionCounter().exception_total.fetch_add(1, std::memory_order_relaxed);
        if (exceptionCounter().first_address.load(std::memory_order_relaxed) == nullptr) {
            exceptionCounter().first_address.store(address, std::memory_order_relaxed);
            exceptionCounter().first_code.store(code, std::memory_order_relaxed);
        }
        exceptionCounter().last_address.store(address, std::memory_order_relaxed);
        exceptionCounter().last_code.store(code, std::memory_order_relaxed);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void installExceptionCounter()
{
    if (exceptionCounter().installed) {
        return;
    }
    exceptionCounter().installed = true;
    // 1 = 最先调用（在 SEH/调试器处理之前）
    ::AddVectoredExceptionHandler(1, first_chance_exception_handler);
    log::info("[诊断] 首异常计数器已安装（VEH；汇总输出见下）");
}

long exceptionTotal()
{
    return exceptionCounter().exception_total.load(std::memory_order_relaxed);
}

void logExceptionSummary()
{
    const long total = exceptionTotal();
    const long logged = exceptionCounter().logged_total.load(std::memory_order_relaxed);
    if (total == logged) {
        return; // 无新异常
    }

    // 限流：首条 + 每 100 条输出一次
    const bool should_log = (logged == 0) || (total / 100 > logged / 100);
    if (!should_log) {
        return;
    }
    exceptionCounter().logged_total.store(total, std::memory_order_relaxed);

    char code_buffer[32] = {};
    std::snprintf(code_buffer, sizeof(code_buffer), "0x%08lX", exceptionCounter().last_code.load());

    log::warn("[诊断] 首异常累计 " + std::to_string(total) + " 次；首个于 " +
              describe_address(exceptionCounter().first_address.load()) + "（代码 " + code_buffer + "），" +
              "最近一次于 " + describe_address(exceptionCounter().last_address.load()) + "（限流输出）");
}

void logMemorySample(const std::string& tag)
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(),
                               reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                               sizeof(counters))) {
        log::warn("[诊断] 内存采样失败（GetProcessMemoryInfo）");
        return;
    }

    char buffer[192] = {};
    std::snprintf(buffer, sizeof(buffer),
                  "[诊断] 内存: 工作集 %.1f MB / 私有 %.1f MB / 峰值 %.1f MB%s%s",
                  static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0),
                  static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0),
                  static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0),
                  tag.empty() ? "" : "  |  ", tag.empty() ? "" : tag.c_str());
    log::info(buffer);
}

} // namespace aiwrite::utils::diagnostics
