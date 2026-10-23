// ============================================================================//  pydoll_channel.cpp — 基础设施（包目录 / 解释器 / 管道名 / 守护进程）+ 会话生命周期 + 按站点动作//  * ARCH-Z1b：自 1282 行单文件 `pydoll_channel.cpp` 逐字拆出，**零语义变化**//  * 模块内契约见 `pydoll_channel_detail.h`；公开 API 见 `pydoll_channel.h`// ============================================================================
#include "pydoll_channel_detail.h"

namespace aiwrite::web::channel {


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
    candidates.emplace_back(paths::exeDir() / "python");
    // 开发态：从 exe 目录**向上**找 `source/python`（exe 通常在 <仓库根>/build/bin）≤ 6 层
    std::filesystem::path dir = paths::exeDir();
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
        paths::exeDir() / "python" / ".venv" / "Scripts" / "python.exe",
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


// 起守护进程：`python -m brain_ai_browser --serve [--once] --pipe-name <名> --idle-timeout <n>`
//  * stdout/stderr → `<日志目录>/pydoll_channel_daemon.log`（诊断可查）
//  * stdin → NUL（避免部分 Python 构建对空句柄敏感）
bool spawn_daemon(const std::string& pipeName, bool once, int idle_timeout_s,
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

    const std::filesystem::path log_path = paths::logsDir() / "pydoll_channel_daemon.log";
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
    command += L" --pipe-name " + widen(pipeName) + L" --idle-timeout " +
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

// ---- 会话生命周期（批 3 step 9 · `M7B-18`）：诊断工具**自己起会话 / 自己收尾** ----

SessionState& sessionState()
{
    static SessionState state;
    return state;
}

// 确保**常驻会话**在跑（复用已有 or 起守护进程）；返回会话管道名（失败 → 空串 + *error）
//  * 「已有会话」= 本进程已记账（`session_ready()`），**或**管道能连上（本进程外起过）
//  * 起守护进程一律 `once=false`：会话要跨多条命令（open_tab → run_script → … → shutdown）
//  * 本函数是**等价抽取**（自 step 9 起就是这段逻辑）；step 11 只为 `login_site` 复用而抽出
std::string ensure_daemon_session(std::string* error)
{
    const std::string pipeName = daemonPipeName();
    if (!sessionReady()) {
        bool alive = false;
        {
            PipeClient  probe;
            std::string probe_error;
            if (probe.connect(pipeName, 300, &probe_error)) {
                alive = true;
                probe.close();
            }
        }
        if (!alive) {
            DaemonProcess daemon;
            std::string   spawn_error;
            if (!spawn_daemon(pipeName, /*once=*/false, 600, &daemon, &spawn_error)) {
                if (error != nullptr) {
                    *error = spawn_error;
                }
                return {};
            }
            sessionState().process      = daemon;
            sessionState().pipe_name = pipeName;
            log::info("[新通道] 会话已起（pid=" + std::to_string(daemon.pid) + " · 管道 " +
                      pipeName + "）");
        }
    }
    return pipeName;
}

// 把「已登录」证据写进内存 `SessionStore`（`I15′`：只记**名单** —— 无 Cookie 值、不落盘；
// 加密快照归 L2 `session.py`）
void remember_session(const SiteRef& site, const std::vector<std::string>& names)
{
    Session session;
    session.loggedIn   = true;
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
bool query_login_state(PipeClient& client, const SiteRef& site, int timeout_ms, bool* loggedIn,
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
    if (loggedIn != nullptr) {
        *loggedIn = state.value("state", std::string("unknown")) == "logged_in";
    }
    if (names != nullptr) {
        *names = state.value("cookie_names", std::vector<std::string>{});
    }
    return true;
}

// **M7B step 12（`M7B-44`）**：请求取消**正在进行的登录态轮询**（`login_site` / `pydoll_login`）
//  * 面板关窗 / 进程退出时由 `ui::wait_web_tasks()` 调用 —— 否则退出要把登录上限（120 s）干等完
//  * **幂等 · 线程安全**；只改**轮询循环的继续条件**，不动会话、不发任何命令
//  * 下一次 `login_site` / `pydoll_login` 进入时会**自动复位**（一次取消只作用于在跑的那一次）
void requestCancelSessionOps()
{
    sessionState().cancel_polls = true;
    log::info("[新通道] 已请求取消登录态轮询（等待中的 `login_site` / `pydoll_login` 将尽早结束）");
}

bool sessionReady()
{
    return sessionState().process.process != nullptr;
}

bool shutdownSession()
{
    if (sessionState().process.process == nullptr) {
        return false;   // 幂等：本来就没有会话
    }
    std::string ignored;
    PipeClient  closer;     // ⚠️ 作用域必须**盖过** `wait_daemon`：句柄先断会变 `pipe_error`
    if (closer.connect(sessionState().pipe_name, 2'000, &ignored)) {
        closer.sendCommand("shutdown", nlohmann::json::object(), &ignored);
        // 发完**不主动 close**（照 `--pydoll-selftest` 做法）
    }
    const int code = wait_daemon(sessionState().process, 60'000);
    if (code != 0) {
        log::warn("[新通道] 会话收尾非干净退出（退出码 " + std::to_string(code) +
                  "）—— 详见 pydoll_channel_daemon.log");
    }
    release_daemon(sessionState().process);
    sessionState().pipe_name.clear();
    return true;
}

// ---- 确保会话（`M7B-18` 换代：**只读判定** → 「确保有活会话且已导航到站点」）----
bool ensureSession(const SiteRef& site, int timeout_ms, std::string* error)
{
    const int boot_ms = timeout_ms > 0 ? timeout_ms : kTimeoutOpenMs;

    // ① 复用已有会话？（管道能连上 = 有活守护进程；连不上 → 自己起一个）
    const std::string pipeName = ensure_daemon_session(error);
    if (pipeName.empty()) {
        return false;   // *error 已由 helper 填好（可操作原因，`I21`）
    }

    PipeClient  client;
    std::string connect_error;
    if (!client.connect(pipeName, boot_ms, &connect_error)) {
        if (error != nullptr) {
            *error = "会话管道连不上（" + pipeName + "）：" + connect_error;
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
    bool                     loggedIn = false;
    std::vector<std::string> names;
    std::string              verdict;
    std::string              state_error;
    const bool               state_ok =
        query_login_state(client, site, boot_ms, &loggedIn, &names, &verdict, &state_error);
    client.close();
    if (!state_ok) {
        if (error != nullptr) {
            *error = state_error;
        }
        return false;
    }
    if (!loggedIn) {
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
bool loginSite(const SiteRef& site, int timeout_seconds, std::string* error)
{
    // ⓪ 复位取消位：一次取消只作用于**在跑的那一次**（`wait_web_tasks()` 在退出时置位）
    sessionState().cancel_polls = false;

    // ① 复用 / 起**常驻**会话（有头：`spawn_daemon` 不传 headless —— 用户要能看见窗口登录）
    const std::string pipeName = ensure_daemon_session(error);
    if (pipeName.empty()) {
        return false;   // *error 已由 helper 填好（可操作原因，`I21`）
    }

    PipeClient  client;
    std::string connect_error;
    if (!client.connect(pipeName, 30'000, &connect_error)) {
        if (error != nullptr) {
            *error = "会话管道连不上（" + pipeName + "）：" + connect_error;
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
        if (sessionState().cancel_polls.load()) {
            client.close();
            if (error != nullptr) {
                *error = "本次登录轮询已取消（进程退出 / 用户取消）—— 浏览器窗口**保持打开**，"
                         "可随时合上；该站点若已登录，网页版生成会复用同一会话";
            }
            log::info("[新通道] 登录轮询已取消（站点 " + login_request_site(site) + "）");
            return false;
        }
        bool                     loggedIn = false;
        std::vector<std::string> names;
        std::string              verdict;
        std::string              state_error;
        const bool               observed = query_login_state(client, site, kTimeoutStateMs, &loggedIn,
                                                            &names, &verdict, &state_error);
        if (observed && loggedIn) {
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
bool logoutSite(const SiteRef& site, int timeout_ms, std::string* error)
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
    if (!client.connect(daemonPipeName(), wait_ms, &connect_error)) {
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


} // namespace aiwrite::web::channel

