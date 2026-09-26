// =============================================================================
//  AIwrite 主程序入口（M1：技术验证 + 项目骨架）
//  GUI 程序（/SUBSYSTEM:WINDOWS + mainCRTStartup），启动时初始化日志与配置目录
// =============================================================================
#include "ui/app.h"
#include "ui/editor_state.h"
#include "ui/output_panel.h"
#include "ai/deepseek_official_provider.h"
#include "ai/provider_spec.h"   // M_patchB L1：Provider 配置表（--provider-selftest）
#include "engine/node_registry.h"
#include "ai/deepseek_web_client.h"
#include "ai/dom_web_client.h"      // L3（PB2-13/15）：通用 DOM 站点适配器 + 选择器探测
#include "utils/config.h"
#include "utils/text_export.h"
#include "utils/credential.h"
#include "utils/log.h"
#include "utils/paths.h"
#include "web/webview_host.h"
#include "web/session_store.h"   // M_patchB：--provider-selftest 的网页版登录态检查

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

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
    std::printf("                   ＋ --provider <id>：按**配置表条目**的站点探测（PB2-23；\n");
    std::printf("                     严格解析、不回落；站点不可用 → 打印原因 + 退出码 2）\n");
    std::printf("  --web-adapter-selftest --provider <id>\n");
    std::printf("                   L3（PB2-15）选择器探测 / 诊断：在**已登录窗口**里只读检查\n");
    std::printf("                   input_selector / send / answer_selector / done_when / token_expr，\n");
    std::printf("                   并给可操作修复建议；退出码 0=全部命中 / 1=有缺项 / 2=站点不可用\n");
    std::printf("  --web-chat \"<提示词>\"\n");
    std::printf("                   网页版生成：取 PoW 挑战 → C++ 求解 → 调用 /api/v0/chat/completion\n");
    std::printf("                   退出码 0=取到文本 / 1=失败（需先登录过一次）\n");
    std::printf("  --web-session-selftest\n");
    std::printf("                   会话自动引导回归：先按用户手动路径开窗口（不探测）→ 再调 ensure_session\n");
    std::printf("                   应「窗口已开 → 自动补探测」拿到凭证；退出码 0=通过 / 1=失败\n");
    std::printf("  --run-selftest [--web]\n");
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
    std::printf("  --help           显示本帮助\n");
}

// --run-selftest：不打开窗口，直接验证 EditorState 的运行接线（start_run / tick_run / 状态文本）
//  * M_patchB L4（PB2-30①）：`--provider <id>` 可指定**任意网页版条目**（默认 = 表内第一个 web 条目）
int run_selftest(bool use_web, const std::string& provider_id)
{
    aiwrite::ui::EditorState& state = aiwrite::ui::editor();
    state.create_sample_workflow();

    // 默认（自检不联网）：把生成相关节点的 mode 置为 official（官方 API 占位分支）
    // --run-selftest --web：置为 web，走真实网页版生成
    // 注意：LLMGenerate 的模式取「provider 端口（ProviderConfig）」优先，故两处都要设
    // M_patchB L1 修订（PB2-22 / 决策 D-22②）：站点**不回落** → `--web` 必须显式选**网页版条目**
    std::string web_entry = provider_id;
    if (use_web && web_entry.empty()) {
        const std::vector<std::string> web_entry_ids = aiwrite::ai::provider_specs().ids("web");
        web_entry = web_entry_ids.empty() ? std::string() : web_entry_ids.front();
    }
    if (use_web) {
        // PB2-30①：显式指定的条目必须真的存在且是网页版条目（严格解析，**不**回落）
        const aiwrite::ai::ProviderSpec* spec = aiwrite::ai::provider_specs().find(web_entry);
        if (web_entry.empty()) {
            std::printf("[运行自检] ✗ 配置表里没有网页版条目（kind=web）：无法执行 --web 自检\n");
            return 1;
        }
        if (spec == nullptr) {
            std::printf("[运行自检] ✗ 表里没有 id=%s（用 --provider-dump 查看全部）\n", web_entry.c_str());
            return 2;
        }
        if (spec->kind != "web") {
            std::printf("[运行自检] ✗ 条目 %s 不是网页版条目（kind=%s）：--web 自检需要 kind=web"
                        "（决策 D-22②，站点不回落）\n",
                        web_entry.c_str(), spec->kind.c_str());
            return 2;
        }
        if (aiwrite::ai::web_site_error(spec) != std::string()) {
            std::printf("[运行自检] ✗ 条目 %s 站点不可用：%s\n", web_entry.c_str(),
                        aiwrite::ai::web_site_error(spec).c_str());
            return 2;
        }
        if (aiwrite::ai::web_login_only(spec)) {
            std::printf("[运行自检] ✗ 条目 %s 是**登录型条目**（缺生成字段）：请先补齐 web.input_selector / "
                        "send / answer_selector（用 --web-adapter-selftest --provider %s 诊断）\n",
                        web_entry.c_str(), web_entry.c_str());
            return 1;
        }
    }
    for (aiwrite::engine::Node& node : state.graph.nodes) {
        if (use_web && node.type == "ProviderConfig") {
            if (aiwrite::engine::Param* provider = node.findParam("provider")) {
                provider->value = web_entry; // 决策 D-22②：站点不回落，必须选网页版条目
            }
        }
        if (node.type != "LLMGenerate" && node.type != "ProviderConfig") {
            continue;
        }
        if (aiwrite::engine::Param* mode = node.findParam("mode")) {
            mode->value = std::string(use_web ? "web" : "official");
        }
    }
    std::printf("[运行自检] 模式：%s\n", use_web ? "web（网页版真实生成）" : "official（离线占位）");
    if (use_web) {
        std::printf("[运行自检] 网页版条目：%s（站点取自该条目，不回落）\n", web_entry.c_str());
    }

    std::printf("[运行自检] 示例工作流：节点 %zu，连线 %zu\n", state.graph.nodes.size(),
                state.graph.edges.size());
    // PC-05：本次自检把运行结果归档到临时目录（不污染用户 outputs），便于断言
    {
        aiwrite::Config selftest_config         = aiwrite::app_config();
        const std::filesystem::path archive_root =
            std::filesystem::temp_directory_path() / "aiwrite_run_selftest_outputs";
        std::error_code cleanup_ec;
        std::filesystem::remove_all(archive_root, cleanup_ec);
        selftest_config.output.archive_dir  = archive_root.string();
        selftest_config.output.keep_history = false;
        selftest_config.output.max_history  = 1;
        selftest_config.output.ttl_days     = 0;
        aiwrite::set_app_config(selftest_config);
    }

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

    // ---- PC-05：运行结果已自动归档（每节点 .txt + run.json）----
    bool        archive_ok = false;
    std::size_t archive_files = 0;
    bool        doc_found  = false;
    if (!state.last_archive_dir.empty()) {
        const std::filesystem::path dir(state.last_archive_dir);
        archive_ok = std::filesystem::exists(dir / "run.json");
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".txt") {
                continue;
            }
            ++archive_files;
            std::ifstream     in(entry.path(), std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            const std::string content    = buffer.str();
            const std::size_t probe_len  = llm_result.size() < 24 ? llm_result.size() : 24;
            if (!content.empty() &&
                (llm_result.empty() ||
                 content.find(llm_result.substr(0, probe_len)) != std::string::npos)) {
                doc_found = true; // 归档里确实含生成文档（离线模式下 LLM 失败，退化为"有内容"）
            }
        }
    }
    std::printf("[运行自检] PC-05 归档：%s（目录 %s，节点文件 %zu 个，含生成文档=%s）\n",
                archive_ok && doc_found ? "OK" : "失败",
                state.last_archive_dir.empty() ? "（无）" : state.last_archive_dir.c_str(),
                archive_files, doc_found ? "是" : "否");

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

    // ---- VA-01（PA-03）：三处呈现同源（画布节点摘要 / 参数面板结果区 / 输出面板全文）----
    bool consistency_ok = false;
    {
        const std::string all = aiwrite::ui::run_output_text(state.graph, state.executor);
        std::size_t       compared  = 0;
        bool              contained = true;
        for (const aiwrite::engine::NodeRunInfo& info : state.executor.runInfos()) {
            const std::string per_node =
                aiwrite::ui::node_output_text(state.graph, state.executor, info.node_id);
            if (per_node.empty()) {
                continue;
            }
            ++compared;
            if (all.find(per_node) == std::string::npos) {
                contained = false; // 输出面板全文必须包含每个节点全文
            }
        }
        consistency_ok = contained && compared > 0;
        std::printf("[运行自检] VA-01 三处一致：%s（比对 %zu 个有输出的节点，全文 %zu 字符）\n",
                    consistency_ok ? "OK" : "失败", compared, all.size());
    }

    // ---- P1-c：参数驱动（"无硬编码"断言）----
    // 改「文本输入」的「文本内容」参数（等价于在参数面板输入框里打字）后重新运行，
    // 断言：① 该节点运行输出 == 新文本（且与上次不同）② 下游「提示词模板」输出包含新文本
    // （即：真正发给 AI 的提示词来自输入框，而不是任何写死的示例文本）
    bool param_driven_ok = false;
    {
        const std::string      marker     = "P1C 参数驱动：这段文本来自输入框。";
        aiwrite::engine::Node* input_node = nullptr;
        aiwrite::engine::Node* tpl_node   = nullptr;
        for (aiwrite::engine::Node& node : state.graph.nodes) {
            if (input_node == nullptr && node.type == "TextInput") {
                input_node = &node;
            }
            if (tpl_node == nullptr && node.type == "PromptTemplate") {
                tpl_node = &node;
            }
        }
        aiwrite::engine::Param* text_param =
            (input_node != nullptr) ? input_node->findParam("text") : nullptr;
        if (input_node != nullptr && tpl_node != nullptr && text_param != nullptr) {
            const std::string before =
                aiwrite::ui::node_output_text(state.graph, state.executor, input_node->id);
            text_param->value = marker; // 模拟"参数面板输入框里改写文本内容"
            state.start_run();
            int guard2 = 0;
            while (state.executor.running() && guard2++ < 4096) {
                state.tick_run();
            }
            const std::string after  = aiwrite::ui::node_output_text(state.graph, state.executor,
                                                                     input_node->id);
            const std::string prompt = aiwrite::ui::node_output_text(state.graph, state.executor,
                                                                     tpl_node->id);
            const bool        changed = (after == marker) && (after != before);
            const bool        flowed  = prompt.find(marker) != std::string::npos;
            param_driven_ok           = changed && flowed;
            std::printf("[运行自检] P1-c 参数驱动（无硬编码）：%s（节点输出随参数变化=%s / "
                        "下游提示词含新文本=%s）\n",
                        param_driven_ok ? "OK" : "失败", changed ? "是" : "否", flowed ? "是" : "否");
        }
        else {
            std::printf("[运行自检] P1-c 参数驱动（无硬编码）：跳过（图中缺少 TextInput / "
                        "PromptTemplate 节点）\n");
            param_driven_ok = true;
        }
    }

    // ---- PB-01：线程化运行（事件队列 + Graph 不被工作线程写 + 快照同源）----
    bool async_ok = false;
    if (!use_web) {
        // 归位到 Idle（上一轮运行把状态写进了 Graph；异步运行只应改副本）
        for (aiwrite::engine::Node& node : state.graph.nodes) {
            node.state = aiwrite::engine::NodeState::Idle;
            node.error_message.clear();
        }

        std::string start_error;
        const bool  started = state.executor.startAsync(&state.graph, &start_error);

        std::vector<aiwrite::engine::RunEvent> events;
        int                                    guard3 = 0;
        while (state.executor.state() == aiwrite::engine::ExecState::Running && guard3++ < 20000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            state.executor.pumpEvents(&events);
        }
        // 【API 契约】先 join 再取最后一轮事件：会话结束的那一刻 state_ 已翻转为非 Running，
        // 而 worker 的 Finished 事件可能尚未入队 —— 必须先 stopAsync()（join）再 pumpEvents()
        state.executor.stopAsync();
        state.executor.pumpEvents(&events);

        std::size_t state_events = 0;
        std::size_t output_events = 0;
        std::size_t console_events = 0;
        std::size_t finish_events = 0;
        std::size_t delta_events  = 0;
        for (const aiwrite::engine::RunEvent& event : events) {
            switch (event.kind) {
            case aiwrite::engine::RunEvent::Kind::NodeState: ++state_events; break;
            case aiwrite::engine::RunEvent::Kind::NodeOutput: ++output_events; break;
            case aiwrite::engine::RunEvent::Kind::Console: ++console_events; break;
            case aiwrite::engine::RunEvent::Kind::Delta: ++delta_events; break;
            case aiwrite::engine::RunEvent::Kind::Finished: ++finish_events; break;
            }
        }

        // ① 主线程尚未应用事件 → Graph 仍是 Idle（证明工作线程只改自己的副本）
        bool graph_untouched = true;
        for (const aiwrite::engine::Node& node : state.graph.nodes) {
            if (node.state != aiwrite::engine::NodeState::Idle) {
                graph_untouched = false;
                break;
            }
        }

        // ② 快照 == 权威数据（runInfos / outputs）
        const aiwrite::engine::RunSnapshot snapshot =
            aiwrite::engine::makeSnapshot(state.graph, state.executor);
        bool same_view = snapshot.nodes.size() == state.executor.runInfos().size();
        for (const aiwrite::engine::NodeRunInfo& info : state.executor.runInfos()) {
            const aiwrite::engine::RunNodeView* view = snapshot.find(info.node_id);
            if (view == nullptr || view->state != info.state || view->type != info.type ||
                std::fabs(view->duration_ms - info.duration_ms) > 0.0001) {
                same_view = false;
                break;
            }
        }

        bool text_same = true;
        for (const aiwrite::engine::NodeRunInfo& info : state.executor.runInfos()) {
            const std::string authoritative =
                aiwrite::engine::nodeOutputText(state.graph, state.executor.outputs(), info.node_id);
            const aiwrite::engine::RunNodeView* view = snapshot.find(info.node_id);
            const std::string from_snapshot = (view != nullptr) ? view->text : std::string();
            if (authoritative != from_snapshot) {
                text_same = false;
                break;
            }
        }

        async_ok = started && finish_events == 1 && state_events > 0 && output_events > 0 &&
                   console_events > 0 && graph_untouched && same_view && text_same &&
                   state.executor.state() != aiwrite::engine::ExecState::Running;
        std::printf("[运行自检] PB-01 线程化：%s（启动=%s / 状态事件 %zu / 输出事件 %zu / Console %zu / "
                    "增量事件 %zu / Finished %zu / Graph 未被工作线程写=%s / 快照一致=%s / 文本一致=%s）\n",
                    async_ok ? "OK" : "失败", started ? "是" : "否", state_events, output_events,
                    console_events, delta_events, finish_events, graph_untouched ? "是" : "否",
                    same_view ? "是" : "否", text_same ? "是" : "否");
    }
    else {
        std::printf("[运行自检] PB-01 线程化：跳过（--web 模式避免二次联网；该断言在离线模式验证）\n");
        async_ok = true;
    }

    // ---- PB-01 第二步：EditorState 异步入口（start_run_async + tick_run 泵事件）----
    bool session_ok = true;
    if (!use_web) {
        for (aiwrite::engine::Node& node : state.graph.nodes) {
            node.state = aiwrite::engine::NodeState::Idle;
            node.error_message.clear();
        }

        const bool started = state.start_run_async();
        int        guard4  = 0;
        while (state.session_active() && guard4++ < 40000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            state.tick_run(); // 主线程：pump_run_events（写 Graph + 维护 run_snapshot）
        }
        const bool session_ended = !state.session_active();

        // 读模型 == 权威数据（状态 / 类型 / 文本），且 Graph 节点状态已由事件应用到位
        const aiwrite::engine::RunSnapshot& view = state.run_snapshot_view();
        bool same_view = view.nodes.size() == state.executor.runInfos().size();
        for (const aiwrite::engine::NodeRunInfo& info : state.executor.runInfos()) {
            const aiwrite::engine::RunNodeView* item = view.find(info.node_id);
            const aiwrite::engine::Node*        node = state.graph.findNode(info.node_id);
            if (item == nullptr || node == nullptr || item->state != info.state ||
                node->state != info.state ||
                item->text != aiwrite::engine::nodeOutputText(state.graph, state.executor.outputs(),
                                                              info.node_id)) {
                same_view = false;
                break;
            }
        }
        state.stop_run_async(); // 幂等（收尾已 join）
        session_ok = started && session_ended && same_view &&
                     state.executor.state() != aiwrite::engine::ExecState::Running;
        std::printf("[运行自检] PB-01 第二步（EditorState 异步 + RunSnapshot）：%s（启动=%s / 会话结束=%s / "
                    "快照·Graph == 权威=%s / 节点 %zu 个 / 状态 %s）\n",
                    session_ok ? "OK" : "失败", started ? "是" : "否", session_ended ? "是" : "否",
                    same_view ? "是" : "否", view.nodes.size(),
                    aiwrite::engine::execStateName(state.executor.state()));
    }
    else {
        std::printf("[运行自检] PB-01 第二步：跳过（--web 模式避免二次联网）\n");
    }

    // ---- M_textio P5：汇点节点（TextOutput）的运行态值可读（回归断言）----
    bool sink_ok = false;
    {
        std::string output_id;
        for (const aiwrite::engine::Node& node : state.graph.nodes) {
            if (node.type == "TextOutput") {
                output_id = node.id;
            }
        }
        std::string source_id;
        if (!output_id.empty()) {
            if (const aiwrite::engine::Edge* edge = state.graph.findEdgeIntoInput(output_id, "text")) {
                source_id = edge->from_node;
            }
        }
        std::string sink_text;
        std::string source_text;
        bool sink_same = false;
        if (!output_id.empty() && !source_id.empty()) {
            sink_text   = aiwrite::engine::nodeOutputText(state.graph, state.executor.outputs(), output_id);
            source_text = aiwrite::engine::nodeOutputText(state.graph, state.executor.outputs(), source_id);
            const aiwrite::engine::RunSnapshot snap = aiwrite::engine::makeSnapshot(state.graph, state.executor);
            const aiwrite::engine::RunNodeView* view = snap.find(output_id);
            sink_same = (sink_text == source_text) && view != nullptr && view->text == source_text;
        }
        sink_ok = sink_same;
        std::printf("[运行自检] M_textio 汇点结果可读：%s（TextOutput=%s / 上游=%s / 汇点 %zu 字符 / 上游 %zu 字符）\n",
                    sink_ok ? "OK" : "失败", output_id.c_str(), source_id.c_str(), sink_text.size(),
                    source_text.size());
    }

    // ---- 参数范围安全断言（防“Slider 断言崩溃”回归）----
    //  ImGui 的 Slider* 只接受 half-range（imgui_widgets.cpp SliderBehavior：
    //  IM_ASSERT(p_max <= T_MAX / 2)）；参数定义越界会让控件一渲染就 abort。
    bool param_range_ok = true;
    {
        std::size_t checked   = 0;
        std::string first_bad = "";
        for (const aiwrite::engine::Definition* definition :
             aiwrite::engine::NodeRegistry::instance().listTypes()) {
            for (const aiwrite::engine::Param& param : definition->params) {
                if (!param.min_value.has_value() || !param.max_value.has_value()) {
                    continue;
                }
                ++checked;
                const double lo    = *param.min_value;
                const double hi    = *param.max_value;
                const double limit = (param.type == aiwrite::engine::ParamType::Int)
                                         ? 1073741823.0
                                         : 1.7e38; // IM_S32_MAX/2 与 ≈FLT_MAX/2
                const bool bad = !(lo <= hi) || lo < -limit || hi > limit;
                if (bad) {
                    param_range_ok = false;
                    if (first_bad.empty()) {
                        first_bad = definition->type + "." + param.id;
                        std::printf("[运行自检]   越界参数：%s min=%g max=%g（安全上限 ±%g）\n",
                                    first_bad.c_str(), lo, hi, limit);
                    }
                }
            }
        }
        std::printf("[运行自检] 参数范围安全（ImGui Slider 上限）：%s（检查 %zu 个带范围参数）\n",
                    param_range_ok ? "OK" : "失败", checked);
    }

    // ---- M_rerun：文本生成「重新生成（新 seed）」断言（离线）----
    bool seed_ok = false;
    {
        aiwrite::ai::OfficialChatRequest request;
        request.api_key = "sk-selftest";
        request.prompt  = "hi";
        request.model   = "deepseek-chat";
        request.seed    = 12345;
        const nlohmann::json with_seed = aiwrite::ai::build_request_body(request);
        request.seed                   = 0;
        const nlohmann::json without_seed = aiwrite::ai::build_request_body(request);
        bool has_param = false;
        for (const aiwrite::engine::Node& node : state.graph.nodes) {
            if (node.type != "LLMGenerate") {
                continue;
            }
            if (const aiwrite::engine::Param* seed_param = node.findParam("seed")) {
                has_param = (seed_param->number(-1.0) == 0.0);
            }
        }
        seed_ok = with_seed.contains("seed") && with_seed["seed"].get<int>() == 12345 &&
                  !without_seed.contains("seed") && has_param;
        std::printf("[运行自检] M_rerun 新 seed：%s（含 seed=%s / 不含 seed=%s / 节点含 seed 参数=%s）\n",
                    seed_ok ? "OK" : "失败", with_seed.contains("seed") ? "是" : "否",
                    without_seed.contains("seed") ? "是" : "否", has_param ? "是" : "否");
    }

    // ---- F1（M3-04 重做）：复制/粘贴的模型级断言（先断言、后接线 UI）----
    // 说明：画布层守卫见 node_canvas.cpp（clamp_position / 未知手柄反向推送 / 视图跟随前校验），
    //       这里覆盖模型侧：新 id 唯一、位置偏移且有限、参数复制、选区内部连线重映射、可撤销。
    bool paste_ok = false;
    {
        // 注意：paste_clipboard() 内部会压撤销快照（Graph 重新分配）→ 源节点必须**按值**捕获，
        //       不能持有 Node* 指针（否则断言读到悬空内存，误报失败）。
        struct Source {
            std::string id;
            std::string type;
            float       x = 0.0f;
            float       y = 0.0f;
            std::vector<std::pair<std::string, nlohmann::json>> params;
        };
        std::vector<Source> sources;
        // 优先取「存在连线」的一对节点（覆盖"选区内部连线被一并粘贴"这条路径）
        std::vector<std::string> prefer;
        for (const aiwrite::engine::Edge& edge : state.graph.edges) {
            const aiwrite::engine::Node* from = state.graph.findNode(edge.from_node);
            const aiwrite::engine::Node* to   = state.graph.findNode(edge.to_node);
            if (from != nullptr && to != nullptr && from->type != "ProviderConfig" &&
                to->type != "ProviderConfig") {
                prefer = {edge.from_node, edge.to_node};
                break;
            }
        }

        for (const aiwrite::engine::Node& node : state.graph.nodes) {
            if (sources.size() >= 2) {
                break;
            }
            const bool wanted = !prefer.empty()
                                    ? (node.id == prefer[0] || node.id == prefer[1])
                                    : (node.type == "TextInput" || node.type == "PromptTemplate");
            if (!wanted) {
                continue;
            }
            Source source;
            source.id   = node.id;
            source.type = node.type;
            source.x    = node.x;
            source.y    = node.y;
            for (const aiwrite::engine::Param& param : node.params) {
                source.params.emplace_back(param.id, param.value);
            }
            sources.push_back(std::move(source));
        }

        const auto in_selection = [&sources](const std::string& id) {
            for (const Source& source : sources) {
                if (source.id == id) {
                    return true;
                }
            }
            return false;
        };

        // 选区**内部**连线数量（这些应被一并粘贴）
        std::size_t internal_edges = 0;
        for (const aiwrite::engine::Edge& edge : state.graph.edges) {
            if (in_selection(edge.from_node) && in_selection(edge.to_node)) {
                ++internal_edges;
            }
        }

        std::set<std::string> ids_before;
        for (const aiwrite::engine::Node& node : state.graph.nodes) {
            ids_before.insert(node.id);
        }
        const std::size_t nodes_before_paste = state.graph.nodes.size();
        const std::size_t edges_before_paste = state.graph.edges.size();

        state.selected_nodes.clear();
        for (const Source& source : sources) {
            state.selected_nodes.push_back(source.id);
        }
        state.copy_selection();
        const bool copied = state.has_clipboard();
        const bool pasted = state.paste_clipboard();

        std::vector<const aiwrite::engine::Node*> added;
        for (const aiwrite::engine::Node& node : state.graph.nodes) {
            if (ids_before.find(node.id) == ids_before.end()) {
                added.push_back(&node);
            }
        }

        const std::size_t edges_after_paste = state.graph.edges.size();

        const bool count_ok = copied && pasted && added.size() == sources.size() &&
                              state.graph.nodes.size() == nodes_before_paste + sources.size() &&
                              edges_after_paste == edges_before_paste + internal_edges;

        // 诊断：粘贴后"新增节点之间的连线"数量（应与选区内部连线数一致）
        std::size_t added_edges = 0;
        for (const aiwrite::engine::Edge& edge : state.graph.edges) {
            bool from_added = false;
            bool to_added   = false;
            for (const aiwrite::engine::Node* node : added) {
                from_added = from_added || node->id == edge.from_node;
                to_added   = to_added || node->id == edge.to_node;
            }
            if (from_added && to_added) {
                ++added_edges;
            }
        }

        bool placement_ok = true;
        bool params_ok    = true;
        for (const aiwrite::engine::Node* node : added) {
            const Source* origin = nullptr;
            for (const Source& source : sources) {
                if (source.type == node->type) {
                    origin = &source;
                    break;
                }
            }
            if (origin == nullptr) {
                placement_ok = false;
                continue;
            }
            constexpr float kOffset = 40.0f;
            if (!std::isfinite(node->x) || !std::isfinite(node->y) ||
                std::fabs(node->x) > 1.0e6f || std::fabs(node->y) > 1.0e6f ||
                std::fabs(node->x - (origin->x + kOffset)) > 0.01f ||
                std::fabs(node->y - (origin->y + kOffset)) > 0.01f) {
                placement_ok = false;
            }
            for (const std::pair<std::string, nlohmann::json>& expected : origin->params) {
                const aiwrite::engine::Param* actual = node->findParam(expected.first);
                if (actual == nullptr || actual->value != expected.second) {
                    params_ok = false;
                }
            }
        }

        const bool selection_ok = state.selected_nodes.size() == 2 &&
                                  state.request_navigate_to_content; // 视图跟随延后到画布绘制后
        const bool undo_ok = (state.undo.canUndo() && state.undo_once() &&
                              state.graph.nodes.size() == nodes_before_paste &&
                              state.graph.edges.size() == edges_before_paste);
        // 空剪贴板：清空选择后复制 → 剪贴板应为空，粘贴被拒绝
        state.selected_nodes.clear();
        state.copy_selection();
        const bool empty_ok = !state.has_clipboard() && !state.paste_clipboard();

        paste_ok = count_ok && placement_ok && params_ok && selection_ok && undo_ok && empty_ok;
        std::printf("[运行自检] F1 复制/粘贴：%s（新增 %zu 节点 / 连线 期望+%zu 实际+%zu 新增节点间 %zu / "
                    "位置=%s / 参数=%s / 选择与跟随=%s / 可撤销=%s / 空剪贴板=%s）\n",
                    paste_ok ? "OK" : "失败", added.size(), internal_edges,
                    edges_after_paste - edges_before_paste, added_edges,
                    placement_ok ? "OK" : "失败", params_ok ? "OK" : "失败",
                    selection_ok ? "OK" : "失败", undo_ok ? "OK" : "失败",
                    empty_ok ? "OK" : "失败");
    }

    const bool pass = finished && saved && opened && same && json_clean && archive_ok && doc_found &&
                      consistency_ok && param_driven_ok && async_ok && session_ok && sink_ok && seed_ok &&
                      param_range_ok &&
                      paste_ok;
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

// --web-session-selftest：回归「窗口已打开但内存无凭证」真 bug（2026-09-26）
//  步骤 1：按**用户手动路径**开登录窗口（probe_after_load=false → 内存里不会有 userToken）
//  步骤 2：调 web::ensure_session() —— 修复前：只“等” → 25s 超时失败（运行时报「未取得网页版凭证」）
//                                       修复后：识别「窗口已开 → 补探测」 → 数秒内取到凭证
int web_session_selftest()
{
    aiwrite::web::LoginWindow& window = aiwrite::web::login_window();

    // ---- 步骤 1：模拟用户手动点「打开登录窗口」（历史行为：不自动探测）----
    aiwrite::web::LoginRequest manual;
    manual.offscreen        = true;  // 离屏，不打扰用户
    manual.probe_after_load = false; // 【关键】刻意不探测 → 内存无 userToken
    std::string error;
    if (!window.start(manual, &error)) {
        std::printf("[会话自检] FAIL 启动登录窗口失败：%s\n", error.c_str());
        return 1;
    }
    for (int i = 0; i < 100; ++i) { // ≤10 秒：等页面加载（Cookie 提取即可）
        if (aiwrite::web::SessionStore::instance().snapshot().logged_in) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const aiwrite::web::Session before = aiwrite::web::SessionStore::instance().snapshot();
    const bool                 token_empty = before.user_token.empty();
    std::printf("[会话自检] 步骤 1：窗口已开（Cookie %zu 条，userToken %s）\n",
                before.cookie_count(), token_empty ? "空（符合预期）" : "非空（场景不成立）");

    // ---- 步骤 2：ensure_session 必须自己补探测，而不是干等 ----
    const bool ok = aiwrite::web::ensure_session(30000, &error);
    const aiwrite::web::Session after = aiwrite::web::SessionStore::instance().snapshot();
    if (ok) {
        std::printf("[会话自检] 步骤 2：ensure_session=OK（userToken 长度 %zu，仅内存）\n",
                    after.user_token.size());
    }
    else {
        std::printf("[会话自检] 步骤 2：ensure_session=失败：%s\n", error.c_str());
    }

    const bool pass = token_empty && ok;
    std::printf("[会话自检] %s（判定：开窗口时无凭证=%s / ensure_session 取得凭证=%s）\n",
                pass ? "PASS" : "FAIL", token_empty ? "是" : "否", ok ? "是" : "否");

    window.request_close(); // 收尾：关闭离屏窗口
    window.join();
    aiwrite::web::SessionStore::instance().clear(); // 自检不留会话
    return pass ? 0 : 1;
}

// --vlm-selftest：图片理解（M5-02）自检
//   ① 离线：编码图片 → 构造 OpenAI 兼容多模态请求体（不需要 Key）
//   ② 联网：Key 三级优先级（参数 → 环境变量 → 凭据库引用）解析成功时真实调用
//   退出码：0=通过（联网取到文本）/ 1=失败 / 2=缺少 API Key（离线部分已通过）
int vlm_selftest(const std::string& image_path, const std::string& api_base,
                 const std::string& model, const std::string& key_ref)
{
    aiwrite::ai::OfficialChatRequest request;
    request.api_base    = api_base;
    request.model       = model;
    request.prompt      = "用一句中文描述这张图片的主要内容。";
    request.images      = {image_path};
    request.temperature = 1.0;
    request.max_tokens  = 512;
    request.top_p       = 0.95;

    // ---- ① 离线：图片编码 + 请求体构造 ----
    std::string       encode_error;
    const std::string data_url =
        aiwrite::ai::encode_image_data_url(image_path, request.image_max_bytes, &encode_error);
    if (data_url.empty()) {
        std::printf("[图片理解自检] FAIL：图片编码失败：%s\n", encode_error.c_str());
        return 1;
    }
    const nlohmann::json       body     = aiwrite::ai::build_request_body(request);
    const nlohmann::json&      messages = body["messages"];
    // 注意：未设置 system_prompt 时 messages 只有 1 条 → 取最后一条（user）
    const nlohmann::json&      parts    = messages.back()["content"];
    const bool                 body_ok  =
        parts.is_array() && parts.size() == 2 && parts[0]["type"] == "text" &&
        parts[1]["type"] == "image_url" &&
        parts[1]["image_url"]["url"].get<std::string>() == data_url;
    std::printf("[图片理解自检] 端点：%s\n", aiwrite::ai::build_endpoint(api_base).c_str());
    std::printf("[图片理解自检] 模型：%s\n", model.c_str());
    std::printf("[图片理解自检] 图片：%s（data URL %zu 字符）\n", image_path.c_str(),
                data_url.size());
    if (!body_ok) {
        std::printf("[图片理解自检] FAIL：请求体不是预期的多模态结构（text + image_url）\n");
        return 1;
    }
    std::printf("[图片理解自检] 离线请求体构造 PASS\n");

    // ---- ② 联网：Key 解析（参数 → 环境变量 → 凭据库引用）----
    const aiwrite::utils::ResolvedSecret secret =
        aiwrite::utils::resolve_secret(std::string(), key_ref);
    if (secret.key.empty()) {
        std::printf("[图片理解自检] 未找到 API Key（环境变量 DEEPSEEK_API_KEY 或凭据库 %s）\n",
                    key_ref.c_str());
        std::printf("  提示：在界面「提供商配置 → API Key」填写一次即会自动入库（ref=%s）\n",
                    key_ref.c_str());
        return 2;
    }
    request.api_key = secret.key;
    std::printf("[图片理解自检] 凭据来源=%s，开始真实调用…\n", secret.source.c_str());

    const aiwrite::ai::OfficialChatResult result = aiwrite::ai::official_chat(request);
    std::printf("[图片理解自检] HTTP %d，耗时 %.0f ms\n", result.http_status, result.elapsed_ms);
    if (!result.ok || result.text.empty()) {
        std::printf("[图片理解自检] FAIL：%s\n",
                    result.error.empty() ? "未取到文本" : result.error.c_str());
        return 1;
    }
    std::printf("---- 生成文本 ----\n%s\n", result.text.c_str());
    std::printf("[图片理解自检] PASS\n");
    return 0;
}

// --provider-selftest / --provider-dump：Provider 配置表（M_patchB L1 / PB2-01、PB2-07 离线部分）
//   ① 表校验 + 离线断言（official 与 web 两类条目**同一套**）
//   ② --provider <id>：API 条目 → 有 Key 时发一条 ping；网页版条目 → 只检查登录态与端点
//   退出码：0=通过 / 1=失败 / 2=缺 Key（API）或未登录（web）—— 离线部分均已通过

std::string join_names(const std::vector<std::string>& items)
{
    std::string text;
    for (const std::string& item : items) {
        if (!text.empty()) {
            text += "、";
        }
        text += item;
    }
    return text.empty() ? std::string("（无）") : text;
}

void print_spec_list(const aiwrite::ai::ProviderSpecs& specs)
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

int provider_dump()
{
    const aiwrite::ai::ProviderSpecs specs = aiwrite::ai::provider_specs();
    std::printf("[配置表] %s；条目 %zu 条（official %zu / web %zu）\n",
                specs.report.summary().c_str(), specs.items.size(),
                specs.count_kind("official"), specs.count_kind("web"));
    print_spec_list(specs);
    std::printf("[配置表] 已实现协议：%s\n",
                join_names(aiwrite::ai::implemented_protocols()).c_str());
    std::printf("[配置表] 已实现网页版适配器：%s\n",
                join_names(aiwrite::ai::implemented_web_adapters()).c_str());
    std::printf("[配置表] 用户目录：%s（新增条目）\n",
                aiwrite::paths::user_providers_dir().string().c_str());
    std::printf("[配置表] 用户覆盖：%s\n", aiwrite::paths::user_providers_file().string().c_str());
    for (const std::string& warning : specs.report.warnings) {
        std::printf("[配置表][警告] %s\n", warning.c_str());
    }
    for (const std::string& error : specs.report.errors) {
        std::printf("[配置表][错误] %s\n", error.c_str());
    }
    return 0;
}

int provider_selftest(const std::string& provider_id, const std::string& api_base,
                      const std::string& model, const std::string& key_ref)
{
    const aiwrite::ai::ProviderSpecs specs = aiwrite::ai::provider_specs();
    std::printf("[Provider 自检] 表：%s；条目 %zu 条（official %zu / web %zu）\n",
                specs.report.summary().c_str(), specs.items.size(),
                specs.count_kind("official"), specs.count_kind("web"));
    print_spec_list(specs);

    int       offline_passed = 0;
    const int offline_failed = aiwrite::ai::provider_spec_selftest(&offline_passed);
    if (provider_id.empty()) {
        std::printf("[Provider 自检] 未指定 --provider：仅完成离线部分（表校验 + 断言）\n");
        return offline_failed == 0 ? 0 : 1;
    }

    const aiwrite::ai::ProviderSpec* spec = specs.find(provider_id);
    if (spec == nullptr) {
        std::printf("[Provider 自检] FAIL：表里没有 id=%s（用 --provider-dump 查看全部）\n",
                    provider_id.c_str());
        return 1;
    }

    // ---- 网页版条目：只检查登录态与端点一致性（不发送内容，避免误触发站点）----
    if (spec->kind == "web") {
        const std::string token =
            aiwrite::web::SessionStore::instance().snapshot().user_token;
        const bool logged_in = !token.empty();
        std::printf("[Provider 自检] 网页版 %s：适配器=%s\n", spec->display.c_str(),
                    spec->web.adapter.c_str());
        std::printf("[Provider 自检] 登录页：%s\n", spec->web.login_url.c_str());
        if (spec->web.adapter == "dom") {
            // M_patchB L1 修订（PB2-26）：DOM 条目**没有内置端点**（真实交互由 DOM 执行器在页面内完成）
            //  * 如实显示「不适用 / 生成未就绪」，避免把内置默认（DeepSeek）端点误读成本条目的端点
            std::printf("[Provider 自检] 端点：不适用（DOM 适配器；无内置端点）\n");
            std::printf("[Provider 自检] 生成：%s\n",
                        aiwrite::ai::web_login_only(spec)
                            ? "未就绪（登录型站点条目：缺选择器 / 发送 / 取答案）"
                            : "未就绪（DOM 适配器本版本尚未实现，L3 PB2-13…16）");
        }
        else {
            std::printf("[Provider 自检] 端点：host=%s completion=%s challenge=%s\n",
                        spec->web.endpoints.host.c_str(),
                        spec->web.endpoints.completion_path.c_str(),
                        spec->web.endpoints.challenge_path.c_str());
        }
        std::printf("[Provider 自检] 探测路径：%s\n", join_names(spec->web.probe_paths).c_str());
        std::printf("[Provider 自检] 登录态=%s（内存凭证，程序退出即销毁）\n",
                    logged_in ? "有" : "无");
        if (!logged_in) {
            std::printf("  提示：先在界面「提供商配置 → 打开登录窗口」登录一次，或跑 "
                        "--web-session-selftest\n");
            return 2;
        }
        return offline_failed == 0 ? 0 : 1;
    }

    // ---- API 条目：协议实现检查 → Key 解析（env 列表 → 凭据库）→ 一条 ping ----
    if (spec->protocol != "openai") {
        std::printf("[Provider 自检] %s 的 protocol=%s 本版本尚未实现（已实现：%s）"
                    "：跳过联网测试\n",
                    spec->display.c_str(), spec->protocol.c_str(),
                    join_names(aiwrite::ai::implemented_protocols()).c_str());
        return offline_failed == 0 ? 0 : 1;
    }
    const std::string effective_base = aiwrite::ai::resolve_api_base(*spec, api_base);
    const std::string effective_model =
        aiwrite::ai::resolve_model(*spec, model, std::string(), false);
    const std::string effective_ref = aiwrite::ai::resolve_key_ref(*spec, key_ref);
    std::printf("[Provider 自检] 生效地址=%s；生效模型=%s；引用名=%s\n", effective_base.c_str(),
                effective_model.c_str(), effective_ref.c_str());
    if (effective_base.empty()) {
        std::printf("[Provider 自检] FAIL：该条目未填 api_base，请用 --api-base 指定\n");
        return 1;
    }

    std::string key;
    std::string key_source;
    for (const std::string& env_name : spec->env_names) {
        const char* value = std::getenv(env_name.c_str());
        if (value != nullptr && *value != '\0') {
            key        = value;
            key_source = "环境变量 " + env_name;
            break;
        }
    }
    if (key.empty()) {
        std::string       load_error;
        const std::string stored = aiwrite::utils::load_credential(effective_ref, &load_error);
        if (!stored.empty()) {
            key        = stored;
            key_source = "凭据库 " + effective_ref;
        }
    }
    if (key.empty()) {
        std::printf("[Provider 自检] 未找到 API Key（环境变量 %s / 凭据库 %s）\n",
                    join_names(spec->env_names).c_str(), effective_ref.c_str());
        std::printf("  提示：在界面「提供商配置 → API Key」填写一次即自动入库\n");
        return 2;
    }
    std::printf("[Provider 自检] 凭据来源=%s，发一条 ping（消耗极小额度）…\n", key_source.c_str());

    aiwrite::ai::OfficialChatRequest request;
    request.api_base    = effective_base;
    request.model       = effective_model;
    request.prompt      = "请只回复：pong";
    request.max_tokens  = 16;
    request.temperature = 0.0;
    request.top_p       = 1.0;
    request.api_key     = key;
    const aiwrite::ai::OfficialChatResult ping = aiwrite::ai::official_chat(request);
    std::printf("[Provider 自检] HTTP %d，耗时 %.0f ms\n", ping.http_status, ping.elapsed_ms);
    if (!ping.ok || ping.text.empty()) {
        std::printf("[Provider 自检] FAIL：%s\n",
                    ping.error.empty() ? "未取到文本" : ping.error.c_str());
        return 1;
    }
    std::printf("[Provider 自检] 返回：%s\n", ping.text.substr(0, 120).c_str());
    std::printf("[Provider 自检] PASS\n");
    return offline_failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    aiwrite::ui::AppOptions options;
    bool login_selftest   = false;
    bool run_selftest_flag = false;
    bool run_selftest_web  = false;
    bool web_probe_flag    = false;
    bool web_session_selftest_flag = false;
    bool dom_adapter_selftest_flag = false; // L3（PB2-15）：选择器探测 / 诊断
    bool dom_dump_flag             = false; // L4（PB2-29 / v15）：选择器候选枚举
    std::string web_chat_prompt;
    bool        vlm_selftest_flag = false;                              // M5-02 图片理解自检
    std::string vlm_image;                                              // --image
    std::string vlm_api_base;                                           // 空 = 用表/内置默认（D-07 智谱）
    std::string vlm_model;                                              // 空 = 用表/内置默认
    std::string vlm_key_ref;                                            // 空 = 用表/内置默认
    bool        provider_selftest_flag = false;                         // M_patchB L1 配置表自检
    bool        provider_dump_flag     = false;                         // 打印生效配置表
    std::string provider_id;                                            // --provider <id>
    int  selftest_timeout = 30;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--console") {
            options.console = true;
        }
        else if (arg == "--login-selftest") {
            login_selftest = true;
        }
        else if (arg == "--cred-list") {
            const std::vector<aiwrite::utils::CredentialInfo> items =
                aiwrite::utils::list_credentials(30);
            std::printf("凭据库（%s）：%zu 条\n", aiwrite::utils::credentials_dir().c_str(),
                        items.size());
            for (const aiwrite::utils::CredentialInfo& info : items) {
                std::printf("  %-28s 更新=%lld 剩余TTL=%d 天\n", info.ref.c_str(),
                            info.updated_at, info.ttl_remaining_days);
            }
            return 0;
        }
        else if (arg == "--cred-erase") {
            if (i + 1 >= argc) {
                std::printf("用法: aiwrite --cred-erase <ref>\n");
                return 2;
            }
            const std::string target = argv[++i];
            std::string       erase_error;
            const bool        erased = aiwrite::utils::erase_credential(target, &erase_error);
            std::printf("凭据 %s：%s\n", target.c_str(),
                        erased ? "已删除" : erase_error.c_str());
            return erased ? 0 : 1;
        }
        else if (arg == "--export-selftest") {
            const int failed = aiwrite::utils::text_export_selftest();
            std::printf("文本导出自检：%s（失败 %d 项，详见日志）\n", failed == 0 ? "PASS" : "FAIL", failed);
            return failed == 0 ? 0 : 1;
        }
        else if (arg == "--cred-selftest") {
            const int failed = aiwrite::utils::credential_selftest();
            std::printf("凭据自检：%s（失败 %d 项，详见日志）\n", failed == 0 ? "PASS" : "FAIL", failed);
            return failed == 0 ? 0 : 1;
        }
        else if (arg == "--cred-purge") {
            std::string purge_error;
            const int   removed = aiwrite::utils::purge_expired_credentials(30, &purge_error);
            std::printf("已清理过期凭据 %d 个（TTL 30 天）\n", removed);
            return 0;
        }
        else if (arg == "--web-probe") {
            web_probe_flag = true;
        }
        else if (arg == "--web-adapter-selftest") {
            dom_adapter_selftest_flag = true;
        }
        else if (arg == "--web-dom-dump") {
            dom_dump_flag = true; // L4（PB2-29）：只读枚举页面候选元素 + 建议选择器
        }
        else if (arg == "--web-session-selftest") {
            web_session_selftest_flag = true;
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
        else if (arg == "--vlm-selftest") {
            vlm_selftest_flag = true;
        }
        else if (arg == "--provider-selftest") {
            provider_selftest_flag = true;
        }
        else if (arg == "--provider-dump") {
            provider_dump_flag = true;
        }
        else if (arg == "--provider") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --provider 缺少取值\n");
                return 2;
            }
            provider_id = argv[++i];
        }
        else if (arg == "--image") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --image 缺少取值\n");
                return 2;
            }
            vlm_image = argv[++i];
        }
        else if (arg == "--api-base") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --api-base 缺少取值\n");
                return 2;
            }
            vlm_api_base = argv[++i];
        }
        else if (arg == "--model") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --model 缺少取值\n");
                return 2;
            }
            vlm_model = argv[++i];
        }
        else if (arg == "--key-ref") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --key-ref 缺少取值\n");
                return 2;
            }
            vlm_key_ref = argv[++i];
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

    // 网页版会话自动引导回归（窗口已开但内存无凭证 → ensure_session 应自动补探测）
    if (web_session_selftest_flag) {
        const int session_code = web_session_selftest();
        aiwrite::log::info("AIwrite 会话自检退出，返回码 " + std::to_string(session_code));
        aiwrite::log::shutdown();
        return session_code;
    }

    // 网页版登录自检（不需要 GUI，不进消息循环）
    if (login_selftest) {
        const int selftest_code = aiwrite::web::selftest(selftest_timeout);
        aiwrite::log::info("AIwrite 登录自检退出，返回码 " + std::to_string(selftest_code));
        aiwrite::log::shutdown();
        return selftest_code;
    }

    // L3（PB2-15）：选择器探测 / 诊断（按 dom 条目；只读，不发送内容）
    if (dom_adapter_selftest_flag) {
        int code = 2;
        if (provider_id.empty()) {
            std::printf("[选择器探测] 请用 --provider <id> 指定要诊断的网页版条目；当前表里的 web 条目：\n");
            for (const aiwrite::ai::ProviderSpec& spec : aiwrite::ai::provider_specs().items) {
                if (spec.kind != "web") {
                    continue;
                }
                const char* kind_text = (spec.web.adapter == "dom")
                                            ? (aiwrite::ai::web_login_only(&spec) ? "dom · 登录型"
                                                                                  : "dom")
                                            : spec.web.adapter.c_str();
                std::printf("   %-16s %-28s %s\n", spec.id.c_str(), spec.display.c_str(), kind_text);
            }
            std::printf("  例：aiwrite.exe --web-adapter-selftest --provider kimi-web\n");
        }
        else {
            code = aiwrite::ai::dom_adapter_selftest(provider_id, selftest_timeout);
        }
        aiwrite::log::info("AIwrite 选择器探测退出，返回码 " + std::to_string(code));
        aiwrite::log::shutdown();
        return code;
    }

    // L4（PB2-29 / v15）：选择器**候选枚举**（只读）—— 输出页面候选元素指纹 + 建议选择器
    if (dom_dump_flag) {
        int code = 2;
        if (provider_id.empty()) {
            std::printf("[候选枚举] 请用 --provider <id> 指定网页版条目"
                        "（例：--web-dom-dump --provider kimi-web）\n");
        }
        else {
            code = aiwrite::ai::dom_selector_dump(provider_id, selftest_timeout);
        }
        aiwrite::log::info("AIwrite 选择器候选枚举退出，返回码 " + std::to_string(code));
        aiwrite::log::shutdown();
        return code;
    }

    // 网页版协议探测（需要 profile 里已有登录态）
    if (web_probe_flag) {
        // M_patchB L1 修订（PB2-23）：`--web-probe --provider <id>` = 按**条目**的站点（严格解析，不回落）
        const int probe_code =
            provider_id.empty()
                ? aiwrite::web::protocol_probe(selftest_timeout)
                : aiwrite::web::protocol_probe_for_provider(provider_id, selftest_timeout);
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

    // 图片理解自检（M5-02）：离线请求体断言 + 有 Key 时真实读图生成
    if (vlm_selftest_flag) {
        std::string image = vlm_image;
#ifdef AIWRITE_SOURCE_DIR
        if (image.empty()) {
            image = (std::filesystem::path(AIWRITE_SOURCE_DIR) / "assets" / "images" /
                     "sample.png")
                        .string();
        }
#endif
        if (image.empty()) {
            std::fprintf(stderr, "请用 --image <路径> 指定要理解的图片\n");
            return 2;
        }
        const std::string effective_api_base =
            vlm_api_base.empty() ? std::string("https://open.bigmodel.cn/api/paas/v4")
                                 : vlm_api_base;
        const std::string effective_model =
            vlm_model.empty() ? std::string("glm-4v-flash") : vlm_model;
        const std::string effective_key_ref =
            vlm_key_ref.empty() ? std::string("brain-ai/zhipu") : vlm_key_ref;
        const int vlm_code = vlm_selftest(image, effective_api_base, effective_model,
                                          effective_key_ref);
        aiwrite::log::info("AIwrite 图片理解自检退出，返回码 " + std::to_string(vlm_code));
        aiwrite::log::shutdown();
        return vlm_code;
    }

    // Provider 配置表（M_patchB L1）：打印生效表 / 表校验 + 离线断言（+ 可选联网 ping）
    if (provider_dump_flag) {
        const int dump_code = provider_dump();
        aiwrite::log::shutdown();
        return dump_code;
    }
    if (provider_selftest_flag) {
        const int code = provider_selftest(provider_id, vlm_api_base, vlm_model, vlm_key_ref);
        aiwrite::log::info("AIwrite Provider 自检退出，返回码 " + std::to_string(code));
        aiwrite::log::shutdown();
        return code;
    }

    // 执行自检（不需要 GUI）：验证 EditorState 的运行接线
    if (run_selftest_flag) {
        const int selftest_code = run_selftest(run_selftest_web, provider_id);
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
