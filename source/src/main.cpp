// =============================================================================
//  AIwrite 主程序入口（M1：技术验证 + 项目骨架）
//  GUI 程序（/SUBSYSTEM:WINDOWS + mainCRTStartup），启动时初始化日志与配置目录
// =============================================================================
#include "ui/app.h"
#include "ui/editor_state.h"
#include "ui/output_panel.h"
#include "ai/deepseek_web_client.h"
#include "utils/config.h"
#include "utils/text_export.h"
#include "utils/credential.h"
#include "utils/log.h"
#include "utils/paths.h"
#include "web/webview_host.h"

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
                      consistency_ok && param_driven_ok && async_ok && session_ok && sink_ok && paste_ok;
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
