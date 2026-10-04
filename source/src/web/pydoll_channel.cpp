// 新通道：Pydoll 守护进程（命名管道）—— 实现（M7B 批 2 · `M7B-15` / `M7B-16`）
//
//  * 本批**不接线**：只服务 `--pydoll-selftest` / `--pydoll-login`（批 2 冒烟 · `M7B-19` 门槛）
//  * 守护进程启动逻辑与 `main.cpp::pipe_selftest()` **同构**（同一套候选与 `CreateProcess` 语义）；
//    统一收口（抽公共函数）归 `M7B-18` 诊断换代 —— 本批各自独立，**不动既有自检路径**
//  * 一切失败都返回**可操作原因**（`I21`）：无 Python / 无包目录 / 守护进程未起 / 命令超时

#include "web/pydoll_channel.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
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
    std::printf("[新通道] 守护进程日志：%s\n",
                (paths::logs_dir() / "pydoll_channel_daemon.log").string().c_str());
    return exit_code;
}

// ---- 按站点确保登录态（判据 = `I15′`：CDP Cookie 名单命中条目 `cookie_names`）----
//  * 本批**只读判定**（要求已有守护进程在跑）；自动 `open_tab` / 复用 `--serve` 常驻归批 3
bool ensure_session(const SiteRef& site, int timeout_ms, std::string* error)
{
    const std::string pipe_name = daemon_pipe_name();
    PipeClient        client;
    if (!client.connect(pipe_name, 2'000, error)) {
        if (error != nullptr) {
            *error = "Python 守护进程未在运行（管道 " + pipe_name + " 连不上）—— "
                     "新通道本批**未接线**（生产路径仍走 WebView2）；"
                     "自动 `open_tab` / 常驻复用归 M7B 批 3（`M7B-20`）";
        }
        return false;
    }
    nlohmann::json state;
    if (!client.call("login_state",
                     {{"provider", site.provider_id}, {"cookie_names", site.cookie_names}}, &state,
                     timeout_ms, error)) {
        client.close();
        return false;
    }
    const std::string              verdict = state.value("state", "unknown");
    const std::vector<std::string> names =
        state.value("cookie_names", std::vector<std::string>{});
    client.close();
    if (verdict != "logged_in") {
        if (error != nullptr) {
            *error = "该站点尚未登录（state=" + verdict + "）—— 请先完成一次登录（`--pydoll-login`）";
        }
        return false;
    }
    // 写内存会话（`I15′`：只记名单 —— **无 Cookie 值**、不落盘；加密快照归 L2 `session.py`）
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
    return true;
}

// ---- 以下四项**本批如实回「尚未实现（批 3）」**（不假装成功；`I21` 同族）----

bool logout_site(const SiteRef& site, int timeout_ms, std::string* error)
{
    (void)site;
    (void)timeout_ms;
    if (error != nullptr) {
        *error = "新通道的 `logout_site` 尚未实现：§6.1 词表暂无该命令 —— "
                 "按站点注销（`Storage.clearDataForOrigin`）归 M7B 批 3；当前请走 WebView2 路径";
    }
    return false;
}

bool run_script(const SiteRef& site, const std::string& script_js, int timeout_ms,
                std::string* json_result, std::string* error)
{
    (void)site;
    (void)script_js;
    (void)timeout_ms;
    (void)json_result;
    if (error != nullptr) {
        *error = "新通道的 `run_script` 尚未实现：DOM 适配器改走 `send_prompt` / `read_answer`"
                 "（§6.1 帧表）归 M7B 批 3（`M7B-20` / `M7B-21`）";
    }
    return false;
}

std::string current_tab_site()
{
    return {};  // 守护进程尚未暴露「当前 tab」—— 批 3
}

bool tab_on_site(const std::string& site)
{
    (void)site;
    return false;  // 同上（批 3）
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
        const auto started = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                started)
                   .count() < timeout_seconds) {
            nlohmann::json state;
            if (client.call("login_state",
                            {{"provider", site_id}, {"cookie_names", site.cookie_names}}, &state,
                            kTimeoutStateMs, &error) &&
                state.value("state", "unknown") == "logged_in") {
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
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        if (result != 0) {
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
