#pragma once

// ============================================================================
//  命名管道客户端（C++ ↔ Python 守护进程）—— M7B-02（批 1 · step 2）
//
//  * 方向：**C++ = 客户端**（`CreateFileW`）；Python 守护进程 = 服务器
//    （`source/python/brain_ai_browser/pipe.py` 的 `PipeServer` / `CreateNamedPipeW`）
//  * 线程模型（`M7.md` `Q4`）：`connect()` 后起**读线程**；`call()` 阻塞的是**调用线程**
//    —— 契约：**不得在 UI 线程调 `call()` / `send_command()`**（长任务由执行器工作线程调用）
//  * 分帧：UTF-8 JSON 一行一帧，帧结构复用 `web/channel_frames.h`（§6.1）
//    —— **非法行只丢弃 + 记日志 + 回 `err{bad_frame}`**，不抛异常、不崩 UI
//  * 回包配对：帧头 `id`（事件帧 `"-"`）。`call()` 在「同 `id` 的 `evt` / `err`」上完成；
//    `delta` / `stage` 属**进度事件**（不完成调用 → 走 `on_event`，`M7B-21` 起逐字呈现）
//  * 关闭：`close()` 用 `CancelIoEx` 打断读线程挂起的 overlapped 读 → join → 关句柄
//    （不留悬挂 I/O；`MB-D0-8` L1 的"干净退出"前提）
//  * ⚠️ 句柄**必须**以 `FILE_FLAG_OVERLAPPED` 打开：同步句柄上的并发 I/O 会被 I/O 管理器
//    **串行化** —— 读线程一挂起，写就排队等死（2026-10-03 实测踩到，见 §6.2 step 2 记录）
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "web/channel_frames.h"

namespace aiwrite::web {

// 事件回调：**在读线程上同步调用** ⇒ 回调内不得长时间阻塞、不得再调 `call()` / `close()`
using EventCallback = std::function<void(const channel::Frame&)>;

// 管道全名 `\\.\pipe\aiwrite-browser-<pid>`（与 Python `pipe.daemon_pipe_name` 同构，`P1`）
std::string daemon_pipe_name(std::uint32_t pid);
std::string daemon_pipe_name();  // 缺省 = 本进程 pid

class PipeClient {
public:
    PipeClient();
    ~PipeClient();

    PipeClient(const PipeClient&)            = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // 连接：重试至 `timeout_ms`（管道尚未创建 / 实例忙）；失败时 *error = **可操作原因**（`I21`）
    // `pipe_name` 可给短名（`aiwrite-browser-123`）或全名（`\\.\pipe\…`）；空名不合法
    bool connect(const std::string& pipe_name, int timeout_ms, std::string* error);

    bool        connected() const;
    std::string pipe_name() const;

    // 同步命令：发命令帧 → 等「同 id 的 evt / err」→ *response = 载荷 JSON
    //  * 返回 false 且 *error 可操作（超时 / 通道关闭 / 守护进程回 err）
    //  * 词表校验在**发送前**做（未知命令 / 缺必需字段 → 直接失败，**不发帧、不静默执行**）
    bool call(const std::string& name, const nlohmann::json& fields, nlohmann::json* response,
              int timeout_ms, std::string* error);

    // 只发不等：`shutdown` 的"回包"是**进程退出**（§6.1 / `I23①`），故不配对
    bool send_command(const std::string& name, const nlohmann::json& fields, std::string* error);

    void on_event(EventCallback callback);

    // 断开（幂等）：打断读线程 → join → 关句柄；未完成的 `call()` 立即失败
    void close();

    // 诊断：累计丢弃的非法帧数（对端发来的坏帧）
    std::uint64_t dropped_frames() const;

private:
    struct Pending {
        std::mutex              mutex;
        std::condition_variable cv;
        bool                    done = false;
        channel::Frame          frame;
    };

    void        reader_loop(void* handle, void* read_event);
    void        dispatch(const channel::Frame& frame);
    bool        write_line(const std::string& line, std::string* error);
    bool        complete_pending(const std::string& id, const channel::Frame& frame);
    void        fail_all_pending(const std::string& reason);
    std::string next_id();
    EventCallback current_callback();

    mutable std::mutex          mutex_;
    void*                       handle_ = nullptr;       // HANDLE（头文件不拉 windows.h）
    void*                       read_event_ = nullptr;   // overlapped 读事件（仅读线程使用）
    void*                       write_event_ = nullptr;  // overlapped 写事件（write_mutex_ 串行）
    std::thread                 reader_;
    bool                        connected_ = false;
    bool                        closing_   = false;
    std::string                 pipe_name_;
    std::atomic<std::uint64_t>  next_id_{1};
    std::atomic<std::uint64_t>  dropped_frames_{0};
    std::unordered_map<std::string, std::shared_ptr<Pending>> pending_;
    std::mutex                  callback_mutex_;
    EventCallback               event_callback_;
    std::mutex                  write_mutex_;
};

// ---- IPC 计数（**M7B step 13 · `M7B-46`**：全进程收口 · 「渲染路径零 IPC」护栏）----
//  * 收口点 = `PipeClient`（`connect` / `call` / `send_command`）—— 全仓 IPC 必经此处
//  * **两套计数**（缺一不可）：
//    ① **全局**（`ipc_connect_count` / `ipc_command_count`）= 诊断 / 自检用（**跨线程**累加，
//       如 `--pipe-selftest` 打印它 ⇒ 证明计数在动）；
//    ② **本线程**（`ipc_thread_*`）= **护栏判据**：UI 线程在 `draw_property_panel()` 前后取差值。
//       ⚠️ 必须按**线程归属**判定 —— 后台线程（`WebTask` 登录轮询 / tab 刷新）**并发**发 IPC 时，
//       全局计数会被它们改动 ⇒ 用全局计数当护栏会**误报**。
//  * 2026-10-04 实测卡死根因：面板每帧调 `tab_on_site()` = 新建连接 + `current_tab`，
//    单次 30–140 ms、**最坏 2.1 s** ⇒ 帧率 2–10 fps
//  * 线程安全（`std::atomic` / `thread_local`）；`ipc_reset_counters()` **仅**自检 / 诊断用
std::uint64_t ipc_connect_count();        // 全局：成功建立的连接数（每次短连接 +1）
std::uint64_t ipc_command_count();        // 全局：发出的命令帧数（`call` + `send_command`）
std::uint64_t ipc_thread_connect_count(); // 本线程：其发起的连接数（**护栏判据**）
std::uint64_t ipc_thread_command_count(); // 本线程：其发出的命令帧数（**护栏判据**）
void          ipc_reset_counters();

} // namespace aiwrite::web
