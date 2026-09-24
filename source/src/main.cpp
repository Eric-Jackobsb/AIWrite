// =============================================================================
//  AIwrite 主程序入口（M1：技术验证 + 项目骨架）
//  GUI 程序（/SUBSYSTEM:WINDOWS + mainCRTStartup），启动时初始化日志与配置目录
// =============================================================================
#include "ui/app.h"
#include "ui/editor_state.h"
#include "ui/output_panel.h"
#include "ai/deepseek_web_client.h"
#include "utils/log.h"
#include "utils/paths.h"
#include "web/webview_host.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <string>

namespace {

void print_usage()
{
    std::printf("AIwrite - AI 小说/剧本创作软件 (M1 技术验证)\n");
    std::printf("用法: aiwrite [选项]\n");
    std::printf("  --console        为日志额外分配控制台窗口（便于开发调试）\n");
    std::printf("  --login-selftest [--timeout <秒>]\n");
    std::printf("                   网页版登录自检：离屏跑「WebView2 环境 → 导航 → 提取 Cookie」\n");
    std::printf("                   退出码 0=通过 / 1=失败 / 2=超时（默认超时 30 秒）\n");
    std::printf("  --web-probe [--timeout <秒>]\n");
    std::printf("                   网页版协议探测：在已登录页面内读取 userToken 并请求 PoW 挑战\n");
    std::printf("                   退出码 0=取到 userToken / 1=失败（需先登录过一次）\n");
    std::printf("  --web-chat \"<提示词>\"\n");
    std::printf("                   网页版生成：取 PoW 挑战 → C++ 求解 → 调用 /api/v0/chat/completion\n");
    std::printf("                   退出码 0=取到文本 / 1=失败（需先登录过一次）\n");
    std::printf("  --run-selftest [--web]\n");
    std::printf("                   执行自检：创建示例工作流 → 运行到结束，打印各节点状态与统计\n");
    std::printf("                   --web：LLMGenerate 走网页版真实生成（需已登录过一次）\n");
    std::printf("                   默认 LLMGenerate 为占位实现，预期该节点 error、下游 skipped\n");
    std::printf("  --help           显示本帮助\n");
}

// --run-selftest：不打开窗口，直接验证 EditorState 的运行接线（start_run / tick_run / 状态文本）
int run_selftest(bool use_web)
{
    aiwrite::ui::EditorState& state = aiwrite::ui::editor();
    state.create_sample_workflow();

    // 默认（自检不联网）：把生成相关节点的 mode 置为 official（官方 API 占位分支）
    // --run-selftest --web：置为 web，走真实网页版生成
    // 注意：LLMGenerate 的模式取「provider 端口（ProviderConfig）」优先，故两处都要设
    for (aiwrite::engine::Node& node : state.graph.nodes) {
        if (node.type != "LLMGenerate" && node.type != "ProviderConfig") {
            continue;
        }
        if (aiwrite::engine::Param* mode = node.findParam("mode")) {
            mode->value = std::string(use_web ? "web" : "official");
        }
    }
    std::printf("[运行自检] 模式：%s\n", use_web ? "web（网页版真实生成）" : "official（离线占位）");

    std::printf("[运行自检] 示例工作流：节点 %zu，连线 %zu\n", state.graph.nodes.size(),
                state.graph.edges.size());
    if (!state.start_run()) {
        std::printf("[运行自检] FAIL：start_run 被拒绝（%s）\n", state.status.c_str());
        return 1;
    }

    int guard = 0;
    while (state.executor.running() && guard++ < 4096) {
        state.tick_run();
    }

    std::printf("[运行自检] 状态栏文本：%s\n", state.run_status_text().c_str());
    for (const aiwrite::engine::Node& node : state.graph.nodes) {
        std::string detail;
        if (!node.error_message.empty()) {
            detail = "（" + node.error_message + "）";
        }
        std::printf("[运行自检] %-3s %-15s %-8s %s\n", node.id.c_str(), node.type.c_str(),
                    aiwrite::engine::nodeStateName(node.state), detail.c_str());
    }
    std::printf("[运行自检] %s\n", state.executor.summary().c_str());

    const bool finished = state.executor.state() == aiwrite::engine::ExecState::Finished;

    // ---- PA-02：输出面板数据通道（与面板「复制全文 / 导出」同一实现）----
    const std::string output_text = aiwrite::ui::run_output_text(state.graph, state.executor);
    std::printf("[运行自检] 输出面板文本：%zu 字符\n", output_text.size());
    if (!output_text.empty()) {
        const std::size_t head = output_text.size() > 100 ? 100 : output_text.size();
        std::printf("[运行自检] 输出面板预览：%s…\n", output_text.substr(0, head).c_str());
    }

    // VA-07 取样：LLMGenerate 节点的运行结果（跑完不得出现在工作流 JSON 里）
    std::string llm_result;
    for (const aiwrite::engine::Node& node : state.graph.nodes) {
        if (node.type == "LLMGenerate") {
            llm_result = aiwrite::ui::node_output_text(state.graph, state.executor, node.id);
            break;
        }
    }

    // ---- M2-05：工作流文件往返（保存 → 打开，走 app 侧同一代码路径）----
    // 备份/还原 recent.json，避免自检污染真实最近列表
    const std::filesystem::path recent_file = aiwrite::paths::recent_file();
    std::string                 recent_backup;
    std::error_code             ec;
    const bool                  had_recent = std::filesystem::exists(recent_file, ec);
    if (had_recent) {
        std::ifstream     in(recent_file, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        recent_backup = buffer.str();
    }

    const std::filesystem::path temp_file =
        std::filesystem::temp_directory_path() / "aiwrite_run_selftest_workflow.json";
    std::string file_error;
    const bool  saved      = state.save_workflow_to(temp_file.string(), &file_error);
    const std::size_t nodes_before = state.graph.nodes.size();
    const bool  opened     = saved && state.open_workflow_from(temp_file.string(), &file_error);
    const bool  same       = opened && state.graph.nodes.size() == nodes_before;
    std::printf("[运行自检] 工作流文件往返：保存=%s / 打开=%s / 节点数一致=%s%s\n",
                saved ? "OK" : "失败", opened ? "OK" : "失败", same ? "OK" : "不一致",
                file_error.empty() ? "" : ("（" + file_error + "）").c_str());

    // VA-07：保存出来的 JSON 不得包含运行结果（运行态值原则）
    bool json_clean = true;
    if (saved && !llm_result.empty()) {
        std::ifstream     json_in(temp_file, std::ios::binary);
        std::stringstream json_buffer;
        json_buffer << json_in.rdbuf();
        const std::string json = json_buffer.str();
        const std::size_t probe_len = llm_result.size() < 24 ? llm_result.size() : 24;
        json_clean = json.find(llm_result.substr(0, probe_len)) == std::string::npos;
    }
    std::printf("[运行自检] VA-07 工作流 JSON 不含运行结果：%s\n", json_clean ? "OK" : "失败");

    std::filesystem::remove(temp_file, ec);
    if (had_recent) {
        std::ofstream out(recent_file, std::ios::binary | std::ios::trunc);
        out << recent_backup;
    }
    else {
        std::filesystem::remove(recent_file, ec);
    }

    const bool pass = finished && saved && opened && same && json_clean;
    std::printf("[运行自检] %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// --web-chat：不打开界面，用已登录 profile 直接调网页版生成（PoW 求解 + SSE 解析）
int web_chat_selftest(const std::string& prompt)
{
    aiwrite::web::LoginRequest request;
    request.offscreen              = true;
    request.timeout_seconds        = 0;    // 生命周期由本函数控制
    request.probe_after_load       = true; // 目的：拿到 Cookie + userToken
    request.auto_close_after_probe = false; // 窗口保留给后续 PoW 求解复用

    aiwrite::web::LoginWindow& window = aiwrite::web::login_window();
    std::string error;
    if (!window.start(request, &error)) {
        std::printf("[网页版生成] 启动登录窗口失败: %s\n", error.c_str());
        return 1;
    }

    // 等探测完成（窗口保持打开，供后续 PoW 求解复用）
    for (int i = 0; i < 200; ++i) { // ≤20 秒
        if (!aiwrite::web::SessionStore::instance().snapshot().probe.ran_at.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const aiwrite::web::Session session = aiwrite::web::SessionStore::instance().snapshot();
    std::printf("[网页版生成] 会话：Cookie %zu 条，userToken %s\n", session.cookie_count(),
                session.user_token.empty() ? "未获取" : "已获取（仅内存）");
    if (session.user_token.empty()) {
        std::printf("[网页版生成] FAIL：请先登录一次（`aiwrite.exe --login-selftest` 或界面"
                    "参数面板的「打开登录窗口」）\n");
        return 1;
    }

    aiwrite::ai::WebChatRequest chat;
    chat.prompt = prompt;
    const aiwrite::ai::WebChatResult result = aiwrite::ai::web_chat(session, chat);

    std::printf("[网页版生成] HTTP %d，PoW 尝试 %lld 次\n", result.http_status,
                result.pow_attempts);
    std::printf("---- 原始响应前几行 ----\n%s\n", result.raw_head.c_str());
    std::printf("---- 生成文本 ----\n%s\n", result.text.c_str());
    if (!result.error.empty()) {
        std::printf("---- 错误 ----\n%s\n", result.error.c_str());
    }

    const bool pass = result.ok && !result.text.empty();
    std::printf("[网页版生成] %s\n", pass ? "PASS" : "FAIL");

    aiwrite::web::login_window().request_close(); // 收尾：关闭离屏窗口
    aiwrite::web::login_window().join();
    aiwrite::web::SessionStore::instance().clear(); // 自检不留会话
    return pass ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    aiwrite::ui::AppOptions options;
    bool login_selftest   = false;
    bool run_selftest_flag = false;
    bool run_selftest_web  = false;
    bool web_probe_flag    = false;
    std::string web_chat_prompt;
    int  selftest_timeout = 30;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--console") {
            options.console = true;
        }
        else if (arg == "--login-selftest") {
            login_selftest = true;
        }
        else if (arg == "--web-probe") {
            web_probe_flag = true;
        }
        else if (arg == "--web-chat") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --web-chat 缺少取值\n");
                return 2;
            }
            web_chat_prompt = argv[++i];
        }
        else if (arg == "--run-selftest") {
            run_selftest_flag = true;
        }
        else if (arg == "--web") {
            run_selftest_web = true; // 与 --run-selftest 组合：真实调用网页版生成
        }
        else if (arg == "--timeout") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --timeout 缺少取值\n");
                return 2;
            }
            selftest_timeout = std::atoi(argv[++i]);
        }
        else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            print_usage();
            return 2;
        }
    }

    aiwrite::log::init(options.console);
    aiwrite::log::info("AIwrite 启动（M1 技术验证 + 项目骨架）");

    const int dir_failures = aiwrite::paths::ensure_data_dirs();
    if (dir_failures != 0) {
        aiwrite::log::warn("有 " + std::to_string(dir_failures) + " 个数据目录创建失败，请检查权限");
    }
    aiwrite::log::info("数据目录: " + aiwrite::paths::data_root().string());

    // 网页版登录自检（不需要 GUI，不进消息循环）
    if (login_selftest) {
        const int selftest_code = aiwrite::web::selftest(selftest_timeout);
        aiwrite::log::info("AIwrite 登录自检退出，返回码 " + std::to_string(selftest_code));
        aiwrite::log::shutdown();
        return selftest_code;
    }

    // 网页版协议探测（需要 profile 里已有登录态）
    if (web_probe_flag) {
        const int probe_code = aiwrite::web::protocol_probe(selftest_timeout);
        aiwrite::log::info("AIwrite 协议探测退出，返回码 " + std::to_string(probe_code));
        aiwrite::log::shutdown();
        return probe_code;
    }

    // 网页版生成（PoW + SSE；需要 profile 里已有登录态）
    if (!web_chat_prompt.empty()) {
        const int chat_code = web_chat_selftest(web_chat_prompt);
        aiwrite::log::info("AIwrite 网页版生成退出，返回码 " + std::to_string(chat_code));
        aiwrite::log::shutdown();
        return chat_code;
    }

    // 执行自检（不需要 GUI）：验证 EditorState 的运行接线
    if (run_selftest_flag) {
        const int selftest_code = run_selftest(run_selftest_web);
        aiwrite::log::info("AIwrite 运行自检退出，返回码 " + std::to_string(selftest_code));
        aiwrite::log::shutdown();
        return selftest_code;
    }

    const int exit_code = aiwrite::ui::run(options);

    // 若用户留着网页版登录窗口就退出了程序：先收尾（关窗口、等线程结束），再关日志
    aiwrite::web::stop_login_window();

    aiwrite::log::info("AIwrite 退出，返回码 " + std::to_string(exit_code));
    aiwrite::log::shutdown();
    return exit_code;
}
