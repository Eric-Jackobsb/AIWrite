#include "web/pipe_client.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <utility>

#include "utils/log.h"

// 实现纪律：**不抛异常**（读线程里的任何失败 → 记日志 + 失败未完成调用），
// 因为调用方可能是执行器工作线程，而 UI 只应看到"错误文本"（`I21`）。
namespace aiwrite::web {
namespace {

constexpr std::size_t kReadChunk      = 4096;
constexpr int         kConnectRetryMs = 50;
constexpr int         kWriteTimeoutMs = 30000;  // 单帧写上限（对端不读时**不无限等**）
// 无换行的行超过该长度 → 交给解析器判非法（与 Python `pipe.read_line` 同策略）
constexpr std::size_t kMaxLineBytes = channel::kMaxFrameBytes + 1;
constexpr const char* kEventId      = "-";  // 事件帧 id（§6.1）

std::wstring to_wide(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                          size);
    return wide;
}

std::string to_utf8(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (size <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(),
                          size, nullptr, nullptr);
    return utf8;
}

std::string win_error_text(unsigned long code)
{
    wchar_t*    buffer = nullptr;
    const DWORD size   = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (size == 0 || buffer == nullptr) {
        return "Win32 错误 " + std::to_string(code);
    }
    std::wstring text(buffer, size);
    ::LocalFree(buffer);
    while (!text.empty() &&
           (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    std::string utf8 = to_utf8(text);
    while (!utf8.empty() && utf8.back() == ' ') {
        utf8.pop_back();
    }
    return utf8;
}

std::string normalize_pipe_name(const std::string& name)
{
    if (name.rfind("\\\\", 0) == 0) {  // 已是 `\\.\pipe\…` 全名
        return name;
    }
    return "\\\\.\\pipe\\" + name;
}

// 进度事件（§6.1 表 2）：不完成配对调用，只喂回调（`M7B-21` 起 `delta` 逐字呈现）
bool is_progress_event(const std::string& name)
{
    return name == "delta" || name == "stage";
}

bool is_pipe_gone(unsigned long code)
{
    return code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED ||
           code == ERROR_NO_DATA || code == ERROR_OPERATION_ABORTED;
}

// ---- IPC 计数（**M7B step 13 · `M7B-46`**：全进程收口 · 「渲染路径零 IPC」护栏）----
//  * 收口点 = 本文件（`connect` / `call` / `send_command`）—— 全仓所有 IPC 都经 `PipeClient`
//  * 计数分**两套**：**全局**（诊断 / 自检打印）与**本线程**（**护栏判据**：UI 线程在
//    `draw_property_panel()` 前后取差值 ⇒ 非 0 即渲染路径发起了 IPC）
//    ⚠️ 护栏**不能**用全局计数 —— 后台线程（`WebTask`）并发发 IPC 会改动它 ⇒ **误报**
//  * （2026-10-04 实测卡死根因：面板每帧调 `tab_on_site()` = 新建连接 + `current_tab`，
//     单次 30–140 ms、最坏 2.1 s ⇒ 帧率 2–10 fps）
} // namespace

namespace {
std::atomic<std::uint64_t> g_ipc_connects{0};   // 全局：成功建立的连接数
std::atomic<std::uint64_t> g_ipc_commands{0};   // 全局：发出的命令帧数（`call` + `send_command`）
// 本线程计数（护栏判据）：`thread_local` ⇒ 后台线程的 IPC **不**污染 UI 线程的差值
thread_local std::uint64_t t_ipc_connects = 0;
thread_local std::uint64_t t_ipc_commands = 0;
} // namespace

std::uint64_t ipc_connect_count()
{
    return g_ipc_connects.load();
}

std::uint64_t ipc_command_count()
{
    return g_ipc_commands.load();
}

std::uint64_t ipc_thread_connect_count()
{
    return t_ipc_connects;
}

std::uint64_t ipc_thread_command_count()
{
    return t_ipc_commands;
}

void ipc_reset_counters()
{
    g_ipc_connects.store(0);
    g_ipc_commands.store(0);
    t_ipc_connects = 0;
    t_ipc_commands = 0;
}

std::string daemon_pipe_name(std::uint32_t pid)
{
    return "\\\\.\\pipe\\aiwrite-browser-" + std::to_string(pid);
}

std::string daemon_pipe_name()
{
    return daemon_pipe_name(static_cast<std::uint32_t>(::GetCurrentProcessId()));
}

PipeClient::PipeClient() = default;

PipeClient::~PipeClient()
{
    close();
}

std::string PipeClient::next_id()
{
    return std::to_string(next_id_.fetch_add(1));
}

EventCallback PipeClient::current_callback()
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    return event_callback_;
}

std::uint64_t PipeClient::dropped_frames() const
{
    return dropped_frames_.load();
}

bool PipeClient::connect(const std::string& pipe_name, int timeout_ms, std::string* error)
{
    close();
    const std::string full = normalize_pipe_name(pipe_name);
    const std::wstring wide = to_wide(full);
    if (wide.empty() || full == "\\\\.\\pipe\\") {
        if (error != nullptr) {
            *error = "管道名不合法（空）";
        }
        return false;
    }

    const ULONGLONG deadline =
        ::GetTickCount64() + static_cast<ULONGLONG>(timeout_ms > 0 ? timeout_ms : 0);
    HANDLE        handle      = INVALID_HANDLE_VALUE;
    unsigned long last_code   = 0;
    for (;;) {
        handle = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            break;
        }
        last_code = ::GetLastError();
        if (last_code != ERROR_FILE_NOT_FOUND && last_code != ERROR_PIPE_BUSY) {
            if (error != nullptr) {
                *error = "连接管道失败：" + win_error_text(last_code) + "（" + full + "）";
            }
            return false;
        }
        if (::GetTickCount64() >= deadline) {
            if (error != nullptr) {
                *error = "等待守护进程管道超时（" + std::to_string(timeout_ms) + " ms，" + full +
                         "）：" + win_error_text(last_code) +
                         "；请确认 Python 守护进程已启动（brain_ai_browser --serve）";
            }
            return false;
        }
        ::Sleep(kConnectRetryMs);
    }

    DWORD mode = PIPE_READMODE_BYTE;
    if (::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr) == 0) {
        aiwrite::log::warn("[管道] SetNamedPipeHandleState 失败（逐行协议不受影响）：" +
                           win_error_text(::GetLastError()));
    }
    // overlapped 事件（auto-reset）：只在 ReadFile/WriteFile 返回 ERROR_IO_PENDING 时才等待
    HANDLE read_event  = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE write_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (read_event == nullptr || write_event == nullptr) {
        const std::string reason = "创建 overlapped 事件失败：" + win_error_text(::GetLastError());
        ::CloseHandle(handle);
        if (read_event != nullptr) {
            ::CloseHandle(read_event);
        }
        if (write_event != nullptr) {
            ::CloseHandle(write_event);
        }
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        handle_      = handle;
        read_event_  = read_event;
        write_event_ = write_event;
        connected_   = true;
        closing_     = false;
        pipe_name_   = full;
    }
    reader_ = std::thread([this, handle, read_event] { reader_loop(handle, read_event); });
    g_ipc_connects.fetch_add(1); // `M7B-46` 护栏：全局（诊断）
    ++t_ipc_connects;            // `M7B-46` 护栏：本线程（判据）
    aiwrite::log::info("[管道] 已连接 " + full);
    return true;
}

bool PipeClient::connected() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

std::string PipeClient::pipe_name() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pipe_name_;
}

bool PipeClient::write_line(const std::string& line, std::string* error)
{
    const auto fail = [error](const std::string& reason) {
        aiwrite::log::warn("[管道] " + reason);
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    };

    std::lock_guard<std::mutex> write_lock(write_mutex_);
    HANDLE                      handle      = nullptr;
    HANDLE                      write_event = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handle      = static_cast<HANDLE>(handle_);
        write_event = static_cast<HANDLE>(write_event_);
        if (handle == nullptr || write_event == nullptr || !connected_) {
            if (error != nullptr) {
                *error = "通道未连接（先调 PipeClient::connect）";
            }
            return false;
        }
    }

    const std::string payload = line + "\n";
    OVERLAPPED        overlapped{};
    overlapped.hEvent = write_event;
    DWORD written     = 0;
    // **必须 overlapped**：同步句柄上的并发 I/O 会被 I/O 管理器串行化 →
    // 读线程的挂起 ReadFile 会把本写排队等死（2026-10-03 实测踩到）
    if (::WriteFile(handle, payload.data(), static_cast<DWORD>(payload.size()), nullptr,
                    &overlapped) == 0) {
        const unsigned long code = ::GetLastError();
        if (code == ERROR_IO_PENDING) {
            if (::WaitForSingleObject(write_event, kWriteTimeoutMs) != WAIT_OBJECT_0) {
                ::CancelIoEx(handle, &overlapped);
                return fail("写管道超时（" + std::to_string(kWriteTimeoutMs) + " ms，对端未读取）");
            }
        } else if (is_pipe_gone(code)) {
            return fail("对端已关闭（写失败）：" + win_error_text(code));
        } else {
            return fail("写管道失败：" + win_error_text(code));
        }
    }
    if (::GetOverlappedResult(handle, &overlapped, &written, FALSE) == 0) {
        const unsigned long code = ::GetLastError();
        return fail(is_pipe_gone(code) ? "对端已关闭（写失败）：" + win_error_text(code)
                                       : "写管道失败：" + win_error_text(code));
    }
    if (written != payload.size()) {
        return fail("写管道不完整（" + std::to_string(written) + "/" +
                    std::to_string(payload.size()) + " 字节）");
    }
    aiwrite::log::info("[管道] 已发出 " + std::to_string(payload.size()) + " 字节：" +
                       line.substr(0, 160));
    return true;
}

bool PipeClient::call(const std::string& name, const nlohmann::json& fields,
                      nlohmann::json* response, int timeout_ms, std::string* error)
{
    g_ipc_commands.fetch_add(1); // `M7B-46` 护栏：全局（入口即计，含词表校验失败 —— 宁可过报）
    ++t_ipc_commands;            // `M7B-46` 护栏：本线程（判据）
    const std::string    id      = next_id();
    const std::string    line    = channel::make_command(name, id, fields.dump());
    const channel::Frame command = channel::parse_frame(line);
    std::string          reason;
    if (!channel::validate_command(command, &reason)) {
        if (error != nullptr) {
            *error = reason;  // 词表校验失败 → **不发帧**（不静默执行）
        }
        return false;
    }

    auto pending = std::make_shared<Pending>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[id] = pending;
    }
    if (!write_line(line, error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(id);
        return false;
    }

    bool finished = false;
    {
        std::unique_lock<std::mutex> lock(pending->mutex);
        finished = pending->cv.wait_for(
            lock, std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 1),
            [&pending] { return pending->done; });
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(id);  // 超时后回包一律丢弃（不假装完成）
    }
    if (!finished) {
        if (error != nullptr) {
            *error = "命令 " + name + " 超时（" + std::to_string(timeout_ms) + " ms）";
        }
        return false;
    }

    const channel::Frame& frame = pending->frame;
    if (frame.kind == "err") {
        if (error != nullptr) {
            *error = frame.name + "：" + frame.payload_json;
        }
        return false;
    }
    if (response != nullptr) {
        *response = frame.payload_json.empty()
                        ? nlohmann::json::object()
                        : nlohmann::json::parse(frame.payload_json, nullptr, false);
        if (response->is_discarded()) {
            *response = nlohmann::json::object();
        }
    }
    return true;
}

bool PipeClient::send_command(const std::string& name, const nlohmann::json& fields,
                              std::string* error)
{
    g_ipc_commands.fetch_add(1); // `M7B-46` 护栏：全局（入口即计，同 `call`）
    ++t_ipc_commands;            // `M7B-46` 护栏：本线程（判据）
    const std::string    id      = next_id();
    const std::string    line    = channel::make_command(name, id, fields.dump());
    const channel::Frame command = channel::parse_frame(line);
    std::string          reason;
    if (!channel::validate_command(command, &reason)) {
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    }
    return write_line(line, error);
}

void PipeClient::on_event(EventCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    event_callback_ = std::move(callback);
}

bool PipeClient::complete_pending(const std::string& id, const channel::Frame& frame)
{
    std::shared_ptr<Pending> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto                  it = pending_.find(id);
        if (it == pending_.end()) {
            return false;
        }
        pending = it->second;
        pending_.erase(it);
    }
    {
        std::lock_guard<std::mutex> lock(pending->mutex);
        pending->frame = frame;
        pending->done  = true;
    }
    pending->cv.notify_all();
    return true;
}

void PipeClient::fail_all_pending(const std::string& reason)
{
    std::unordered_map<std::string, std::shared_ptr<Pending>> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(pending_);
    }
    for (auto& item : pending) {
        {
            std::lock_guard<std::mutex> lock(item.second->mutex);
            // 用词表构造函数生成（**转义安全**，不手拼 JSON）
            item.second->frame =
                channel::parse_frame(channel::make_error("daemon_down", item.first, reason));
            item.second->done = true;
        }
        item.second->cv.notify_all();
    }
}

void PipeClient::dispatch(const channel::Frame& frame)
{
    if (frame.bad) {
        dropped_frames_.fetch_add(1);
        aiwrite::log::warn("[管道] 丢弃非法帧：" + frame.reason);
        // §6.1：非法行 = 丢弃 + 记日志 + 回 err{bad_frame}（不回会让对端一直等）
        std::string ignored;
        write_line(channel::make_error(channel::error_code_bad_frame(), kEventId, "非法帧已丢弃"),
                   &ignored);
        return;
    }
    //  ⚠️ `MB-Q10` 口径（step 4 实测 · 2026-10-03 step 6 同步）：`stage` 帧**带请求 id** 时是
    //  该命令的**完成回包**（`open_tab` 回的就是 `stage{open, ok}`）；只有 `id == "-"` 的才是
    //  **纯进度事件**。若把带 id 的 `stage` 也当进度事件吞掉，`call("open_tab", …)` 会等到超时。
    if (frame.kind == "evt" && is_progress_event(frame.name) && frame.id == kEventId) {
        // 纯进度事件（`id="-"`）：喂回调，**不完成**配对调用（`M7B-21` 起 `delta` 逐字呈现）
        const EventCallback callback = current_callback();
        if (callback) {
            callback(frame);
        }
        return;
    }
    if (frame.kind == "cmd") {
        aiwrite::log::warn("[管道] 收到意料之外的方向（kind=cmd：" + frame.name + "）");
        return;
    }
    aiwrite::log::info("[管道] 收到 " + frame.kind + " " + frame.name + "（id=" + frame.id + "）");
    // evt / err：优先完成配对调用；无配对 → 当「无请求的事件」喂回调
    if (complete_pending(frame.id, frame)) {
        return;
    }
    const EventCallback callback = current_callback();
    if (callback) {
        callback(frame);
    }
}

void PipeClient::reader_loop(void* handle_ptr, void* read_event_ptr)
{
    HANDLE      handle     = static_cast<HANDLE>(handle_ptr);
    HANDLE      read_event = static_cast<HANDLE>(read_event_ptr);
    std::string buffer;
    char        chunk[kReadChunk];
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closing_) {
                break;
            }
        }
        DWORD      read = 0;
        OVERLAPPED overlapped{};
        overlapped.hEvent = read_event;
        if (::ReadFile(handle, chunk, static_cast<DWORD>(sizeof(chunk)), nullptr, &overlapped) == 0) {
            const unsigned long code = ::GetLastError();
            if (code == ERROR_IO_PENDING) {
                // 挂起 → 等完成（close() 会用 CancelIoEx 打断 → 事件被置位）
                if (::WaitForSingleObject(read_event, INFINITE) != WAIT_OBJECT_0) {
                    break;
                }
            } else {
                if (!is_pipe_gone(code)) {
                    aiwrite::log::warn("[管道] 读失败：" + win_error_text(code));
                }
                break;  // 对端已走 / 句柄被取消
            }
        }
        if (::GetOverlappedResult(handle, &overlapped, &read, FALSE) == 0) {
            const unsigned long code = ::GetLastError();
            if (!is_pipe_gone(code)) {
                aiwrite::log::warn("[管道] 读失败：" + win_error_text(code));
            }
            break;
        }
        if (read == 0) {
            break;  // EOF：守护进程退出 / 关闭了管道
        }
        buffer.append(chunk, read);
        std::size_t begin = 0;
        for (;;) {
            const std::size_t end = buffer.find('\n', begin);
            if (end == std::string::npos) {
                break;
            }
            std::string line = buffer.substr(begin, end - begin);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            begin = end + 1;
            if (!line.empty()) {
                dispatch(channel::parse_frame(line));
            }
        }
        if (begin > 0) {
            buffer.erase(0, begin);
        }
        if (buffer.size() > kMaxLineBytes) {
            // 无换行的超长行 → 判非法（缓冲不无限增长）
            const std::string oversize = buffer;
            buffer.clear();
            dispatch(channel::parse_frame(oversize));
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
    }
    fail_all_pending("通道已断开（守护进程已退出或管道被关闭）");
}

void PipeClient::close()
{
    HANDLE      handle      = nullptr;
    HANDLE      read_event  = nullptr;
    HANDLE      write_event = nullptr;
    std::thread reader;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (handle_ == nullptr) {
            return;  // 幂等：未连接 / 已关闭
        }
        closing_     = true;
        connected_   = false;
        handle       = static_cast<HANDLE>(handle_);
        read_event   = static_cast<HANDLE>(read_event_);
        write_event  = static_cast<HANDLE>(write_event_);
        handle_      = nullptr;
        read_event_  = nullptr;
        write_event_ = nullptr;
        reader       = std::move(reader_);
    }
    if (handle != nullptr) {
        // 取消读线程挂起的 overlapped ReadFile（→ 事件置位 → 该线程收尾退出）
        ::CancelIoEx(handle, nullptr);
    }
    if (reader.joinable()) {
        reader.join();
    }
    if (read_event != nullptr) {
        ::CloseHandle(read_event);
    }
    if (write_event != nullptr) {
        ::CloseHandle(write_event);
    }
    if (handle != nullptr) {
        ::CloseHandle(handle);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pipe_name_.clear();
    }
    fail_all_pending("通道已关闭（PipeClient::close）");
}

} // namespace aiwrite::web
