// ============================================================================//  pydoll_channel_selftest.cpp — 自检（selftest / scriptSelftest / chatSelftest）与 --pydoll-login//  * ARCH-Z1b：自 1282 行单文件 `pydoll_channel.cpp` 逐字拆出，**零语义变化**//  * 模块内契约见 `pydoll_channel_detail.h`；公开 API 见 `pydoll_channel.h`// ============================================================================
#include "pydoll_channel_detail.h"

namespace aiwrite::web::channel {


// ============================================================================
//  对外接口（批 2：只落地 selftest / ensure_session / pydoll_login）
// ============================================================================

int selftest(int timeout_ms)
{
    const std::string pipeName = channel_pipe_name("pydoll-selftest");
    std::string       error;
    DaemonProcess     daemon;

    log::info("[新通道] 自检：起守护进程（pipe=" + pipeName + "）");
    if (!spawn_daemon(pipeName, /*once=*/true, 20, &daemon, &error)) {
        log::error("[新通道] 自检失败（依赖问题）：" + error);
        std::printf("[新通道] 依赖问题：%s\n", error.c_str());
        return 2;
    }
    std::printf("[新通道] 守护进程已起（pid=%u · 解释器=%s）· 等待管道…\n",
                static_cast<unsigned>(daemon.pid), daemon.python.c_str());

    int        exit_code = 1;
    PipeClient client;
    if (!client.connect(pipeName, timeout_ms, &error)) {
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
                client.sendCommand("shutdown", nlohmann::json::object(), &ignored);
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
                static_cast<unsigned long long>(ipcConnectCount()),
                static_cast<unsigned long long>(ipcCommandCount()));

    std::printf("[新通道] 守护进程日志：%s\n",
                (paths::logsDir() / "pydoll_channel_daemon.log").string().c_str());
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
int scriptSelftest(int timeout_seconds)
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
    const std::string pipeName = channel_pipe_name("script-selftest");
    std::string       error;
    DaemonProcess     daemon;
    if (!spawn_daemon(pipeName, /*once=*/true, 30, &daemon, &error)) {
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
    if (!client.connect(pipeName, timeout_ms, &error)) {
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
            client.sendCommand("shutdown", nlohmann::json::object(), &ignored);
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
                (paths::logsDir() / "pydoll_channel_daemon.log").string().c_str());
    return exit_code;
}

// ---- `--pydoll-chat-selftest`（**step 14 · P4**）：内容返回端到端（v4 三命令）----
int chatSelftest(int timeout_seconds)
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
    const std::string pipeName = channel_pipe_name("chat-selftest");
    std::string       error;
    DaemonProcess     daemon;
    if (!spawn_daemon(pipeName, /*once=*/true, 30, &daemon, &error)) {
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
    if (!client.connect(pipeName, timeout_ms, &error)) {
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
            client.sendCommand("shutdown", nlohmann::json::object(), &ignored);
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
                (paths::logsDir() / "pydoll_channel_daemon.log").string().c_str());
    return exit_code;
}

int pydollLogin(const std::string& provider_id, int timeout_seconds)
{
    // ① 解析条目（**严格**：缺 `web.login_url` 等 → 可操作错误；不发请求、不开窗）
    const ai::ProviderSpecs& table = ai::providerSpecs();
    const ai::ProviderSpec*  spec  = table.find(provider_id);
    if (spec == nullptr) {
        std::printf("[pydoll-login] 未知条目：%s（用 `--provider-dump` 查看可用 id）\n",
                    provider_id.c_str());
        return 2;
    }
    const ai::ProviderWebSpec web        = ai::strictWebSpecFor(spec);
    const std::string         site_error = ai::webSiteError(spec);
    if (!site_error.empty()) {
        std::printf("[pydoll-login] 条目不可用：%s\n", site_error.c_str());
        return 2;
    }
    const std::string site_id = ai::strictWebProviderIdFor(spec);
    const SiteRef site = login_request_of(web, site_id, /*for_probe=*/false, /*offscreen=*/false);

    // ② 起守护进程（**非 once**：保持存活，直到我们主动 `shutdown`）
    const std::string pipeName = channel_pipe_name("pydoll-login");
    std::string       error;
    DaemonProcess     daemon;
    if (!spawn_daemon(pipeName, /*once=*/false, 30, &daemon, &error)) {
        std::printf("[pydoll-login] 依赖问题：%s\n", error.c_str());
        log::error("[pydoll-login] " + error);
        return 2;
    }
    std::printf("[pydoll-login] 守护进程已起（pid=%u）· 站点 %s\n",
                static_cast<unsigned>(daemon.pid), site.url.c_str());

    int        result = 1;
    PipeClient client;
    if (!client.connect(pipeName, 30'000, &error)) {
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
                    site.url.c_str(), joinNames(site.cookie_names).c_str(), timeout_seconds);
        sessionState().cancel_polls = false;   // 复位取消位（一次取消只作用于在跑的那一次 · `M7B-44`）
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
                session.loggedIn   = true;
                session.site        = siteKeyOf(site.url);
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
            if (sessionState().cancel_polls.load()) {
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
    client.sendCommand("shutdown", {{"grace_ms", 5'000}}, &ignored);
    client.close();
    const int code = wait_daemon(daemon, 30'000);
    std::printf("[pydoll-login] 守护进程退出码=%d（-1 = 未在期限内退出；**未强杀** · `I23①`）\n",
                code);
    release_daemon(daemon);
    return result;
}


} // namespace aiwrite::web::channel

