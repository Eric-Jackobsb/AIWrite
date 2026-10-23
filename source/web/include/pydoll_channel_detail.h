#pragma once

// ============================================================================
//  新通道（Pydoll 守护进程）—— **模块内契约**(ARCH-Z1b)
//
//  * 只允许 `web/src/pydoll_channel*.cpp` 自己 include；**不是**公开 API
//  * 公开语义入口 = `pydoll_channel.h`
//  * 逐字搬迁（零语义变化）：结构体 / 常量 / 函数体均自原 1282 行单文件原样搬出
//  * 拆分版图：
//      `pydoll_channel.cpp`          基础设施（包目录/解释器/管道名/守护进程）+ 会话生命周期 + 按站点动作
//      `pydoll_channel_commands.cpp` 内容返回（run_script / connect_session / tab 判定 / joinNames）
//      `pydoll_channel_selftest.cpp` 自检（--pydoll-selftest / --pydoll-script-selftest / --pydoll-chat-selftest）与 --pydoll-login
// ============================================================================

#include "pydoll_channel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <windows.h>

#include "provider_spec.h"
#include "log.h"
#include "paths.h"
#include "channel_frames.h"
#include "pipe_client.h"
#include "session_store.h"

namespace aiwrite::web::channel {


struct DaemonProcess {
    void*         process = nullptr;  // HANDLE（不把 windows.h 暴露到接口）
    void*         thread  = nullptr;
    std::uint32_t pid     = 0;
    std::string   python;             // 物证：实际用的解释器
    std::string   package_dir;        // 物证：实际用的包目录
};

// ARCH-A7：原先 3 条文件级 static（`g_session` / `g_session_pipe` / `g_cancel_polls`）
// ⇒ 显式状态对象，单一定义点 = `sessionState()`。
struct SessionState {
    DaemonProcess     process;     // 常驻会话的守护进程（`process == nullptr` = 无会话）
    std::string       pipe_name;   // 会话管道名（= `daemon_pipe_name()`）
    // **M7B step 12（`M7B-44`）**：登录态轮询的取消位（见 `request_cancel_session_ops()`）
    //  * 由 `ui::wait_web_tasks()`（退出流程）置位；`login_site` / `pydoll_login` 每轮检查 ⇒ 尽早结束
    //  * **不**用于中断浏览器命令本身（单条命令超时仍由各自的 `timeout_ms` 兜底）
    std::atomic<bool> cancel_polls{false};
};


// ---- 跨 TU 入口（逐字搬迁，签名不变）----
std::string channel_pipe_name(const char* tag);
bool        spawn_daemon(const std::string& pipeName, bool once, int idle_timeout_s,
                         DaemonProcess* out, std::string* error);
int         wait_daemon(const DaemonProcess& daemon, int timeout_ms);
void        release_daemon(DaemonProcess& daemon);
SessionState& sessionState();
std::string joinNames(const std::vector<std::string>& names);

} // namespace aiwrite::web::channel

