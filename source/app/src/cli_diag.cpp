// =============================================================================
//  AIwrite 主程序入口（M1：技术验证 + 项目骨架）
//  GUI 程序（/SUBSYSTEM:WINDOWS + mainCRTStartup），启动时初始化日志与配置目录
// =============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // M7B 批 1 step 2：--pipe-selftest（CreateProcessW / WaitForSingleObject）

#include "app.h"
#include "editor_controller.h"
#include "output_panel.h"
#include "deepseek_official_provider.h"
#include "provider_spec.h"   // M_patchB L1：Provider 配置表（--provider-selftest）
#include "provider_factory.h"    // L2（B2-b · PB2-09）：按表 protocol 分派
#include "inference_provider.h"  // L2（B2-b · PB2-08）：统一推理接口（ping 走同一入口）
#include "node_registry.h"
#include "workflow_io.h"   // VA-07：graph_to_json（运行前 JSON 基线 / TST-L01 判据）
#include "deepseek_web_client.h"
#include "dom_web_client.h"      // L3（PB2-13/15）：通用 DOM 站点适配器 + 选择器探测
#include "upload_supply_probe.h"   // M8 Phase 2：C++ 供料探针（--web-supply-probe）
#include "upload_registry.h"  // M8-14：站点上传模板（--upload-selftest）
#include "image_convert.h"    // M8-14：图片准备（离线断言）
#include "upload_evidence.h"  // M8-14：上传双证据（离线断言）
#include "config.h"
#include "text_export.h"
#include "credential.h"
#include "log.h"
#include "paths.h"
#include "webview_host.h"
#include "pipe_client.h"      // M7B 批 1 step 2：命名管道客户端（--pipe-selftest / M7B-02）
#include "pydoll_channel.h"   // M7B 批 2：新通道（--pydoll-login / --pydoll-selftest）
#include "session_store.h"   // M_patchB：--provider-selftest 的网页版登录态检查
#include "connection_test.h" // M_patchAB（PB2-07 / PB2-30②）：退出前收尾后台诊断线程
#include "prompt_generator.h" // M_8nodes_extension L1-d（§4.3）：退出前收尾「生成提示词」线程
#include "session_snapshot.h" // 批 3（FEA-L02）：--web-session-list 的 L2 快照只读元数据

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <atomic>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

#include "cli_internal.h"

// ============================================================================
//  CLI 诊断 / 杂项域（CLN-14 拆分）—— 定义见 ui/include/cli_internal.h
//  * 原 main.cpp 匿名块函数提升为全局命名空间（跨 TU 与 cli_selftest.cpp / main.cpp 共享）
// ============================================================================

std::string reportTimestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

bool readTextFile(const std::string& path, std::string* out, std::string* error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) {
            *error = "打不开文件（不存在 / 无权限）：" + path;
        }
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (out != nullptr) {
        *out = buffer.str();
    }
    return true;
}

bool writeTextFile(const std::string& path, const std::string& text, std::string* error)
{
    std::error_code ec;
    const std::filesystem::path fs_path(path);
    if (!fs_path.parent_path().empty()) {
        std::filesystem::create_directories(fs_path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error != nullptr) {
            *error = "写不了文件（目录不存在 / 无权限）：" + path;
        }
        return false;
    }
    out << text;
    if (!out) {
        if (error != nullptr) {
            *error = "写入过程中出错：" + path;
        }
        return false;
    }
    return true;
}

void printUsage()
{
    std::printf("AIwrite - AI 小说/剧本创作软件 (M1 技术验证)\n");
    std::printf("用法: aiwrite [选项]\n");
    std::printf("  --open <路径>    启动即打开指定工作流 JSON（默认用内置示例工作流）\n");
    std::printf("  --console        为日志额外分配控制台窗口（便于开发调试）\n");
    std::printf("  --login-selftest [--timeout <秒>]\n");
    std::printf("                   网页版登录自检：离屏跑「WebView2 环境 → 导航 → 提取 Cookie」\n");
    std::printf("                   退出码 0=通过 / 1=失败 / 2=超时（默认超时 30 秒）\n");
    std::printf("  --web-probe [--timeout <秒>]\n");
    std::printf("                   网页版协议探测：在已登录页面内读取 userToken 并请求 PoW 挑战\n");
    std::printf("                   退出码 0=取到 userToken / 1=失败（需先登录过一次）\n");
    std::printf("                   ＋ --provider <id>：按**配置表条目**的站点探测（PB2-23；\n");
    std::printf("                     严格解析、不回落；站点不可用 → 打印原因 + 退出码 2）\n");
    std::printf("  --web-adapter-selftest --provider <id>\n");
    std::printf("                   L3（PB2-15）选择器探测 / 诊断：在**已登录窗口**里只读检查\n");
    std::printf("                   input_selector / send / answer_selector / done_when / token_expr，\n");
    std::printf("                   并给可操作修复建议；退出码 0=全部命中 / 1=有缺项或页面异常\n");
    std::printf("                   / 2=站点不可用 / 3=需登录（人工介入；批 3 · FEA-L03）\n");
    std::printf("                   ＋ --provider all：**批量只读探测**全部 DOM 站点（批 3 · FEA-L01）\n");
    std::printf("                     退出码 0=全部可用 / 3=有站需登录 / 1=有真失败 / 2=无可测站点\n");
    std::printf("                   ＋ --json <路径>：把报告落成机器读版 JSON（供与上一轮对比）\n");
    std::printf("                   ＋ --json-diff <上一轮报告>：对比并打印状态 / 判据变化\n");
    std::printf("  --web-chat \"<提示词>\"\n");
    std::printf("                   网页版生成：取 PoW 挑战 → C++ 求解 → 调用 /api/v0/chat/completion\n");
    std::printf("                   退出码 0=取到文本 / 1=失败（需先登录过一次）\n");
    std::printf("  --web-session-selftest\n");
    std::printf("                   会话自动引导回归：先按用户手动路径开窗口（不探测）→ 再调 ensure_session\n");
    std::printf("                   应「窗口已开 → 自动补探测」拿到凭证；退出码 0=通过 / 1=失败\n");
    std::printf("  --web-session-list [--json <路径>]\n");
    std::printf("                   会话 / 登录态清单（批 3 · FEA-L02）：① 本进程内存会话（I15：不落盘，\n");
    std::printf("                   全新进程必然为空）② L2 DPAPI 快照**只读元数据**（路径 / 字节 / 时间）\n");
    std::printf("                   ③ 每条网页版条目的登录判据配置（cookie_names）\n");
    std::printf("                   退出码 0=清单成功（有无会话都算成功）/ 1=读快照元数据异常\n");
    std::printf("  --run-selftest [--web] [--official]\n");
    std::printf("                   执行自检：创建示例工作流 → 运行到结束，打印各节点状态与统计\n");
    std::printf("                   --web：LLMGenerate 走网页版真实生成（需已登录过一次）\n");
    std::printf("                   默认 LLMGenerate 为占位实现，预期该节点 error、下游 skipped\n");
    std::printf("  --vlm-selftest [--image <路径>] [--api-base <地址>] [--model <模型名>]\n");
    std::printf("                 [--key-ref <凭据引用名>]\n");
    std::printf("                   图片理解（M5-02）自检：编码图片 → 构造多模态请求体（离线断言）\n");
    std::printf("                   → 有 Key 时真实调用并打印生成文本\n");
    std::printf("                   退出码 0=通过 / 1=失败 / 2=缺少 API Key（离线部分已通过）\n");
    std::printf("  --provider-selftest [--provider <id>] [--api-base <地址>] [--model <模型名>]\n");
    std::printf("                      [--key-ref <凭据引用名>]\n");
    std::printf("                   Provider 配置表（M_patchB L1）自检：表校验 + 离线断言\n");
    std::printf("                   --provider <id> 时：API 条目 → 有 Key 就发一条 ping；\n");
    std::printf("                   网页版条目 → 只检查登录态与端点一致性（不发送内容）\n");
    std::printf("                   退出码 0=通过 / 1=失败 / 2=缺 Key 或未登录（离线部分已通过）\n");
    std::printf("  --provider-dump  打印生效的 Provider 配置表（含来源与覆盖链）\n");
    std::printf("  --pipe-selftest [--timeout <秒>]\n");
    std::printf("                   M7B-02 命名管道冒烟：起 Python 守护进程（brain_ai_browser --serve）\n");
    std::printf("                   → hello → 校验 ready{proto, python, browser} → shutdown\n");
    std::printf("                   退出码 0=通过 / 1=失败 / 2=环境缺失（找不到 Python 或包目录）\n");
    std::printf("  --pydoll-selftest [--timeout <秒>]\n");
    std::printf("                   M7B 新通道自检（批 2）：起守护进程 → hello → shutdown（**不开浏览器**）\n");
    std::printf("                   退出码 0=通过 / 1=失败 / 2=依赖缺失（无 Python / 无包目录）\n");
    std::printf("  --pydoll-login <id> [--timeout <秒>]\n");
    std::printf("                   M7B 新通道冒烟（批 2）：起**有头**浏览器打开条目站点，\n");
    std::printf("                   等**人工登录**（不代填密码 · §13）→ 判据 = 条目 cookie_names 命中 → 关窗\n");
    std::printf("                   退出码 0=登录成功 / 1=超时或失败 / 2=参数或依赖问题\n");

    std::printf("  --web-supply-probe [--provider <id>] [--timeout <秒>] [--get <路径?查询串>]\n");
    std::printf("                   M8 Phase 2 供料探针（**只读**）：按条目起有头 WebView2（需人工登录）→\n");
    std::printf("                   取样 device_id 候选 + 会话 Cookie（值脱敏）→ 报告写 ~/.brain-ai/logs/；\n");
    std::printf("                   带 --get 时用会话 Cookie 发**一次 GET**，判定站点接口是否下发签名材料\n");
    std::printf("                   退出码 0=完成 / 1=页面或会话未就绪 / 2=条目或站点不可用\n");

    std::printf("  --upload-selftest [--provider <id>] [--image <路径>] [--timeout <秒>]\n");
    std::printf("                   M8-14 站点上传模板自检：离线三组（图片准备 / 双证据判定 / 注册表与\n");
    std::printf("                   契约）→ 带 --provider 时**追加真机一次**（按条目把 --image 交给\n");
    std::printf("                   WebView2 页面里的站点自己的上传逻辑，并要求**双证据**齐备：\n");
    std::printf("                   页面附件区节点 + 站点上传请求回执；缺一即如实报错、不继续发送）\n");
    std::printf("                   退出码 0=全绿 / 1=有失败 / 2=站点不可用（非网页版 / 缺 login_url）\n");
    std::printf("                   真机示例：aiwrite.exe --upload-selftest --provider doubao-web --image \"D:\\a.png\"\n");
    std::printf("                   （需该站点已登录过一次；离屏窗口自动复用，用户无需干预）\n");

    std::printf("  --help           显示本帮助\n");
}

int pipeSelftest(int timeout_ms)
{
    namespace fs = std::filesystem;
    const auto   to_wide = [](const std::string& text) -> std::wstring {
        if (text.empty()) {
            return {};
        }
        const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                               static_cast<int>(text.size()), nullptr, 0);
        if (size <= 0) {
            return {};
        }
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                              wide.data(), size);
        return wide;
    };
    const auto text_field = [](const nlohmann::json& object, const char* key) -> std::string {
        if (object.is_object() && object.contains(key) && object[key].is_string()) {
            return object[key].get<std::string>();
        }
        return "?";
    };

    // ---- ① 包目录：<仓库根>/source/python（开发期）→ <exe>/python（部署期，M7B-11 拷贝）----
    std::vector<fs::path> package_candidates;
#ifdef AIWRITE_SOURCE_DIR
    // AIWRITE_SOURCE_DIR = <仓库根>/source ⇒ Python 包就在其下（`source/python/`）。
    // 2026-10-03 目录迁移（运行代码归源码树 / 运行期数据仍在 ~/.brain-ai）：原为 <仓库根>/python。
    package_candidates.emplace_back(fs::path(AIWRITE_SOURCE_DIR) / "python");
#endif
    package_candidates.emplace_back(aiwrite::paths::exeDir() / "python");
    fs::path package_dir;
    for (const fs::path& candidate : package_candidates) {
        if (fs::exists(candidate / "brain_ai_browser" / "__main__.py")) {
            package_dir = candidate;
            break;
        }
    }
    if (package_dir.empty()) {
        std::printf("[管道自检] 环境缺失（退出码 2）：找不到 brain_ai_browser 包目录\n");
        for (const fs::path& candidate : package_candidates) {
            std::printf("   候选：%s\n", candidate.string().c_str());
        }
        return 2;
    }

    // ---- ② 解释器：`.venv` 优先 → 退回 PATH 上的 python.exe ----
    fs::path python_exe;
    const fs::path venv_candidates[] = {package_dir / ".venv" / "Scripts" / "python.exe",
                                        aiwrite::paths::exeDir() / "python" / ".venv" /
                                            "Scripts" / "python.exe"};
    for (const fs::path& candidate : venv_candidates) {
        if (fs::exists(candidate)) {
            python_exe = candidate;
            break;
        }
    }
    const std::string python_text =
        python_exe.empty() ? std::string("python.exe") : python_exe.string();

    // ---- ③ 起守护进程（stdout/stderr → <日志目录>/pipe_selftest_daemon.log）----
    const std::string pipeName =
        std::string("aiwrite-browser-selftest-") + std::to_string(::GetCurrentProcessId());
    const fs::path log_path = aiwrite::paths::logsDir() / "pipe_selftest_daemon.log";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE log_handle = ::CreateFileW(to_wide(log_path.string()).c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log_handle == INVALID_HANDLE_VALUE) {
        std::printf("[管道自检] 环境缺失（退出码 2）：无法创建守护进程日志 %s\n",
                    log_path.string().c_str());
        return 2;
    }
    // 子进程 stdin 指向 NUL（自检不喂输入；避免部分 Python 构建对空句柄敏感）
    HANDLE nul_handle =
        ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);

    std::wstring command = L"\"" + to_wide(python_text) +
                           L"\" -m brain_ai_browser --serve --once --pipe-name " +
                           to_wide(pipeName) + L" --idle-timeout 20";
    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdInput   = nul_handle;
    si.hStdOutput  = log_handle;
    si.hStdError   = log_handle;
    PROCESS_INFORMATION pi{};
    const std::wstring  work_dir = to_wide(package_dir.string());

    const BOOL spawned = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW, nullptr,
                                          work_dir.empty() ? nullptr : work_dir.c_str(), &si, &pi);
    const DWORD spawn_error = spawned ? 0 : ::GetLastError();
    ::CloseHandle(log_handle);
    if (nul_handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(nul_handle);
    }
    if (spawned == 0) {
        std::printf("[管道自检] %s：无法启动守护进程（%lu，解释器 %s）\n",
                    spawn_error == ERROR_FILE_NOT_FOUND ? "环境缺失（退出码 2）" : "FAIL",
                    static_cast<unsigned long>(spawn_error), python_text.c_str());
        std::printf("   提示：在 %s 下建虚拟环境（python -m venv .venv && .venv\\Scripts\\pip "
                    "install -r requirements.txt）\n",
                    package_dir.string().c_str());
        return spawn_error == ERROR_FILE_NOT_FOUND ? 2 : 1;
    }
    ::CloseHandle(pi.hThread);

    // 自检收尾（**非生产口径**）：先等进程退出，超时才强杀
    const auto reap_child = [&pi](int wait_ms) -> int {
        if (::WaitForSingleObject(pi.hProcess, static_cast<DWORD>(wait_ms)) == WAIT_TIMEOUT) {
            std::printf("[管道自检] 守护进程未在 %d ms 内退出 → 自检收尾强杀"
                        "（生产路径禁止此手段，见 §13）\n",
                        wait_ms);
            ::TerminateProcess(pi.hProcess, 1);
            ::WaitForSingleObject(pi.hProcess, 3000);
        }
        DWORD code = 0;
        ::GetExitCodeProcess(pi.hProcess, &code);
        ::CloseHandle(pi.hProcess);
        return static_cast<int>(code);
    };
    // 失败诊断：打印守护进程日志尾部（stdout/stderr 都重定向到这里）
    const auto dump_log_tail = [&log_path](int lines) {
        std::ifstream stream(log_path, std::ios::binary);
        if (!stream) {
            return;
        }
        std::vector<std::string> tail;
        std::string              line;
        while (std::getline(stream, line)) {
            tail.push_back(std::move(line));
            if (tail.size() > static_cast<std::size_t>(lines)) {
                tail.erase(tail.begin());
            }
        }
        std::printf("---- 守护进程日志尾部（%s）----\n", log_path.string().c_str());
        for (const std::string& text : tail) {
            std::printf("  %s\n", text.c_str());
        }
    };

    // ---- ④ 连管道 → 1 命令（hello）→ 1 事件（ready）----
    std::atomic<bool> closing_seen{false};
    aiwrite::web::PipeClient client;
    client.onEvent([&closing_seen](const aiwrite::web::channel::Frame& frame) {
        std::printf("[事件] %-10s id=%-3s %s\n", frame.name.c_str(), frame.id.c_str(),
                    frame.payload_json.c_str());
        if (frame.kind == "evt" && frame.name == "stage" &&
            frame.payload_json.find("\"close\"") != std::string::npos) {
            closing_seen.store(true);
        }
    });

    std::string error;
    if (!client.connect(pipeName, timeout_ms, &error)) {
        std::printf("[管道自检] FAIL 连接守护进程失败：%s\n", error.c_str());
        client.close();
        const int code = reap_child(3000);
        std::printf("[管道自检] 守护进程退出码 = %d\n", code);
        dump_log_tail(15);
        return 1;
    }
    std::printf("[管道自检] 已连接 %s\n", pipeName.c_str());

    nlohmann::json ready;
    const bool     hello_ok = client.call("hello", nlohmann::json::object(), &ready, 5000, &error);
    const std::string proto_text =
        (ready.is_object() && ready.contains("proto") && ready["proto"].is_number_integer())
            ? std::to_string(ready["proto"].get<int>())
            : std::string("?");
    if (hello_ok) {
        std::printf("[管道自检] ready：proto=%s python=%s browser=%s\n", proto_text.c_str(),
                    text_field(ready, "python").c_str(), text_field(ready, "browser").c_str());
    } else {
        std::printf("[管道自检] FAIL hello 失败：%s\n", error.c_str());
    }
    const bool ready_ok = hello_ok && ready.is_object() && ready.contains("proto") &&
                          ready["proto"].is_number_integer() &&
                          ready["proto"].get<int>() == aiwrite::web::channel::kProtoVersion &&
                          !text_field(ready, "python").empty() &&
                          text_field(ready, "python") != "?" &&
                          !text_field(ready, "browser").empty() &&
                          text_field(ready, "browser") != "?";

    // ---- ⑤ shutdown → 等 stage{close} 事件（Python → C++ 事件通道）----
    nlohmann::json shutdown_fields = nlohmann::json::object();
    shutdown_fields["grace_ms"]    = 500;
    const bool shutdown_ok = client.sendCommand("shutdown", shutdown_fields, &error);
    if (!shutdown_ok) {
        std::printf("[管道自检] FAIL shutdown 发送失败：%s\n", error.c_str());
    }
    for (int i = 0; i < 150 && !closing_seen.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const bool event_ok = closing_seen.load();

    // ---- ⑥ 收尾：关通道 → 等守护进程退出（I23①：等进程退出，不强杀）----
    client.close();
    const int  daemon_code = reap_child(timeout_ms);
    const bool pass        = ready_ok && shutdown_ok && event_ok && daemon_code == 0;

    std::printf("[管道自检] %s（hello→ready=%s / 事件 stage{close}=%s / 守护进程退出码=%d / "
                "丢弃非法帧=%llu）\n",
                pass ? "PASS" : "FAIL", ready_ok ? "是" : "否", event_ok ? "是" : "否", daemon_code,
                static_cast<unsigned long long>(client.droppedFrames()));
    std::printf("[管道自检] 守护进程日志：%s\n", log_path.string().c_str());
    if (!pass) {
        dump_log_tail(15);
    }
    return pass ? 0 : 1;
}

int webSessionList(const std::string& json_path)
{
    using aiwrite::web::SessionStore;

    std::printf("\n===== 会话 / 登录态清单（批 3 · FEA-L02）=====\n");

    // ---- ① 本进程内存会话 ----
    SessionStore&                  store  = SessionStore::instance();
    const std::vector<std::string> sites  = store.sites();
    nlohmann::json                 memory = nlohmann::json::array();
    std::printf("① 内存会话（本进程）：%zu 个站点\n", sites.size());
    if (sites.empty()) {
        std::printf("   无 —— 凭证**只在内存**（I15：不写盘、不进日志）⇒ 全新进程必然为空：\n");
        std::printf("   要拿内存凭证，请在**同一次运行**里先登录"
                    "（界面「打开登录窗口」/ --login-selftest / --web-adapter-selftest）\n");
    }
    else {
        std::printf("   %-30s %-16s %-12s %-7s %-5s %-20s %s\n", "站点键（origin）", "条目 id",
                    "登录态", "Cookie", "凭证", "更新时间", "会话来源 URL");
        for (const std::string& site : sites) {
            const aiwrite::web::Session session = store.snapshot(site);
            const aiwrite::ai::ProviderSpec* spec =
                aiwrite::ai::providerSpecs().find(session.provider_id);
            std::string state_label = "非表内条目（按 origin 归档）";
            if (spec != nullptr) {
                const aiwrite::ai::WebSessionVerdict verdict = aiwrite::ai::webSessionState(
                    spec, aiwrite::web::webSessionEvidence(session));
                state_label = aiwrite::ai::webSessionStateLabel(verdict.state);
            }
            std::printf("   %-30s %-16s %-12s %-7zu %-5s %-20s %s\n", site.c_str(),
                        session.provider_id.empty() ? "（未标注）" : session.provider_id.c_str(),
                        state_label.c_str(), session.cookieCount(),
                        session.user_token.empty() ? "无" : "有", session.updated_at.c_str(),
                        session.url.c_str());
            nlohmann::json item;
            item["site"]         = site;
            item["provider_id"]  = session.provider_id;
            item["state_label"]  = state_label;
            item["logged_in"]    = session.loggedIn;
            item["cookie_count"] = session.cookieCount();
            item["has_token"]    = !session.user_token.empty();
            item["updated_at"]   = session.updated_at;
            item["url"]          = session.url;
            memory.push_back(item);
        }
    }
    // ---- ② L2 快照只读元数据（红线 I23③：C++ 侧永不读快照内容）----
    const aiwrite::web::SnapshotInfo info = aiwrite::web::inspectSnapshot();
    std::printf("\n② L2 登录态快照（DPAPI 加密；只有 Python 侧读写 —— 此处只读元数据、不解密）\n");
    std::printf("   路径      : %s\n", aiwrite::web::snapshotFile().string().c_str());
    std::printf("   状态      : %s\n", aiwrite::web::snapshotSummary(info).c_str());
    std::printf("   触发时机  : %s\n", aiwrite::web::snapshotTriggerNote().c_str());
    if (!info.exists) {
        std::printf("   提示      : 快照由守护进程（brain_ai_browser --serve）在登录后写出；"
                    "没跑过就不存在（不是错误）\n");
    }
    else if (!info.in_size_limit) {
        std::printf("   ⚠️ 超限    : > %llu 字节 ⇒ Python 侧按「损坏」处理（重登一次会重写）\n",
                    static_cast<unsigned long long>(aiwrite::web::kSnapshotMaxBytes));
    }

    // ---- ③ 网页版条目登录判据 ----
    std::printf("\n③ 网页版条目登录判据（站点无关 · I15′：cookie_names 命中 或 该 origin Cookie 非空）\n");
    nlohmann::json entries   = nlohmann::json::array();
    int            web_total = 0;
    for (const aiwrite::ai::ProviderSpec& spec : aiwrite::ai::providerSpecs().items) {
        if (spec.kind != "web") {
            continue;
        }
        ++web_total;
        std::printf("   %-16s %-26s %-6s %s\n", spec.id.c_str(), spec.display.c_str(),
                    spec.web.adapter.c_str(),
                    spec.web.cookie_names.empty()
                        ? "（未配 cookie_names ⇒ 判据只剩「该 origin Cookie 非空」）"
                        : joinNames(spec.web.cookie_names).c_str());
        nlohmann::json item;
        item["id"]           = spec.id;
        item["display"]      = spec.display;
        item["adapter"]      = spec.web.adapter;
        item["login_url"]    = spec.web.login_url;
        item["cookie_names"] = spec.web.cookie_names;
        entries.push_back(item);
    }
    std::printf("   共 %d 条网页版条目\n", web_total);

    // ---- 机器读版（供 loop 报告引用 / 与上一轮对比）----
    if (!json_path.empty()) {
        nlohmann::json doc;
        doc["schema"]          = "aiwrite.web_session_list/1";
        doc["generated_at"]    = reportTimestamp();
        doc["memory_sessions"] = memory;
        doc["l2_snapshot"]     = {{"path", aiwrite::web::snapshotFile().string()},
                                  {"exists", info.exists},
                                  {"bytes", info.bytes},
                                  {"modified_at", info.modified_at},
                                  {"in_size_limit", info.in_size_limit},
                                  {"max_bytes", aiwrite::web::kSnapshotMaxBytes},
                                  {"trigger_note", aiwrite::web::snapshotTriggerNote()}};
        doc["web_entries"]     = entries;
        std::string write_error;
        if (!writeTextFile(json_path, doc.dump(2) + "\n", &write_error)) {
            std::printf("\n[会话清单] 落盘失败：%s\n", write_error.c_str());
            return 1;
        }
        std::printf("\n[会话清单] 机器读版已写入：%s\n", json_path.c_str());
    }
    std::printf("[会话清单] 完成（内存会话 %zu 个 · L2 快照 %s · 网页版条目 %d 条）\n\n",
                sites.size(), info.exists ? "存在" : "不存在", web_total);
    std::fflush(stdout);
    return 0;
}

void printSpecList(const aiwrite::ai::ProviderSpecs& specs)
{
    for (const aiwrite::ai::ProviderSpec& spec : specs.items) {
        std::string detail;
        if (spec.kind == "web") {
            detail = "adapter=" + spec.web.adapter + " login=" + spec.web.login_url;
        }
        else {
            detail = "protocol=" + spec.protocol +
                     " base=" + (spec.api_base.empty() ? std::string("（手填）") : spec.api_base);
        }
        std::printf("   %-18s kind=%-8s %s\n", spec.id.c_str(), spec.kind.c_str(),
                    detail.c_str());
        std::printf("   %-18s 来源=%-18s 模型 %zu 个%s\n", "", spec.origin.c_str(),
                    spec.models.size(), spec.verified ? "  [已实测]" : "  [未验证]");
    }
}

int providerDump()
{
    const aiwrite::ai::ProviderSpecs specs = aiwrite::ai::providerSpecs();
    std::printf("[配置表] %s；条目 %zu 条（official %zu / web %zu）\n",
                specs.report.summary().c_str(), specs.items.size(),
                specs.countKind("official"), specs.countKind("web"));
    printSpecList(specs);
    std::printf("[配置表] 已实现协议：%s\n",
                joinNames(aiwrite::ai::implementedProtocols()).c_str());
    std::printf("[配置表] 已实现网页版适配器：%s\n",
                joinNames(aiwrite::ai::implementedWebAdapters()).c_str());
    std::printf("[配置表] 用户目录：%s（新增条目）\n",
                aiwrite::paths::userProvidersDir().string().c_str());
    std::printf("[配置表] 用户覆盖：%s\n", aiwrite::paths::userProvidersFile().string().c_str());
    for (const std::string& warning : specs.report.warnings) {
        std::printf("[配置表][警告] %s\n", warning.c_str());
    }
    for (const std::string& error : specs.report.errors) {
        std::printf("[配置表][错误] %s\n", error.c_str());
    }
    return 0;
}

