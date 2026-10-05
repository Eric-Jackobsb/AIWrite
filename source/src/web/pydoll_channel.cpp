// 新通道：Pydoll 守护进程（命名管道）—— 实现（M7B 批 2 · `M7B-15` / `M7B-16`）
//
//  * 本批**不接线**：只服务 `--pydoll-selftest` / `--pydoll-login`（批 2 冒烟 · `M7B-19` 门槛）
//  * 守护进程启动逻辑与 `main.cpp::pipe_selftest()` **同构**（同一套候选与 `CreateProcess` 语义）；
//    统一收口（抽公共函数）归 `M7B-18` 诊断换代 —— 本批各自独立，**不动既有自检路径**
//  * 一切失败都返回**可操作原因**（`I21`）：无 Python / 无包目录 / 守护进程未起 / 命令超时

#include "web/pydoll_channel.h"

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

#include "ai/provider_spec.h"
#include "utils/log.h"
#include "utils/paths.h"
#include "web/channel_frames.h"
#include "web/pipe_client.h"
#include "web/session_store.h"

namespace aiwrite::web::channel {
namespace {

std::wstring widen(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

// ---- 包目录 / 解释器搜索（开发态 + 部署态都要能找到）----

std::filesystem::path find_package_dir()
{
    std::error_code                    ec;
    std::vector<std::filesystem::path> candidates;
#ifdef AIWRITE_SOURCE_DIR
    candidates.emplace_back(std::filesystem::path(AIWRITE_SOURCE_DIR) / "python");
#endif
    candidates.emplace_back(paths::exe_dir() / "python");
    // 开发态：从 exe 目录**向上**找 `source/python`（exe 通常在 <仓库根>/build/bin）≤ 6 层
    std::filesystem::path dir = paths::exe_dir();
    for (int depth = 0; depth < 6 && !dir.empty(); ++depth) {
        candidates.emplace_back(dir / "source" / "python");
        candidates.emplace_back(dir / "python");
        dir = dir.parent_path();
    }
    // ① **优先**「包目录 + 同级 `.venv/Scripts/python.exe`」—— 那才是**真能跑**（含 pydoll）的那一份。
    //    实测教训（step 6）：只按「包目录存在」挑，会选到 `<exe>/python` 的拷贝 —— 它旁边**没有**
    //    `.venv`，于是退回 PATH 上的 `python.exe`（本机 3.14.3 · **无 pydoll**）⇒ 生产登录必失败。
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "brain_ai_browser" / "__main__.py", ec) &&
            std::filesystem::exists(candidate / ".venv" / "Scripts" / "python.exe", ec)) {
            return candidate;
        }
    }
    // ② 其次：只要包目录在即可（部署态：解释器交 PATH 或 `<exe>/python/.venv`；缺依赖由守护进程
    //    回 `error{no_python}` —— `I21` 不静默）
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "brain_ai_browser" / "__main__.py", ec)) {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path find_python(const std::filesystem::path& package_dir)
{
    std::error_code             ec;
    const std::filesystem::path candidates[] = {
        package_dir / ".venv" / "Scripts" / "python.exe",
        paths::exe_dir() / "python" / ".venv" / "Scripts" / "python.exe",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};  // 空 = 交给 PATH 上的 `python.exe`
}

std::string channel_pipe_name(const char* tag)
{
    return "aiwrite-browser-" + std::string(tag) + "-" + std::to_string(::GetCurrentProcessId());
}

struct DaemonProcess {
    void*         process = nullptr;  // HANDLE（不把 windows.h 暴露到接口）
    void*         thread  = nullptr;
    std::uint32_t pid     = 0;
    std::string   python;             // 物证：实际用的解释器
    std::string   package_dir;        // 物证：实际用的包目录
};

// 起守护进程：`python -m brain_ai_browser --serve [--once] --pipe-name <名> --idle-timeout <n>`
//  * stdout/stderr → `<日志目录>/pydoll_channel_daemon.log`（诊断可查）
//  * stdin → NUL（避免部分 Python 构建对空句柄敏感）
bool spawn_daemon(const std::string& pipe_name, bool once, int idle_timeout_s,
                  DaemonProcess* out, std::string* error)
{
    const std::filesystem::path package_dir = find_package_dir();
    if (package_dir.empty()) {
        if (error != nullptr) {
            *error = "找不到 `brain_ai_browser` 包目录（已查 <exe>/python 与向上各级 source/python）"
                     "—— 开发态请确认 `source/python/brain_ai_browser/__main__.py` 就位；"
                     "部署态请确认打包已把 `python/` 拷到 exe 同级目录";
        }
        return false;
    }
    const std::filesystem::path python_exe = find_python(package_dir);
    const std::string           python_text =
        python_exe.empty() ? std::string("python.exe") : python_exe.string();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    const std::filesystem::path log_path = paths::logs_dir() / "pydoll_channel_daemon.log";
    std::error_code             dir_ec;
    std::filesystem::create_directories(log_path.parent_path(), dir_ec);
    HANDLE log_handle = ::CreateFileW(widen(log_path.string()).c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log_handle == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = "无法创建守护进程日志：" + log_path.string() + "（磁盘 / 权限问题）";
        }
        return false;
    }
    HANDLE nul_handle =
        ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);

    std::wstring command = L"\"" + widen(python_text) + L"\" -m brain_ai_browser --serve";
    if (once) {
        command += L" --once";
    }
    command += L" --pipe-name " + widen(pipe_name) + L" --idle-timeout " +
               std::to_wstring(idle_timeout_s);

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = nul_handle;
    si.hStdOutput = log_handle;
    si.hStdError  = log_handle;
    PROCESS_INFORMATION pi{};
    const std::wstring  work_dir = widen(package_dir.string());

    const BOOL spawned = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW, nullptr,
                                          work_dir.empty() ? nullptr : work_dir.c_str(), &si, &pi);
    const DWORD spawn_error = spawned ? 0 : ::GetLastError();
    ::CloseHandle(log_handle);
    if (nul_handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(nul_handle);
    }
    if (spawned == 0) {
        if (error != nullptr) {
            *error = "无法启动 Python 守护进程（错误码 " + std::to_string(spawn_error) +
                     "，解释器 " + python_text + "）—— 请按 `source/python/requirements.txt`"
                     " 确认 Python 3.12 + pydoll 已就位";
        }
        return false;
    }
    if (out != nullptr) {
        out->process     = static_cast<void*>(pi.hProcess);
        out->thread      = static_cast<void*>(pi.hThread);
        out->pid         = pi.dwProcessId;
        out->python      = python_text;
        out->package_dir = package_dir.string();
    } else {
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
    }
    return true;
}

// 等守护进程退出；返回退出码（-1 = 期限内未退出 —— **不强杀**，交调用方按 `I23①` 口径处置）
int wait_daemon(const DaemonProcess& daemon, int timeout_ms)
{
    if (daemon.process == nullptr) {
        return -1;
    }
    const DWORD waited =
        ::WaitForSingleObject(static_cast<HANDLE>(daemon.process), static_cast<DWORD>(timeout_ms));
    if (waited != WAIT_OBJECT_0) {
        return -1;
    }
    DWORD code = 0;
    ::GetExitCodeProcess(static_cast<HANDLE>(daemon.process), &code);
    return static_cast<int>(code);
}

// 关句柄（**不 kill**：守护进程自会随 `shutdown` / 空闲超时退出；这里只回收句柄资源）
void release_daemon(DaemonProcess& daemon)
{
    if (daemon.thread != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(daemon.thread));
        daemon.thread = nullptr;
    }
    if (daemon.process != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(daemon.process));
        daemon.process = nullptr;
    }
}

} // namespace

// ============================================================================
//  对外接口（批 2：只落地 selftest / ensure_session / pydoll_login）
// ============================================================================

int selftest(int timeout_ms)
{
    const std::string pipe_name = channel_pipe_name("pydoll-selftest");
    std::string       error;
    DaemonProcess     daemon;

    log::info("[新通道] 自检：起守护进程（pipe=" + pipe_name + "）");
    if (!spawn_daemon(pipe_name, /*once=*/true, 20, &daemon, &error)) {
        log::error("[新通道] 自检失败（依赖问题）：" + error);
        std::printf("[新通道] 依赖问题：%s\n", error.c_str());
        return 2;
    }
    std::printf("[新通道] 守护进程已起（pid=%u · 解释器=%s）· 等待管道…\n",
                static_cast<unsigned>(daemon.pid), daemon.python.c_str());

    int        exit_code = 1;
    PipeClient client;
    if (!client.connect(pipe_name, timeout_ms, &error)) {
        log::error("[新通道] 自检失败：连接管道超时 —— " + error);
        std::printf("[新通道] 失败：%s\n", error.c_str());
    } else {
        nlohmann::json hello;
        if (!client.call("hello", nlohmann::json::object(), &hello, kTimeoutStateMs, &error)) {
            log::error("[新通道] 自检失败：hello 未回包 —— " + error);
            std::printf("[新通道] 失败：%s\n", error.c_str());
        } else {
            const int         proto   = hello.value("proto", 0);
            const std::string python  = hello.value("python", "?");
            const std::string browser = hello.value("browser", "?");
            std::printf("[新通道] ready：proto=%d python=%s browser=%s\n", proto, python.c_str(),
                        browser.c_str());
            if (proto != kProtoVersion) {
                log::error("[新通道] 自检失败：协议版本不匹配（期望 " +
                           std::to_string(kProtoVersion) + "，收到 " + std::to_string(proto) + "）");
            } else {
                std::string ignored;
                client.send_command("shutdown", nlohmann::json::object(), &ignored);
                const int code = wait_daemon(daemon, 60'000);
                std::printf("[新通道] 守护进程退出码=%d（0 = 干净收尾 · 未强杀）\n", code);
                exit_code = (code == 0) ? 0 : 1;
            }
        }
        client.close();
    }
    release_daemon(daemon);
    // **M7B step 13（`M7B-46`）**：IPC 计数实测值（「渲染路径零 IPC」护栏的数字来源）
    //  * 本自检**必然 >0**（`hello` + `shutdown` 各一条命令）⇒ 证明计数**真的在动**（不是死码）
    //  * 判据：`draw_property_panel()` 前后这两个数必须**不增**（渲染路径零 IPC）
    std::printf("[新通道] IPC 计数：连接=%llu 命令=%llu\n",
                static_cast<unsigned long long>(ipc_connect_count()),
                static_cast<unsigned long long>(ipc_command_count()));

    std::printf("[新通道] 守护进程日志：%s\n",
                (paths::logs_dir() / "pydoll_channel_daemon.log").string().c_str());
    return exit_code;
}

// 夹具探测脚本：读 `title` + 枚举 `input`（**只读**，与 `--web-dom-dump` 同族形态）
//  ⚠️ 必须带**顶层 `return`**：pydoll 的 `execute_script` 把脚本文本按**函数体**执行 ——
//     写成 `(function(){…})()` 这种「表达式」形态没有顶层 return ⇒ 结果被丢弃（实测回 `null`）。
static constexpr const char* kDomProbeScript =
    "return (function(){var els=document.querySelectorAll('input');"
    "return {title:document.title,count:els.length,"
    "ids:Array.prototype.map.call(els,function(e){return e.id||e.name||e.type;})};})();";

// ---- `--pydoll-script-selftest`：跨语言端到端（C++ → 管道 → Python → 浏览器 → JS）----
int script_selftest(int timeout_seconds)
{
    const int timeout_ms = timeout_seconds > 0 ? timeout_seconds * 1000 : 60'000;

    // ① 夹具：本地临时 HTML（`file:///`）—— 零外网、零登录（同 `--session-selftest` 的 `.invalid` 思路）
    std::error_code             ec;
    const std::filesystem::path page =
        std::filesystem::temp_directory_path(ec) / "aiwrite-script-selftest.html";
    {
        std::ofstream out(page, std::ios::binary | std::ios::trunc);
        out << "<!doctype html><html><head><meta charset=\"utf-8\">"
               "<title>aiwrite-script-selftest</title></head><body>"
               "<input id=\"user\"><input name=\"pass\" type=\"password\">"
               "</body></html>";
    }
    std::string page_path = page.string();
    std::replace(page_path.begin(), page_path.end(), '\\', '/');
    const std::string url = "file:///" + page_path;

    // ② 起守护进程（**独立管道名**：不与生产守护进程 / 其他自检互相干扰）
    const std::string pipe_name = channel_pipe_name("script-selftest");
    std::string       error;
    DaemonProcess     daemon;
    if (!spawn_daemon(pipe_name, /*once=*/true, 30, &daemon, &error)) {
        std::printf("[新通道] 依赖问题：%s\n", error.c_str());
        return 2;
    }
    std::printf("[新通道] 脚本自检：守护进程已起（pid=%u · 解释器=%s）\n",
                static_cast<unsigned>(daemon.pid), daemon.python.c_str());
    std::printf("[新通道] 夹具：%s\n", url.c_str());

    int    passed = 0;
    int    failed = 0;
    const auto check = [&passed, &failed](bool ok, const char* name, const std::string& detail) {
        if (ok) {
            ++passed;
            std::printf("   PASS  %s\n", name);
        } else {
            ++failed;
            std::printf("   FAIL  %s :: %s\n", name, detail.c_str());
        }
    };

    int        exit_code = 1;
    bool       closed    = false;
    PipeClient client;
    if (!client.connect(pipe_name, timeout_ms, &error)) {
        std::printf("[新通道] 失败：连接管道超时 —— %s\n", error.c_str());
    } else {
        nlohmann::json hello;
        if (!client.call("hello", nlohmann::json::object(), &hello, kTimeoutStateMs, &error)) {
            std::printf("[新通道] 失败：hello 未回包 —— %s\n", error.c_str());
        } else if (hello.value("proto", 0) != kProtoVersion) {
            std::printf("[新通道] 失败：协议版本不匹配（期望 %d，收到 %d）\n", kProtoVersion,
                        hello.value("proto", 0));
        } else {
            std::printf("[新通道] ready：proto=%d python=%s\n", hello.value("proto", 0),
                        hello.value("python", "?").c_str());
            nlohmann::json opened;
            const bool     open_ok =
                client.call("open_tab", {{"provider", "script-selftest"}, {"url", url}}, &opened,
                            kTimeoutOpenMs, &error);
            check(open_ok, "M7B-42① `open_tab` 本地夹具（file:/// · 零外网 · 零登录）", error);

            // ③ `run_script` 读 DOM（`--web-dom-dump` / `--web-adapter-selftest` 换代后要依赖的能力）
            nlohmann::json dom;
            const bool     dom_ok = client.call(
                "run_script", {{"provider", "script-selftest"}, {"script", kDomProbeScript}}, &dom,
                timeout_ms, &error);
            const std::string dom_text =
                dom_ok ? dom.value("result", nlohmann::json::object()).dump() : std::string();
            check(dom_ok && dom_text.find("aiwrite-script-selftest") != std::string::npos &&
                      dom_text.find("\"user\"") != std::string::npos &&
                      dom_text.find("\"pass\"") != std::string::npos,
                  "M7B-42② `run_script` 读到夹具 DOM（title + 两个 input 的 id / name）",
                  error + " | " + dom_text);
            std::printf("   ----  脚本返回：%s\n", dom_text.substr(0, 200).c_str());

            // ④ 类型保真（数组 / 字符串 / 布尔 / null 往返 ⇒ 结构化结果可用）
            nlohmann::json types;
            const bool     types_ok =
                client.call("run_script",
                            {{"provider", "script-selftest"}, {"script", "return [1,'two',true,null];"}},
                            &types, timeout_ms, &error);
            const std::string types_text =
                types_ok ? types.value("result", nlohmann::json::object()).dump() : std::string();
            check(types_ok && types_text.find("\"two\"") != std::string::npos &&
                      types_text.find("true") != std::string::npos,
                  "M7B-42③ `run_script` 类型保真（数组 / 字符串 / 布尔 / null 往返）",
                  error + " | " + types_text);

            // ⑤ 空脚本 → **可操作错误**（`script_error`；不假装成功 · `I21`）
            nlohmann::json empty;
            std::string    empty_error;
            const bool     empty_ok =
                client.call("run_script", {{"provider", "script-selftest"}, {"script", "   "}},
                            &empty, kTimeoutStateMs, &empty_error);
            check(!empty_ok && empty_error.find("script") != std::string::npos,
                  "M7B-42④ 空脚本 → 可操作错误（`script_error`；I21 不假装成功）", empty_error);

            // ⑥ 收尾：`shutdown`（**同一连接**：`--once` 只服务一条）→ 等进程退出（`I23①`）
            //  ⚠️ 发完 shutdown **不要立刻 close 句柄**：客户端先断开会让守护进程读循环撞到
            //     `pipe_error`（`daemon._serve_async`）⇒ **退出码 1**。
            //     实测对照：`--pydoll-selftest` 发完就 `wait_daemon`（不 close）⇒ 退出码 0。
            std::string ignored;
            client.send_command("shutdown", nlohmann::json::object(), &ignored);
            closed    = true;
            exit_code = (failed == 0) ? 0 : 1;
        }
        if (!closed) {
            client.close();     // 仅异常路径：断开让守护进程按空闲超时退出
        }
    }
    if (!closed) {
        std::printf("[新通道] 未正常收尾：等待守护进程按空闲超时退出…\n");
    }
    const int daemon_code = wait_daemon(daemon, 90'000);
    std::printf("[新通道] 守护进程退出码=%d（0 = 干净收尾 · 未强杀）\n", daemon_code);
    if (exit_code == 0 && daemon_code != 0) {
        exit_code = 1;
    }
    release_daemon(daemon);
    std::filesystem::remove(page, ec);
    std::printf("[新通道] 脚本自检结果：%d 通过 / %d 失败（退出码 %d）\n", passed, failed, exit_code);
    std::printf("[新通道] 守护进程日志：%s\n",
                (paths::logs_dir() / "pydoll_channel_daemon.log").string().c_str());
    return exit_code;
}

// ---- 会话生命周期（批 3 step 9 · `M7B-18`）：诊断工具**自己起会话 / 自己收尾** ----
namespace {
DaemonProcess g_session;        // 常驻会话的守护进程（`process == nullptr` = 无会话）
std::string   g_session_pipe;   // 会话管道名（= `daemon_pipe_name()`）

// **M7B step 12（`M7B-44`）**：登录态轮询的取消位（见 `request_cancel_session_ops()`）
//  * 由 `ui::wait_web_tasks()`（退出流程）置位；`login_site` / `pydoll_login` 每轮检查 ⇒ 尽早结束
//  * **不**用于中断浏览器命令本身（单条命令超时仍由各自的 `timeout_ms` 兜底）
std::atomic<bool> g_cancel_polls{false};

// 确保**常驻会话**在跑（复用已有 or 起守护进程）；返回会话管道名（失败 → 空串 + *error）
//  * 「已有会话」= 本进程已记账（`session_ready()`），**或**管道能连上（本进程外起过）
//  * 起守护进程一律 `once=false`：会话要跨多条命令（open_tab → run_script → … → shutdown）
//  * 本函数是**等价抽取**（自 step 9 起就是这段逻辑）；step 11 只为 `login_site` 复用而抽出
std::string ensure_daemon_session(std::string* error)
{
    const std::string pipe_name = daemon_pipe_name();
    if (!session_ready()) {
        bool alive = false;
        {
            PipeClient  probe;
            std::string probe_error;
            if (probe.connect(pipe_name, 300, &probe_error)) {
                alive = true;
                probe.close();
            }
        }
        if (!alive) {
            DaemonProcess daemon;
            std::string   spawn_error;
            if (!spawn_daemon(pipe_name, /*once=*/false, 600, &daemon, &spawn_error)) {
                if (error != nullptr) {
                    *error = spawn_error;
                }
                return {};
            }
            g_session      = daemon;
            g_session_pipe = pipe_name;
            log::info("[新通道] 会话已起（pid=" + std::to_string(daemon.pid) + " · 管道 " +
                      pipe_name + "）");
        }
    }
    return pipe_name;
}

// 把「已登录」证据写进内存 `SessionStore`（`I15′`：只记**名单** —— 无 Cookie 值、不落盘；
// 加密快照归 L2 `session.py`）
void remember_session(const SiteRef& site, const std::vector<std::string>& names)
{
    Session session;
    session.logged_in   = true;
    session.site        = login_request_site(site);
    session.provider_id = site.provider_id;
    session.url         = site.url;
    for (const std::string& name : names) {
        Cookie cookie;
        cookie.name = name;
        session.cookies.push_back(cookie);
    }
    SessionStore::instance().set(session);
}

// 调 `login_state`（`I15′`）：
//  * 返回 `false` = **命令失败**（*error 可操作）；
//  * 返回 `true` 时：`*logged_in` = 是否判到已登录；`*names` = 命中名单；`*verdict` = 原判定串
bool query_login_state(PipeClient& client, const SiteRef& site, int timeout_ms, bool* logged_in,
                       std::vector<std::string>* names, std::string* verdict, std::string* error)
{
    nlohmann::json state;
    std::string    state_error;
    if (!client.call("login_state",
                     {{"provider", site.provider_id}, {"cookie_names", site.cookie_names}}, &state,
                     timeout_ms, &state_error)) {
        if (error != nullptr) {
            *error = "登录态观测失败：" + state_error;
        }
        return false;
    }
    if (verdict != nullptr) {
        *verdict = state.value("state", std::string("unknown"));
    }
    if (logged_in != nullptr) {
        *logged_in = state.value("state", std::string("unknown")) == "logged_in";
    }
    if (names != nullptr) {
        *names = state.value("cookie_names", std::vector<std::string>{});
    }
    return true;
}
} // namespace

// **M7B step 12（`M7B-44`）**：请求取消**正在进行的登录态轮询**（`login_site` / `pydoll_login`）
//  * 面板关窗 / 进程退出时由 `ui::wait_web_tasks()` 调用 —— 否则退出要把登录上限（120 s）干等完
//  * **幂等 · 线程安全**；只改**轮询循环的继续条件**，不动会话、不发任何命令
//  * 下一次 `login_site` / `pydoll_login` 进入时会**自动复位**（一次取消只作用于在跑的那一次）
void request_cancel_session_ops()
{
    g_cancel_polls = true;
    log::info("[新通道] 已请求取消登录态轮询（等待中的 `login_site` / `pydoll_login` 将尽早结束）");
}

bool session_ready()
{
    return g_session.process != nullptr;
}

bool shutdown_session()
{
    if (g_session.process == nullptr) {
        return false;   // 幂等：本来就没有会话
    }
    std::string ignored;
    PipeClient  closer;     // ⚠️ 作用域必须**盖过** `wait_daemon`：句柄先断会变 `pipe_error`
    if (closer.connect(g_session_pipe, 2'000, &ignored)) {
        closer.send_command("shutdown", nlohmann::json::object(), &ignored);
        // 发完**不主动 close**（照 `--pydoll-selftest` 做法）
    }
    const int code = wait_daemon(g_session, 60'000);
    if (code != 0) {
        log::warn("[新通道] 会话收尾非干净退出（退出码 " + std::to_string(code) +
                  "）—— 详见 pydoll_channel_daemon.log");
    }
    release_daemon(g_session);
    g_session_pipe.clear();
    return true;
}

// ---- 确保会话（`M7B-18` 换代：**只读判定** → 「确保有活会话且已导航到站点」）----
bool ensure_session(const SiteRef& site, int timeout_ms, std::string* error)
{
    const int boot_ms = timeout_ms > 0 ? timeout_ms : kTimeoutOpenMs;

    // ① 复用已有会话？（管道能连上 = 有活守护进程；连不上 → 自己起一个）
    const std::string pipe_name = ensure_daemon_session(error);
    if (pipe_name.empty()) {
        return false;   // *error 已由 helper 填好（可操作原因，`I21`）
    }

    PipeClient  client;
    std::string connect_error;
    if (!client.connect(pipe_name, boot_ms, &connect_error)) {
        if (error != nullptr) {
            *error = "会话管道连不上（" + pipe_name + "）：" + connect_error;
        }
        return false;
    }

    // ② `open_tab`：导航到**调用方给的** url（`I14`：无站点回落）
    if (!site.url.empty()) {
        nlohmann::json opened;
        std::string    open_error;
        if (!client.call("open_tab", {{"provider", site.provider_id}, {"url", site.url}}, &opened,
                         kTimeoutOpenMs, &open_error)) {
            if (error != nullptr) {
                *error = "打开站点页失败（" + site.url + "）：" + open_error;
            }
            client.close();
            return false;
        }
    }

    // ③ `I15′` 登录态判定（**未登录不算致命**：只读诊断脚本照跑 —— 由调用方决定）
    bool                     logged_in = false;
    std::vector<std::string> names;
    std::string              verdict;
    std::string              state_error;
    const bool               state_ok =
        query_login_state(client, site, boot_ms, &logged_in, &names, &verdict, &state_error);
    client.close();
    if (!state_ok) {
        if (error != nullptr) {
            *error = state_error;
        }
        return false;
    }
    if (!logged_in) {
        if (error != nullptr) {
            *error = "该站点尚未登录（state=" + verdict + "）—— 请在参数面板点「打开登录窗口」"
                     "完成一次登录（或命令行 `--pydoll-login <id>`）；"
                     "**只读诊断仍可继续**（候选来自未登录页面）";
        }
        return false;
    }
    remember_session(site, names);   // `I15′`：只记名单 —— **无 Cookie 值**、不落盘
    return true;
}

// ---- `login_site`（v3 · step 11；**step 12（`M7B-44`）收口**）：面板「打开登录窗口」（**保持会话**）----
//  * **step 12 三处收口**（物证：2026-10-04 一次点击 → `browser_start` **4 次** = 4 个窗口，
//    因为旧实现在轮询期间**每一轮都让守护进程自愈重开**浏览器）：
//    ① 观测命令（`login_state`）在 Python 侧已改成**纯观测 · 不自愈** ⇒ 本函数据此
//       **一旦观测失败就立即结束**（不再无视错误继续空转）；
//    ② 判据为空（条目未配 `web.cookie_names`）⇒ **不再空转** `timeout_seconds`，立刻如实回报；
//    ③ 每轮检查取消位（`request_cancel_session_ops()`）⇒ 退出 / 取消不再干等满上限
bool login_site(const SiteRef& site, int timeout_seconds, std::string* error)
{
    // ⓪ 复位取消位：一次取消只作用于**在跑的那一次**（`wait_web_tasks()` 在退出时置位）
    g_cancel_polls = false;

    // ① 复用 / 起**常驻**会话（有头：`spawn_daemon` 不传 headless —— 用户要能看见窗口登录）
    const std::string pipe_name = ensure_daemon_session(error);
    if (pipe_name.empty()) {
        return false;   // *error 已由 helper 填好（可操作原因，`I21`）
    }

    PipeClient  client;
    std::string connect_error;
    if (!client.connect(pipe_name, 30'000, &connect_error)) {
        if (error != nullptr) {
            *error = "会话管道连不上（" + pipe_name + "）：" + connect_error;
        }
        return false;
    }

    // ② `open_tab`：导航到站点登录页（`I14`：无站点回落 —— url 只来自条目）
    if (!site.url.empty()) {
        nlohmann::json opened;
        std::string    open_error;
        if (!client.call("open_tab", {{"provider", site.provider_id}, {"url", site.url}}, &opened,
                         kTimeoutOpenMs, &open_error)) {
            client.close();
            if (error != nullptr) {
                *error = "打开站点页失败（" + site.url + "）：" + open_error;
            }
            return false;
        }
    }

    // ③ 判据体检（**step 12**）：条目没配 `web.cookie_names` ⇒ `login_state` 恒 `unknown`
    //    （`I14` 的正确行为）⇒ 空转只会白等 + 白弹窗。**窗口照旧保持打开**（用户仍可登录、
    //    会话仍可被网页节点复用），但这里**立刻如实回报**「无法自动判定」（`I21`：不假装能判）
    if (site.cookie_names.empty()) {
        client.close();
        if (error != nullptr) {
            *error = "该条目未配置 `web.cookie_names` ⇒ **无法自动判定登录完成**（判据 `I15′` 无输入）"
                     "—— 浏览器窗口**已打开**，可自行登录；此会话（profile）会被网页节点复用。"
                     "自动判定属批 4 回填项（`M7B-26`~`M7B-29`）：条目补上 `web.cookie_names` 即恢复"
                     "（可用 `--web-adapter-selftest --provider <id>` 复核）";
        }
        log::warn("[新通道] 登录窗口已打开但**无法自动判定**：条目（" + site.provider_id +
                  "）未配 `web.cookie_names` —— 不轮询（避免空转 + 反复弹窗）");
        return false;
    }

    // ④ 轮询 `login_state`：用户在**可见窗口**里手动登录（不代填密码、不绕过验证 · §13）
    const int  total_s  = timeout_seconds > 0 ? timeout_seconds : 300;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(total_s);
    while (std::chrono::steady_clock::now() < deadline) {
        // 取消位（退出流程 / 用户取消）—— **不**干等满上限
        if (g_cancel_polls.load()) {
            client.close();
            if (error != nullptr) {
                *error = "本次登录轮询已取消（进程退出 / 用户取消）—— 浏览器窗口**保持打开**，"
                         "可随时合上；该站点若已登录，网页版生成会复用同一会话";
            }
            log::info("[新通道] 登录轮询已取消（站点 " + login_request_site(site) + "）");
            return false;
        }
        bool                     logged_in = false;
        std::vector<std::string> names;
        std::string              verdict;
        std::string              state_error;
        const bool               observed = query_login_state(client, site, kTimeoutStateMs, &logged_in,
                                                            &names, &verdict, &state_error);
        if (observed && logged_in) {
            remember_session(site, names);
            client.close();
            log::info("[新通道] 登录成功（站点 " + login_request_site(site) + " · 名单 " +
                      std::to_string(names.size()) + " 条 · **仅内存**）—— 会话保持，供网页节点复用");
            return true;
        }
        // ⚠️ **step 12**：观测失败 = 浏览器窗口已被关掉（`login_state` **不再**自愈重开）
        //    ⇒ **立即结束**并如实回报；旧实现无视错误继续空转 ⇒ 每轮都弹一个新窗口
        if (!observed) {
            client.close();
            if (error != nullptr) {
                *error = "登录观测失败：" + state_error +
                         "—— 浏览器窗口可能已被关闭 ⇒ **本次登录到此结束**（本通道**不会**自动"
                         "重开窗口，避免反复弹窗）；如需继续请重新点「打开登录窗口」";
            }
            log::info("[新通道] 登录观测失败 → 结束本次登录（站点 " + login_request_site(site) +
                      "）：" + state_error);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
    client.close();
    // ⚠️ 超时**不关会话**：用户可在窗口里继续登录（下次运行直接复用）；
    //    进程退出时由 `main.cpp` 的 `shutdown_session()` 统一收尾（`I23` 同族纪律）
    if (error != nullptr) {
        *error = "在 " + std::to_string(total_s) + " 秒内未检测到登录完成（判据 `I15′` = 条目 "
                 "`web.cookie_names` 命中）—— 请在已打开的浏览器窗口里完成登录；"
                 "若确认已登录仍报此错，请核对条目的 `web.cookie_names`"
                 "（可用 `--web-adapter-selftest --provider <id>` 复核）";
    }
    return false;
}

// ---- `logout_site`（v3 · step 11）：按站点注销（`Storage.clearDataForOrigin`）----
bool logout_site(const SiteRef& site, int timeout_ms, std::string* error)
{
    const std::string origin = login_request_site(site);   // = `site_key_of(site.url)`
    if (origin.empty()) {
        if (error != nullptr) {
            *error = "无法从站点 url 派生 origin（`site.url` 为空？）—— 按站点注销需要具体站点";
        }
        return false;
    }
    const int wait_ms = timeout_ms > 0 ? timeout_ms : kTimeoutCloseMs;

    PipeClient  client;
    std::string connect_error;
    if (!client.connect(daemon_pipe_name(), wait_ms, &connect_error)) {
        if (error != nullptr) {
            *error = "新通道会话未在运行（管道连不上：" + connect_error +
                     "）—— 请先「打开登录窗口」建立会话再注销；**本次未做任何改动**";
        }
        return false;
    }
    nlohmann::json ack;
    std::string    call_error;
    const bool     command_ok =
        client.call("logout_site", {{"provider", site.provider_id}, {"origin", origin}}, &ack,
                    wait_ms, &call_error);
    client.close();
    if (!command_ok) {
        if (error != nullptr) {
            *error = "按站点注销失败（" + origin + "）：" + call_error;
        }
        return false;
    }
    // 内存会话同步清掉（与旧通道 `logout_site` 的既有语义一致：界面立刻显示「未登录」）
    SessionStore::instance().clear(origin);
    log::info("[新通道] 按站点注销完成（" + origin + "）—— 只清该站点，其他站点不受影响");
    return true;
}

// ---- `run_script`（v2 · 批 3 step 8）：页面内执行诊断 JS（**诊断换代的地基**）----
//  * 守护进程**未在运行** → false + 可操作原因（**不自动起浏览器**：起浏览器归执行器 / `--pydoll-login`）
//  * 服务端截断时（> 48 KiB）**仍返回 true**，但 `*error` 带说明 ⇒ 调用方**必须提示已截断**
bool run_script(const SiteRef& site, const std::string& script_js, int timeout_ms,
                std::string* json_result, std::string* error)
{
    const std::string pipe_name = daemon_pipe_name();
    PipeClient        client;
    if (!client.connect(pipe_name, 2'000, error)) {
        if (error != nullptr) {
            *error = "Python 守护进程未在运行（管道 " + pipe_name + " 连不上）—— "
                     "请先建立会话（`--pydoll-login <id>`），或由执行器按需启动"
                     "（`M7B-20` 接线后自动）";
        }
        return false;
    }
    nlohmann::json response;
    if (!client.call("run_script", {{"provider", site.provider_id}, {"script", script_js}},
                     &response, timeout_ms > 0 ? timeout_ms : kTimeoutScriptMs, error)) {
        client.close();
        return false;
    }
    client.close();

    // 结果原样交付（字符串直取；结构 / 标量 → JSON 文本）；截断事实**不隐藏**
    if (json_result != nullptr) {
        if (response.contains("result")) {
            const nlohmann::json& result = response["result"];
            *json_result = result.is_string() ? result.get<std::string>() : result.dump();
        } else {
            *json_result = "null";
        }
    }
    if (response.value("truncated", false) && error != nullptr) {
        *error = "脚本结果超过单帧余量（48 KiB）：已截断（`truncated=true`）—— "
                 "请收窄脚本返回的数据量（如分页 / 只回候选名）";
    }
    return true;
}

// ---- 内容返回（**v4 · step 14**）：`upload_image` / `send_prompt` / `read_answer` ----
namespace {

// 连守护进程（**不自动起浏览器**：起会话归调用方 / `--pydoll-login`）
bool connect_session(PipeClient* client, int connect_ms, std::string* error)
{
    const std::string pipe_name = daemon_pipe_name();
    if (client->connect(pipe_name, connect_ms, error)) {
        return true;
    }
    if (error != nullptr) {
        *error = "新通道会话未在运行（管道 " + pipe_name + " 连不上）—— "
                 "请先「打开登录窗口」建立会话（`--pydoll-login <id>`），再重试";
    }
    return false;
}

} // namespace

bool upload_image(const SiteRef& site, const std::vector<std::string>& images,
                  const std::string& attach, const std::string& attach_selector,
                  int timeout_ms, std::string* error)
{
    if (images.empty()) {
        if (error != nullptr) {
            *error = "upload_image 需要至少一个**本地绝对路径**（images[]）";
        }
        return false;
    }
    PipeClient client;
    if (!connect_session(&client, 2'000, error)) {
        return false;
    }
    nlohmann::json fields = {{"provider", site.provider_id}, {"images", images}};
    if (!attach.empty()) {
        fields["attach"] = attach;
    }
    if (!attach_selector.empty()) {
        fields["attach_selector"] = attach_selector;   // 文件输入框由调用方按条目下发（I14）
    }
    nlohmann::json ack;
    const bool     ok = client.call("upload_image", fields, &ack,
                                    timeout_ms > 0 ? timeout_ms : kTimeoutInjectMs, error);
    client.close();
    return ok;
}

bool send_prompt(const SiteRef& site, const std::string& prompt,
                 const std::vector<std::string>& input_selector,
                 const std::string& send_kind, const std::string& send_value,
                 bool upload_evidence, int timeout_ms, std::string* error)
{
    if (input_selector.empty()) {
        if (error != nullptr) {
            *error = "send_prompt 需要 `input_selector[]`（多候选，首个「命中且可见」者胜）—— "
                     "选择器由条目 `web.input_selector` 派生（`I14`：本层不认识站点）";
        }
        return false;
    }
    PipeClient client;
    if (!connect_session(&client, 2'000, error)) {
        return false;
    }
    nlohmann::json send = {{"kind", send_kind.empty() ? std::string("key") : send_kind},
                           {"value", send_value}};
    nlohmann::json fields = {{"provider", site.provider_id},
                             {"prompt", prompt},
                             {"input_selector", input_selector},
                             {"send", send}};
    if (upload_evidence) {
        fields["upload_evidence"] = true;   // I18 按位开关：仅**有图**的本次运行置位
    }
    nlohmann::json ack;
    const bool     ok = client.call("send_prompt", fields, &ack,
                                    timeout_ms > 0 ? timeout_ms : kTimeoutInjectMs, error);
    client.close();
    return ok;
}

bool read_answer(const SiteRef& site, const std::vector<std::string>& answer_selector,
                 const std::string& done_when_kind, int poll_ms, int max_polls,
                 int timeout_ms, std::string* text, bool* truncated, int* text_bytes,
                 std::string* error)
{
    if (text != nullptr) {
        text->clear();
    }
    if (truncated != nullptr) {
        *truncated = false;
    }
    if (text_bytes != nullptr) {
        *text_bytes = 0;
    }
    if (answer_selector.empty()) {
        if (error != nullptr) {
            *error = "read_answer 需要 `answer_selector[]`（条目 `web.answer_selector` 派生 · `I14`）";
        }
        return false;
    }
    PipeClient client;
    if (!connect_session(&client, 2'000, error)) {
        return false;
    }
    nlohmann::json fields = {{"provider", site.provider_id},
                             {"answer_selector", answer_selector}};
    if (!done_when_kind.empty()) {
        fields["done_when"] = {{"kind", done_when_kind}};
    }
    if (poll_ms > 0) {
        fields["poll_ms"] = poll_ms;
    }
    if (max_polls > 0) {
        fields["max_polls"] = max_polls;
    }
    nlohmann::json ack;
    const bool     ok = client.call("read_answer", fields, &ack,
                                    timeout_ms > 0 ? timeout_ms : kTimeoutAnswerMs, error);
    client.close();
    if (!ok) {
        return false;   // 未取到答案 / 超时：错误已由服务端给可操作 hint（不假装空答案）
    }
    if (text != nullptr) {
        *text = ack.value("text", std::string());
    }
    if (text_bytes != nullptr) {
        *text_bytes = ack.value("text_bytes", 0);
    }
    const bool cut = ack.value("truncated", false);
    if (truncated != nullptr) {
        *truncated = cut;
    }
    if (cut && error != nullptr) {
        *error = "回答正文超过单帧余量（48 KiB）：已截断（`truncated=true`）—— "
                 "界面 / 日志**必须**标注「已截断」，不得假装完整";
    }
    return true;
}

std::string current_tab_site()
{
    PipeClient  client;
    std::string connect_error;
    if (!client.connect(daemon_pipe_name(), 2'000, &connect_error)) {
        return {};   // 无会话 → 无 tab（如实回空串；**不假装**「不在任何站点」）
    }
    nlohmann::json ack;
    std::string    call_error;
    const bool     ok =
        client.call("current_tab", nlohmann::json::object(), &ack, 8'000, &call_error);
    client.close();
    if (!ok) {
        return {};   // 命令失败同样是「未知」—— 不编造站点
    }
    return ack.value("site", std::string());
}

bool tab_on_site(const std::string& site)
{
    const std::string want = site_key_of(site);   // 归一：既接受 origin，也接受完整 URL
    if (want.empty()) {
        return false;
    }
    const std::string current = current_tab_site();
    return !current.empty() && site_key_of(current) == want;
}

namespace {

std::string join_names(const std::vector<std::string>& names)
{
    std::string text;
    for (std::size_t index = 0; index < names.size(); ++index) {
        text += (index == 0 ? "" : " / ") + names[index];
    }
    return text.empty() ? "（条目未声明）" : text;
}

} // namespace

// `--pydoll-login <id>`：起**有头**浏览器 → 打开条目站点 → 等**人工登录** → 写内存会话 → 关窗
//  * 合规（§13）：有头窗口 + 用户手动操作；**不代填密码、不绕过验证、不注入站点端点**
//  * 判据（`I15′`）：条目 `cookie_names` 命中 ⇒ `logged_in`（**去 `userToken`**）
//  * 退出码：0 = 登录成功；1 = 超时 / 失败；2 = 参数 / 依赖问题
// ---- `--pydoll-chat-selftest`（**step 14 · P4**）：内容返回端到端（v4 三命令）----
int chat_selftest(int timeout_seconds)
{
    const int         timeout_ms = timeout_seconds > 0 ? timeout_seconds * 1000 : 120'000;
    const std::string prompt     = "v4-chat-selftest";

    // ① 夹具：输入框 + 发送按钮 + 回答容器（点发送 → 容器写 `E:` + 输入值）
    //    另记 `window.__inputs`（`input` 事件计数）⇒ 可证明是**逐字符真打字**
    std::error_code             ec;
    const std::filesystem::path page =
        std::filesystem::temp_directory_path(ec) / "aiwrite-chat-selftest.html";
    {
        std::ofstream out(page, std::ios::binary | std::ios::trunc);
        out << "<!doctype html><html><head><meta charset=\"utf-8\">"
               "<title>aiwrite-chat-selftest</title></head><body>"
               "<input id=\"box\" type=\"text\">"
               "<button id=\"send\">send</button>"
               "<div id=\"ans\"></div>"
               "<script>window.__inputs = 0;"
               "document.getElementById('box').addEventListener('input', function () {"
               "  window.__inputs = (window.__inputs || 0) + 1; });"
               "document.getElementById('send').addEventListener('click', function () {"
               "  document.getElementById('ans').innerText ="
               "    'E:' + document.getElementById('box').value; });"
               "</script></body></html>";
    }
    std::string page_path = page.string();
    std::replace(page_path.begin(), page_path.end(), '\\', '/');
    const std::string url = "file:///" + page_path;

    // ② 起守护进程（**独立管道名** · `--once` 单连接）
    const std::string pipe_name = channel_pipe_name("chat-selftest");
    std::string       error;
    DaemonProcess     daemon;
    if (!spawn_daemon(pipe_name, /*once=*/true, 30, &daemon, &error)) {
        std::printf("[新通道] 依赖问题：%s\n", error.c_str());
        return 2;
    }
    std::printf("[新通道] 内容返回自检：守护进程已起（pid=%u · 解释器=%s）\n",
                static_cast<unsigned>(daemon.pid), daemon.python.c_str());
    std::printf("[新通道] 夹具：%s\n", url.c_str());

    int    passed = 0;
    int    failed = 0;
    const auto check = [&passed, &failed](bool ok, const char* name, const std::string& detail) {
        if (ok) {
            ++passed;
            std::printf("   PASS  %s\n", name);
        } else {
            ++failed;
            std::printf("   FAIL  %s :: %s\n", name, detail.c_str());
        }
    };

    int        exit_code = 1;
    bool       closed    = false;
    PipeClient client;
    if (!client.connect(pipe_name, timeout_ms, &error)) {
        std::printf("[新通道] 失败：连接管道超时 —— %s\n", error.c_str());
    } else {
        nlohmann::json hello;
        if (!client.call("hello", nlohmann::json::object(), &hello, kTimeoutStateMs, &error)) {
            std::printf("[新通道] 失败：hello 未回包 —— %s\n", error.c_str());
        } else if (hello.value("proto", 0) != kProtoVersion) {
            std::printf("[新通道] 失败：协议版本不匹配（期望 %d，收到 %d）\n", kProtoVersion,
                        hello.value("proto", 0));
        } else {
            std::printf("[新通道] ready：proto=%d python=%s\n", hello.value("proto", 0),
                        hello.value("python", "?").c_str());
            nlohmann::json opened;
            const bool     open_ok = client.call(
                "open_tab", {{"provider", "chat-selftest"}, {"url", url}}, &opened,
                kTimeoutOpenMs, &error);
            check(open_ok, "M7B-54① `open_tab` 本地夹具（file:/// · 零外网 · 零登录）", error);

            // ③ `send_prompt`（真打字 + 点击发送）→ `stage{send, ok}`（**带 id = 完成回包**）
            nlohmann::json sent;
            const bool     sent_ok = client.call(
                "send_prompt",
                {{"provider", "chat-selftest"},
                 {"prompt", prompt},
                 {"input_selector", nlohmann::json::array({"#box"})},
                 {"send", {{"kind", "click"}, {"value", "#send"}}}},
                &sent, timeout_ms, &error);
            check(sent_ok && sent.value("stage", std::string()) == "send" &&
                      sent.value("ok", false),
                  "M7B-54② `send_prompt` → stage{send, ok}（真打字注入 + 点击发送）",
                  error + " | " + sent.dump().substr(0, 160));

            // ④ 逐字符自证（`input` 事件 > 0 ⇒ 真打字，不是 JS 一次性灌值）
            nlohmann::json probe;
            const bool     probe_ok = client.call(
                "run_script",
                {{"provider", "chat-selftest"},
                 {"script", "return { inputs: window.__inputs || 0,"
                            " value: document.getElementById('box').value };"}},
                &probe, timeout_ms, &error);
            const nlohmann::json probe_value =
                probe_ok ? probe.value("result", nlohmann::json::object()) : nlohmann::json::object();
            check(probe_ok && probe_value.value("inputs", 0) > 0 &&
                      probe_value.value("value", std::string()) == prompt,
                  "M7B-54③ 逐字符自证：`input` 事件 > 0 且输入框值 == 提示词",
                  error + " | " + probe.dump().substr(0, 160));
            // ⑤ `read_answer` → `answer_done{text, text_bytes, truncated=false}`（**字节一致**）
            nlohmann::json answer;
            const bool     answer_ok = client.call(
                "read_answer",
                {{"provider", "chat-selftest"},
                 {"answer_selector", nlohmann::json::array({"#ans"})},
                 {"poll_ms", 200},
                 {"max_polls", 20}},
                &answer, timeout_ms, &error);
            const std::string expected = "E:" + prompt;
            const std::string got      = answer.value("text", std::string());
            check(answer_ok && got == expected &&
                      answer.value("text_bytes", 0) == static_cast<int>(expected.size()) &&
                      answer.value("truncated", true) == false,
                  "M7B-54④ `read_answer` → `answer_done`（正文与夹具渲染**字节一致**；`truncated=false`）",
                  error + " | got=" + got + " | " + answer.dump().substr(0, 160));

            // ⑥ `send_prompt{upload_evidence=true}` 且**无上传证据** → `no_upload_evidence`（`I18` 协议级拦截）
            nlohmann::json blocked;
            std::string    blocked_error;
            const bool     blocked_ok = client.call(
                "send_prompt",
                {{"provider", "chat-selftest"},
                 {"prompt", prompt},
                 {"input_selector", nlohmann::json::array({"#box"})},
                 {"send", {{"kind", "click"}, {"value", "#send"}}},
                 {"upload_evidence", true}},
                &blocked, kTimeoutStateMs, &blocked_error);
            check(!blocked_ok && blocked_error.find("no_upload_evidence") != std::string::npos,
                  "M7B-54⑤ `upload_evidence=true` 且无证据 → 拒绝（`I18` 协议级拦截）", blocked_error);

            // ⑦ `upload_image` 缺 `attach_selector` → 可操作拒绝（不静默 · `I21`）
            nlohmann::json upload;
            std::string    upload_error;
            const bool     upload_ok = client.call(
                "upload_image",
                {{"provider", "chat-selftest"}, {"images", nlohmann::json::array({page_path})}},
                &upload, kTimeoutStateMs, &upload_error);
            check(!upload_ok && upload_error.find("attach_unsupported") != std::string::npos,
                  "M7B-54⑥ `upload_image` 缺 attach_selector → 可操作拒绝", upload_error);

            // ⑧ 收尾：`shutdown`（**同一连接** · `--once`）→ **不立刻 close**（否则守护进程读循环撞
            //    `pipe_error` ⇒ 退出码 1；同 `script_selftest` 的实测教训）
            std::string ignored;
            client.send_command("shutdown", nlohmann::json::object(), &ignored);
            closed    = true;
            exit_code = (failed == 0) ? 0 : 1;
        }
        if (!closed) {
            client.close();
        }
    }
    if (!closed) {
        std::printf("[新通道] 未正常收尾：等待守护进程按空闲超时退出…\n");
    }
    const int daemon_code = wait_daemon(daemon, 90'000);
    std::printf("[新通道] 守护进程退出码=%d（0 = 干净收尾 · 未强杀）\n", daemon_code);
    if (exit_code == 0 && daemon_code != 0) {
        exit_code = 1;
    }
    release_daemon(daemon);
    std::filesystem::remove(page, ec);
    std::printf("[新通道] 内容返回自检结果：%d 通过 / %d 失败（退出码 %d）\n", passed, failed,
                exit_code);
    std::printf("[新通道] 守护进程日志：%s\n",
                (paths::logs_dir() / "pydoll_channel_daemon.log").string().c_str());
    return exit_code;
}

int pydoll_login(const std::string& provider_id, int timeout_seconds)
{
    // ① 解析条目（**严格**：缺 `web.login_url` 等 → 可操作错误；不发请求、不开窗）
    const ai::ProviderSpecs& table = ai::provider_specs();
    const ai::ProviderSpec*  spec  = table.find(provider_id);
    if (spec == nullptr) {
        std::printf("[pydoll-login] 未知条目：%s（用 `--provider-dump` 查看可用 id）\n",
                    provider_id.c_str());
        return 2;
    }
    const ai::ProviderWebSpec web        = ai::strict_web_spec_for(spec);
    const std::string         site_error = ai::web_site_error(spec);
    if (!site_error.empty()) {
        std::printf("[pydoll-login] 条目不可用：%s\n", site_error.c_str());
        return 2;
    }
    const std::string site_id = ai::strict_web_provider_id_for(spec);
    const SiteRef site = login_request_of(web, site_id, /*for_probe=*/false, /*offscreen=*/false);

    // ② 起守护进程（**非 once**：保持存活，直到我们主动 `shutdown`）
    const std::string pipe_name = channel_pipe_name("pydoll-login");
    std::string       error;
    DaemonProcess     daemon;
    if (!spawn_daemon(pipe_name, /*once=*/false, 30, &daemon, &error)) {
        std::printf("[pydoll-login] 依赖问题：%s\n", error.c_str());
        log::error("[pydoll-login] " + error);
        return 2;
    }
    std::printf("[pydoll-login] 守护进程已起（pid=%u）· 站点 %s\n",
                static_cast<unsigned>(daemon.pid), site.url.c_str());

    int        result = 1;
    PipeClient client;
    if (!client.connect(pipe_name, 30'000, &error)) {
        std::printf("[pydoll-login] 守护进程未就绪：%s\n", error.c_str());
        release_daemon(daemon);
        return 1;
    }

    nlohmann::json stage;
    const bool     opened = client.call("open_tab", {{"provider", site_id}, {"url", site.url}}, &stage,
                                        kTimeoutOpenMs, &error);
    if (!opened || stage.value("ok", false) != true) {
        std::printf("[pydoll-login] 打开站点失败：%s\n",
                    error.empty() ? "stage.ok=false（守护进程已给出原因，详见日志）" : error.c_str());
    } else {
        std::printf("[pydoll-login] 已打开 %s —— 请在窗口内**手动登录**"
                    "（不代填密码、不绕过验证 · §13）\n"
                    "               判据：条目 cookie_names 命中（%s）· 超时 %d s\n",
                    site.url.c_str(), join_names(site.cookie_names).c_str(), timeout_seconds);
        g_cancel_polls = false;   // 复位取消位（一次取消只作用于在跑的那一次 · `M7B-44`）
        const auto started = std::chrono::steady_clock::now();
        bool       aborted = false;   // 非超时结束（观测失败 / 取消）—— 超时文案**不得**误报
        while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                started)
                   .count() < timeout_seconds) {
            nlohmann::json state;
            // ⚠️ **M7B step 12（`M7B-44`）**：观测命令（`login_state`）**不再自愈**
            //    （窗口被关 → 回 `daemon_down`）⇒ 失败即**结束**（CLI 与面板口径一致）
            const bool observed = client.call("login_state",
                                              {{"provider", site_id},
                                               {"cookie_names", site.cookie_names}},
                                              &state, kTimeoutStateMs, &error);
            if (observed && state.value("state", "unknown") == "logged_in") {
                const std::vector<std::string> names =
                    state.value("cookie_names", std::vector<std::string>{});
                Session session;
                session.logged_in   = true;
                session.site        = site_key_of(site.url);
                session.provider_id = site_id;
                session.url         = site.url;
                for (const std::string& name : names) {
                    Cookie cookie;
                    cookie.name = name;
                    session.cookies.push_back(cookie);
                }
                SessionStore::instance().set(session);
                std::printf("[pydoll-login] 登录成功（该站点 Cookie 名单 %d 条 · **仅内存**；"
                            "加密快照由守护进程在关闭前写入 L2）\n",
                            static_cast<int>(session.cookies.size()));
                result = 0;
                break;
            }
            if (!observed) {
                std::printf("[pydoll-login] 登录观测失败：%s\n"
                            "               浏览器窗口可能已被关闭 —— 本次登录结束（可重试）\n",
                            error.c_str());
                aborted = true;
                break;
            }
            if (g_cancel_polls.load()) {
                std::printf("[pydoll-login] 已请求取消（进程退出 / 用户取消）—— 本次登录结束\n");
                aborted = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        if (result != 0 && !aborted) {
            std::printf("[pydoll-login] 超时：%d s 内未判定为已登录（可重试；"
                        "若站点改版请核对条目 `web.cookie_names`）\n",
                        timeout_seconds);
        }
    }

    // ③ 收尾：`shutdown`（守护进程 `Browser.close` → 等进程退出 + **关闭前写 L2 快照**，`I23①`）
    std::string ignored;
    client.send_command("shutdown", {{"grace_ms", 5'000}}, &ignored);
    client.close();
    const int code = wait_daemon(daemon, 30'000);
    std::printf("[pydoll-login] 守护进程退出码=%d（-1 = 未在期限内退出；**未强杀** · `I23①`）\n",
                code);
    release_daemon(daemon);
    return result;
}

} // namespace aiwrite::web::channel
