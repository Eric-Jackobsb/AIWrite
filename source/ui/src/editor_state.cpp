#include "editor_state.h"

#include "autosave.h"
#include "node_registry.h"
#include "validate.h"
#include "workflow_io.h"
#include "nodes.h"
#include "output_panel.h"
#include "config.h"
#include "file_dialog.h"
#include "log.h"
#include "text_export.h"
#include "output_archive.h"
#include "paths.h"
#include "probe_verdict.h"   // M_patchAB（PB-07）：会话失效错误串反查（纯函数）
#include "provider_resolve.h" // M_patchAB（PB-07）：失败节点 → 生效条目 id（开哪一站的登录窗口）
#include "failure_summary.h"  // DEBT-L8（M6-04）：失败汇总纯函数（collectFailures / summarizeFailures）

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace aiwrite::ui {

namespace {

// PA-04：逐节点运行明细（机器可读，写入 workflow.log）
//  形如：[运行明细] n1(TextInput)=done/0ms n2(PromptTemplate)=done/1ms n3(LLMGenerate)=error/5003ms/err:…
std::string run_detail_text(const engine::Executor& executor)
{
    std::string text = "[运行明细]";
    for (const engine::NodeRunInfo& info : executor.runInfos()) {
        text += " " + info.node_id + "(" + info.type + ")=" + engine::nodeStateName(info.state) + "/" +
                std::to_string(static_cast<long long>(info.duration_ms + 0.5)) + "ms";
        if (!info.error.empty()) {
            std::string error = info.error;
            std::replace(error.begin(), error.end(), '\n', ' ');
            if (error.size() > 80) {
                error = error.substr(0, 77) + "...";
            }
            text += "/err:" + error;
        }
    }
    return text;
}

} // namespace

EditorState& editor()
{
    static EditorState state;
    return state;
}

void EditorState::setStatus(const std::string& text)
{
    status = text;
}

void EditorState::logSummary(const std::string& prefix) const
{
    log::info(prefix + "（节点 " + std::to_string(graph.nodes.size()) + "，连线 " +
              std::to_string(graph.edges.size()) + "）");
}

// ------------------------------------------------------------------ 撤销 -----
void EditorState::snapshot(const std::string& label)
{
    // 运行中修改工作流 → 计划与图不再一致，先终止本次运行（M2-04 守卫）
    abortRunIfAny("修改工作流（" + label + "）");
    undo.push(graph, label);
}

bool EditorState::undoOnce()
{
    engine::Graph restored;
    std::string   label;
    if (!undo.undo(graph, restored, &label)) {
        setStatus("没有可撤销的操作");
        return false;
    }
    graph = std::move(restored);
    selected_nodes.clear();
    selected_node.clear();
    logSummary("[撤销] " + label);
    setStatus("已撤销：" + label);
    return true;
}

bool EditorState::redoOnce()
{
    engine::Graph restored;
    std::string   label;
    if (!undo.redo(graph, restored, &label)) {
        setStatus("没有可重做的操作");
        return false;
    }
    graph = std::move(restored);
    selected_nodes.clear();
    selected_node.clear();
    logSummary("[重做] " + label);
    setStatus("已重做：" + label);
    return true;
}

// ----------------------------------------------------------------- 工作流 ----
void EditorState::clearWorkflow()
{
    graph.clear();
    undo.clear();
    selected_nodes.clear();
    selected_node.clear();
    clipboard_nodes_.clear();
    clipboard_edges_.clear();
    logSummary("[新建] 已清空画布");
    setStatus("画布已清空（撤销栈也已清空）");

    // PC-01/02：清空画布**不删**快照（保守：不替用户决定丢弃可恢复内容），只重设基线
    markAutosaveBaseline();
}

void EditorState::createSampleWorkflow()
{
    graph.clear();
    undo.clear();

    graph.name        = "默认工作流";
    graph.description = "文本输入 → 提示词模板 → 文本生成 → 文本输出（提供商配置接入文本生成的 provider 输入）";

    createNode("TextInput", -420.0f, -40.0f);
    createNode("PromptTemplate", -120.0f, -40.0f);
    createNode("LLMGenerate", 220.0f, -40.0f);
    createNode("TextOutput", 560.0f, -40.0f);
    // 提供商配置放在文本生成下方，其 provider 输出接到文本生成的 provider 输入
    createNode("ProviderConfig", 220.0f, 260.0f);

    if (graph.nodes.size() == 5) {
        const std::string input_id    = graph.nodes[0].id;
        const std::string prompt_id   = graph.nodes[1].id;
        const std::string llm_id      = graph.nodes[2].id;
        const std::string output_id   = graph.nodes[3].id;
        const std::string provider_id = graph.nodes[4].id;

        std::string error;
        graph.addEdge(input_id, "text", prompt_id, "vars", &error);
        graph.addEdge(prompt_id, "text", llm_id, "prompt", &error);
        graph.addEdge(llm_id, "text", output_id, "text", &error);
        // 设计 §9.2：ProviderConfig 输出 provider 句柄 → 推理节点的 provider 输入
        graph.addEdge(provider_id, "provider", llm_id, "provider", &error);

        if (engine::Node* input = graph.findNode(input_id)) {
            if (engine::Param* param = input->findParam("text")) {
                param->value = std::string("从前有座山，山里有座庙。");
            }
        }
    }

    // 默认启动工作流落盘：~/.brain-ai/workflows/default.json（设计 §11.3 Workflow 存储）
    // 注意：密钥类参数（is_secret，如 API Key）不会写入文件
    const std::filesystem::path default_file = paths::workflowsDir() / "default.json";
    std::string                 save_error;
    if (engine::saveWorkflow(graph, default_file, &save_error)) {
        log::info("[默认工作流] 已保存到 " + default_file.string());
    }
    else {
        log::warn("[默认工作流] 保存失败: " + save_error);
    }

    undo.clear(); // 默认工作流作为初始状态，不进入撤销历史
    selected_nodes.clear();
    selected_node.clear();
    request_navigate_to_content = true;
    logSummary("[默认工作流] 已创建");
    setStatus("已创建默认工作流（含提供商配置 → 文本生成），并保存到 default.json");
}

// ------------------------------------------------------------------ 节点 -----
std::string EditorState::createNode(const std::string& type, float x, float y)
{
    std::string error;
    const std::string id = graph.addNode(type, x, y, &error);
    if (id.empty()) {
        log::error("创建节点失败: " + error);
        setStatus("创建节点失败：" + error);
        return {};
    }

    const engine::Definition* definition = engine::NodeRegistry::instance().find(type);
    const std::string name = (definition != nullptr) ? definition->display_name : type;
    log::info("创建节点 " + id + "（" + name + "）");
    setStatus("已创建节点：" + name + "（" + id + "）");
    return id;
}

bool EditorState::deleteNode(const std::string& node_id)
{
    const engine::Node* node = graph.findNode(node_id);
    if (node == nullptr) {
        return false;
    }
    const std::string title = node->title;

    if (!graph.removeNode(node_id)) {
        return false;
    }

    selected_nodes.erase(std::remove(selected_nodes.begin(), selected_nodes.end(), node_id),
                         selected_nodes.end());
    if (selected_node == node_id) {
        selected_node.clear();
    }
    log::info("删除节点 " + node_id + "（" + title + "）");
    return true;
}

bool EditorState::deleteSelected()
{
    if (selected_nodes.empty() && selected_links.empty()) {
        setStatus("没有选中的节点或连线");
        return false;
    }

    const std::size_t node_total = selected_nodes.size();
    const std::size_t link_total = selected_links.size();

    // ---- 节点：直接删除（连带其连线，内部已按需压快照）----
    if (!selected_nodes.empty()) {
        snapshot("删除选中节点");
        const std::vector<std::string> targets = selected_nodes;
        for (const std::string& id : targets) {
            deleteNode(id);
        }
        selected_nodes.clear();
        selected_node.clear();
    }

    // ---- 连线：同样直接删模型 ----
    // 不能用 ed::DeleteLink()：DeleteItemsAction::Add() 在 GetCurrentAction() != nullptr 时
    // **静默返回 false**（右键菜单打开期间当前动作就是上下文菜单动作），因此删除不会入队。
    // 本工程以 Graph 为唯一真相：删掉边后画布不再提交该连线，编辑器会在本帧 End() 时
    // 因"未被提交（!m_IsLive）"自动回收内部连线对象（见 imgui_node_editor.cpp L1307）。
    if (!selected_links.empty()) {
        snapshot("删除选中连线");
        for (const std::string& edge_id : selected_links) {
            if (graph.removeEdge(edge_id)) {
                log::info("删除连线 " + edge_id);
            }
        }
        selected_links.clear();
    }

    logSummary("[删除] 已删除 节点 " + std::to_string(node_total) + " / 连线 " +
                std::to_string(link_total));
    setStatus("已删除 节点 " + std::to_string(node_total) + " 个、连线 " +
               std::to_string(link_total) + " 条");
    return true;
}

// -------------------------------------------------------------- 复制/粘贴 ----
// 【暂停接线】UI 入口（右键菜单 / 工具栏 / 编辑菜单）已下线，函数保留待 M2/M3 重做时接回。
// 暂停原因：粘贴会新建节点并请求视图跟随，而 ed::GetNodePosition() 对"编辑器还不认识"的手柄
// 返回 FLT_MAX（imgui_node_editor.cpp:1676），旧版 sync_positions() 会把该值当位置回写模型，
// 叠加 NavigateToContent 后视图落入 ~1e9 量级坐标 → 界面无响应（详见 CHANGELOG「复制/粘贴暂停」）。
// 现已加位置守卫（node_canvas.cpp：clamp_position / 未知手柄时反向推送 / 视图跟随前校验）。
void EditorState::copySelection()
{
    clipboard_nodes_.clear();
    clipboard_edges_.clear();

    if (selected_nodes.empty()) {
        setStatus("没有选中的节点");
        return;
    }

    for (const std::string& id : selected_nodes) {
        if (const engine::Node* node = graph.findNode(id)) {
            clipboard_nodes_.push_back(*node);
        }
    }
    // 只复制选区内部的连线
    for (const engine::Edge& edge : graph.edges) {
        const bool from_selected = std::find(selected_nodes.begin(), selected_nodes.end(),
                                             edge.from_node) != selected_nodes.end();
        const bool to_selected   = std::find(selected_nodes.begin(), selected_nodes.end(),
                                             edge.to_node) != selected_nodes.end();
        if (from_selected && to_selected) {
            clipboard_edges_.push_back(edge);
        }
    }

    log::info("复制 " + std::to_string(clipboard_nodes_.size()) + " 个节点到剪贴板");
    setStatus("已复制 " + std::to_string(clipboard_nodes_.size()) + " 个节点");
}

bool EditorState::pasteClipboard()
{
    if (clipboard_nodes_.empty()) {
        setStatus("剪贴板为空");
        return false;
    }

    snapshot("粘贴节点");

    constexpr float kOffset = 40.0f;
    std::vector<std::pair<std::string, std::string>> id_map; // 原 id → 新 id
    std::vector<std::string> created;

    for (const engine::Node& source : clipboard_nodes_) {
        std::string error;
        const std::string new_id = graph.addNode(source.type, source.x + kOffset,
                                                 source.y + kOffset, &error);
        if (new_id.empty()) {
            log::error("粘贴节点失败: " + error);
            continue;
        }
        if (engine::Node* node = graph.findNode(new_id)) {
            node->title  = source.title;
            node->params = source.params; // 一并复制参数值
        }
        id_map.emplace_back(source.id, new_id);
        created.push_back(new_id);
    }

    for (const engine::Edge& edge : clipboard_edges_) {
        std::string from_id;
        std::string to_id;
        for (const auto& entry : id_map) {
            if (entry.first == edge.from_node) {
                from_id = entry.second;
            }
            if (entry.first == edge.to_node) {
                to_id = entry.second;
            }
        }
        if (from_id.empty() || to_id.empty()) {
            continue;
        }
        std::string error;
        graph.addEdge(from_id, edge.from_port, to_id, edge.to_port, &error);
    }

    selected_nodes = created;
    selected_node  = created.empty() ? std::string() : created.front();
    request_navigate_to_content = true;

    logSummary("[粘贴] 已粘贴 " + std::to_string(created.size()) + " 个节点");
    setStatus("已粘贴 " + std::to_string(created.size()) + " 个节点");
    return !created.empty();
}

// ------------------------------------------------------------------ 运行 -----
// 说明（M2 P3-3）：运行前校验 → 执行器 → 每帧 tick。
//  * 值只在 Executor 内部；只有节点状态写回 Graph（画布就地染状态色）
//  * 控制台输出走 log::info，Console 面板读同一环形缓冲（设计 §12.4）
bool EditorState::startRun()
{
    aiwrite::nodes::registerAllExecutors(); // 幂等

    executor.setConsoleHandler([](const std::string& text) {
        log::info("[执行] " + text);  // Console 面板 + app.log
        log::workflow(text);          // 执行日志：workflow.log（设计 §11.1）
    });

    engine::ValidationMessages errors;
    engine::ValidationMessages warnings;
    if (!engine::validateBeforeRun(graph, &errors, &warnings)) {
        for (const std::string& text : errors) {
            log::error("[运行前校验] " + text);
            log::workflow("[运行前校验-失败] " + text);
        }
        setStatus("运行被拒绝：" + (errors.empty() ? std::string("未知原因") : errors.front()));
        return false;
    }
    for (const std::string& text : warnings) {
        log::warn("[运行前校验] " + text);
        log::workflow("[运行前校验-警告] " + text);
    }

    std::string error;
    if (!executor.start(graph, &error)) {
        log::error("启动运行失败：" + error);
        log::workflow("[运行启动失败] " + error);
        setStatus("启动运行失败：" + error);
        return false;
    }

    log::info("开始运行工作流：计划 " + std::to_string(executor.totalCount()) + " 个节点");
    log::workflow("===== 运行开始（计划 " + std::to_string(executor.totalCount()) + " 个节点）=====");
    setStatus("运行中…");
    return true;
}

// PD-06①（FEA-M4-11 · 通用重跑）：从 node_id 开始重跑（复用上游结果）
bool EditorState::startRerunAsync(const std::string& node_id, engine::RerunScope scope)
{
    aiwrite::nodes::registerAllExecutors(); // 幂等

    engine::ValidationMessages errors;
    engine::ValidationMessages warnings;
    if (!engine::validateBeforeRun(graph, &errors, &warnings)) {
        for (const std::string& text : errors) {
            log::error("[运行前校验] " + text);
            log::workflow("[运行前校验-失败] " + text);
        }
        setStatus("重跑被拒绝：" + (errors.empty() ? std::string("未知原因") : errors.front()));
        return false;
    }
    for (const std::string& text : warnings) {
        log::warn("[运行前校验] " + text);
        log::workflow("[运行前校验-警告] " + text);
    }

    engine::RerunSpec rerun;
    rerun.target_node = node_id;
    rerun.scope       = scope;

    std::string error;
    if (!executor.startAsyncFrom(&graph, rerun, &error)) {
        log::error("启动重跑失败：" + error);
        log::workflow("[重跑启动失败] " + error);
        setStatus("启动重跑失败：" + error);
        return false;
    }

    // 重跑**不清空** run_snapshot：上游结果保持可见；受影响节点由事件逐帧覆盖
    run_snapshot.running = true;
    run_snapshot.summary.clear();
    session_active_ = true;
    const char* scope_text = scope == engine::RerunScope::Downstream ? "及下游" : "";
    log::info("开始重跑节点 " + node_id + scope_text + "（后台线程）：计划 " +
              std::to_string(executor.totalCount()) + " 个节点");
    log::workflow("===== 重跑开始（节点 " + node_id + scope_text + " · 计划 " +
                  std::to_string(executor.totalCount()) + " 个节点，异步）=====");
    setStatus("重跑中：" + node_id + scope_text + "…");
    return true;
}

void EditorState::cancelRun()
{
    if (!executor.running()) {
        setStatus("当前没有正在运行的执行会话");
        return;
    }
    executor.requestStop(); // PB-01：置位 + 唤醒（HTTP 级真取消见 PB-02）
    log::warn("已请求停止运行（正在执行的节点会跑完当前步骤）");
    setStatus("正在停止…");
}

void EditorState::tickRun()
{
    // PB-01：异步会话 —— 只应用事件（写 Graph 状态 + 维护 run_snapshot）
    if (session_active_) {
        pumpRunEvents();
        return;
    }
    if (!executor.running()) {
        return; // 空闲：不推进
    }
    if (executor.tick(&graph)) {
        return; // 本帧推进了一个节点
    }
    // 会话结束（Finished / Cancelled / Failed）
    finishRunSession();
}

// ---------------------------------------------------------------- PB-01 -----
// 线程化运行（状态层）：校验 → startAsync；主线程每帧 pump_run_events() 应用事件
bool EditorState::startRunAsync()
{
    aiwrite::nodes::registerAllExecutors(); // 幂等

    engine::ValidationMessages errors;
    engine::ValidationMessages warnings;
    if (!engine::validateBeforeRun(graph, &errors, &warnings)) {
        for (const std::string& text : errors) {
            log::error("[运行前校验] " + text);
            log::workflow("[运行前校验-失败] " + text);
        }
        setStatus("运行被拒绝：" + (errors.empty() ? std::string("未知原因") : errors.front()));
        return false;
    }
    for (const std::string& text : warnings) {
        log::warn("[运行前校验] " + text);
        log::workflow("[运行前校验-警告] " + text);
    }

    std::string error;
    if (!executor.startAsync(&graph, &error)) {
        log::error("启动异步运行失败：" + error);
        log::workflow("[运行启动失败] " + error);
        setStatus("启动运行失败：" + error);
        return false;
    }

    run_snapshot      = engine::RunSnapshot{}; // 新会话：清空读模型
    run_snapshot.running = true;
    session_active_   = true;
    log::info("开始运行工作流（后台线程）：计划 " + std::to_string(executor.totalCount()) + " 个节点");
    log::workflow("===== 运行开始（计划 " + std::to_string(executor.totalCount()) +
                  " 个节点，异步）=====");
    setStatus("运行中…（界面保持响应；停止在节点边界生效）");
    return true;
}

engine::RunNodeView* EditorState::snapshotEntry(const std::string& node_id)
{
    for (engine::RunNodeView& view : run_snapshot.nodes) {
        if (view.node_id == node_id) {
            return &view;
        }
    }
    engine::RunNodeView view;
    view.node_id = node_id;
    if (const engine::Node* node = graph.findNode(node_id)) {
        view.type = node->type;
    }
    run_snapshot.nodes.push_back(std::move(view));
    return &run_snapshot.nodes.back();
}

void EditorState::pumpRunEvents()
{
    std::vector<engine::RunEvent> events;
    executor.pumpEvents(&events);

    for (const engine::RunEvent& event : events) {
        switch (event.kind) {
        case engine::RunEvent::Kind::NodeState: {
            if (engine::Node* node = graph.findNode(event.node_id)) {
                node->state = event.state; // 主线程写 Graph（唯一写者）
                if (!event.error.empty()) {
                    node->error_message = event.error;
                }
            }
            engine::RunNodeView* view = snapshotEntry(event.node_id);
            view->state               = event.state;
            if (!event.error.empty()) {
                view->error = event.error;
            }
            // PD-06①：重跑时被跳过的下游节点 ⇒ 其旧结果已失效，清掉避免「状态=跳过、正文=旧值」
            if (event.state == engine::NodeState::Skipped) {
                view->text.clear();
                view->images.clear();
                view->delta_bytes = 0;
            }
            break;
        }
        case engine::RunEvent::Kind::NodeOutput: {
            engine::RunNodeView* view = snapshotEntry(event.node_id);
            view->text                = event.text;
            view->state               = event.state;
            view->delta_bytes         = event.text.size();
            break;
        }
        case engine::RunEvent::Kind::Delta: {
            // PB-03/PB-08：增量按段追加到读模型（UI 逐字呈现的底座）
            engine::RunNodeView* view = snapshotEntry(event.node_id);
            view->text += event.text;
            view->state       = engine::NodeState::Running;
            view->delta_bytes = view->text.size();
            break;
        }
        case engine::RunEvent::Kind::Console: {
            log::info("[执行] " + event.text); // Console 面板 + app.log
            log::workflow(event.text);         // workflow.log（设计 §11.1）
            break;
        }
        case engine::RunEvent::Kind::Finished: {
            run_snapshot.summary     = event.summary;
            run_snapshot.final_state = event.final_state;
            run_snapshot.running     = false;
            log::info("[执行器] " + event.summary);
            log::workflow("[执行器] " + event.summary);
            finishRunSession();
            break;
        }
        }
    }
}

void EditorState::stopRunAsync()
{
    executor.stopAsync(); // join（幂等；退出 / 切换工作流前必须调用）
    session_active_ = false;
}

// 运行收尾：join → 权威快照落定 → 状态栏/日志 → 归档 → 错误条
void EditorState::finishRunSession()
{
    executor.stopAsync(); // 契约：先 join 再读权威数据

    // CN（展开执行）：权威快照以 executor.runInfos 为准（含内部节点），
    // 但 nodeOutputText 在 UI 图上解析不到内部节点端口 → 内部节点正文会变空。
    // 收尾前保留增量快照（运行中逐事件写入，内部节点正文完整），收尾后把空正文补回。
    const engine::RunSnapshot incremental = run_snapshot;
    run_snapshot                          = engine::makeSnapshot(graph, executor);
    if (incremental.nodes.size() > 1) { // 有增量事件才需要补（首次全量快照为空）
        for (auto& view : run_snapshot.nodes) {
            if (!view.text.empty() && !view.images.empty()) {
                continue;
            }
            if (const engine::RunNodeView* prev = incremental.find(view.node_id)) {
                if (view.text.empty()) {
                    view.text = prev->text;
                }
                if (view.images.empty()) {
                    view.images = prev->images;
                }
            }
        }
    }
    session_active_ = false;
    setStatus("运行结束：" + run_snapshot.summary);
    log::workflow("===== 运行结束：" + run_snapshot.summary + " =====");
    // PA-04：逐节点耗时明细（补 M4-13 登记项）
    if (!executor.runInfos().empty()) {
        log::workflow(run_detail_text(executor));
    }
    // PC-05：运行结果归档
    archiveRunOutputs();
    // PA-06：刷新错误条
    refreshLastError();

    // M_textio P5 + M_8nodes_extension L1-b（§4.8）：TextOutput 的「运行后自动导出」
    for (const engine::Node& node : graph.nodes) {
        if (node.type != "TextOutput") {
            continue;
        }
        const engine::Param* auto_param = node.findParam("auto_export");
        if (auto_param == nullptr || !auto_param->flag()) {
            continue;
        }
        // L1-b：批量导出 —— 导出当前工作流**所有有输出的节点**到同一目录
        //  * 文件名 = file_name 模板 + "-" + 节点 id（文档 §4.8「file_name 模板 + 节点 id」）
        const bool batch = [&node]() {
            const engine::Param* param = node.findParam("batch_export");
            return param != nullptr && param->flag();
        }();
        std::vector<const engine::RunNodeView*> targets;
        if (batch) {
            for (const auto& view : run_snapshot.nodes) {
                if (view.state == engine::NodeState::Done && !view.text.empty()) {
                    targets.push_back(&view);
                }
            }
        }
        else {
            const engine::RunNodeView* view = run_snapshot.find(node.id);
            if (view != nullptr && !view->text.empty()) {
                targets.push_back(view);
            }
        }
        std::string label = "输出";
        if (const engine::Param* label_param = node.findParam("label")) {
            const std::string text = label_param->text();
            if (!text.empty()) {
                label = text;
            }
        }
        std::string stem = workflowDisplayName();
        if (const engine::Param* name_param = node.findParam("file_name")) {
            const std::string text = name_param->text();
            if (!text.empty()) {
                stem = text;
            }
        }
        std::string dir;
        if (const engine::Param* dir_param = node.findParam("export_dir")) {
            dir = dir_param->text();
        }
        std::string header_template;
        if (const engine::Param* tpl_param = node.findParam("export_template")) {
            header_template = tpl_param->text();
        }

        for (const engine::RunNodeView* view : targets) {
            // 批量导出：非本节点的目标加节点 id 后缀（避免互相覆盖）
            const std::string this_stem =
                (batch && view->node_id != node.id) ? (stem + "-" + view->node_id) : stem;
            utils::ExportRequest request;
            request.text             = view->text;
            request.label            = label;
            request.include_meta     = true;
            request.overwrite        = false; // 自动导出不覆盖同名（自动 -1/-2）
            request.ext              = "md";
            request.dir              = dir;
            request.header_template  = header_template;
            request.file_stem        = this_stem;
            request.meta.workflow    = workflowDisplayName();
            request.meta.node_id     = view->node_id;
            request.meta.state       = engine::nodeStateName(view->state);
            request.meta.duration_ms = view->duration_ms;
            request.meta.stamp       = utils::documentTimeText();
            const utils::ExportResult result = utils::exportTextDocument(request);
            if (result.ok) {
                log::info("[自动导出] " + view->node_id + " → " + result.path);
                setStatus("已自动导出文档：" + result.path);
            }
            else {
                log::warn("[自动导出] 失败：" + result.error);
            }
        }
    }
}

// ---- PA-06 轻量错误条 -------------------------------------------------------

namespace {

// 本地时间 HH:MM:SS（错误条展示用）
std::string local_time_text()
{
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buffer[16] = {};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &tm);
    return buffer;
}

} // namespace

void EditorState::clearLastError()
{
    last_error = LastError{};
}

void EditorState::refreshLastError()
{
    // DEBT-L8（M6-04 步骤 5）：失败汇总 —— 收集**所有**失败节点（不只首个）
    const std::vector<engine::FailureSummary> failures = engine::collectFailures(graph);
    if (failures.empty()) {
        last_error = LastError{}; // 本次运行无失败 → 清空（成功运行不留旧错误）
        return;
    }

    last_error.active       = true;
    last_error.failed_count = static_cast<int>(failures.size());
    last_error.summary      = engine::summarizeFailures(graph);
    last_error.node_id      = failures.front().node_id; // 首个失败节点（定位 / relogin 反查）
    last_error.message =
        failures.front().message.empty() ? std::string("执行失败") : failures.front().message;
    last_error.at = local_time_text();
    if (failures.size() > 1) {
        // 多失败：每节点一行（tooltip 全文）
        last_error.details.clear();
        for (const engine::FailureSummary& f : failures) {
            if (!last_error.details.empty()) {
                last_error.details += "\n";
            }
            last_error.details += f.node_id + "：" + f.message;
        }
    }

    // ---- M_patchAB §2.2（`PB-07`）：失败原因**反查**会话失效 ⇒ 错误条给「重新登录」入口 ----
    //  * 判据 = 纯函数（401 / 40002 / 403 / 40003 /「会话已失效」/ invalid token …）；
    //    只有判据命中才给入口（不猜、不误报：普通超时 / 解析失败不显示按钮）
    const web::SessionFailure failure = web::classifySessionFailureText(last_error.message);
    if (failure.session_related) {
        const engine::Node* failed_node = graph.findNode(failures.front().node_id);
        if (failed_node != nullptr) {
            const engine::EffectiveProvider effective =
                engine::resolveEffectiveProvider(graph, *failed_node);
            last_error.relogin_available   = true;
            last_error.relogin_needed      = failure.needs_relogin;
            last_error.relogin_provider_id = effective.provider;
            last_error.relogin_text        = web::sessionReloginText(failure, effective.provider);
            log::warn("[会话诊断] " + last_error.node_id + "：" + last_error.relogin_text);
        }
    }
    log::info("[错误条] " + last_error.summary + (failures.size() > 1 ? "" : "：" +
                                                       last_error.message));
}

// ---- M_patchAB（PB2-07 / PB2-30②）：后台诊断结论（工作线程写、主线程每帧取一次）----
void EditorState::postBackgroundStatus(const std::string& text)
{
    std::lock_guard<std::mutex> lock(background_mutex_);
    background_status_  = text;
    background_pending_ = true;
}

bool EditorState::takeBackgroundStatus(std::string* out)
{
    std::lock_guard<std::mutex> lock(background_mutex_);
    if (!background_pending_) {
        return false;
    }
    if (out != nullptr) {
        *out = background_status_;
    }
    background_pending_ = false;
    return true;
}

std::string EditorState::workflowDisplayName() const
{
    if (current_workflow_path.empty()) {
        return "未命名";
    }
    const std::filesystem::path path(current_workflow_path);
    const std::string           stem = path.stem().string();
    return stem.empty() ? std::string("未命名") : stem;
}

void EditorState::archiveRunOutputs()
{
    // 节点条目与输出面板同源（runInfos + outputs）→ 归档内容与面板一致
    std::vector<utils::ArchiveNode> nodes;
    nodes.reserve(executor.runInfos().size());
    for (const engine::NodeRunInfo& info : executor.runInfos()) {
        utils::ArchiveNode item;
        item.node_id     = info.node_id;
        item.type        = info.type;
        item.state       = engine::nodeStateName(info.state);
        item.duration_ms = info.duration_ms;
        item.error       = info.error;
        item.text        = node_output_text(graph, executor, info.node_id);
        nodes.push_back(std::move(item));
    }

    const Config& config = appConfig();

    utils::ArchiveRequest request;
    request.workflow_name = workflowDisplayName();
    request.archive_dir   = config.output.archive_dir;
    request.keep_history  = config.output.keep_history;
    request.max_history   = config.output.max_history;
    request.ttl_days      = config.output.ttl_days;

    const utils::ArchiveResult result = utils::archiveRun(request, nodes, executor.summary());
    if (result.ok) {
        last_archive_dir = result.dir;
        log::info("[归档] 运行输出已写入 " + result.dir + "（" + std::to_string(result.files) +
                  " 个文件）");
        log::workflow("[归档] " + result.dir + "（" + std::to_string(result.files) + " 个文件）");

        // ---- M6 接线：`output.auto_open_on_complete`（完成后自动打开归档目录）----
        //  * 默认 false ⇒ 行为与改造前**逐字一致**（零副作用）
        //  * 打开失败只写日志（不打断运行结束流程，也不改归档结果）
        if (config.output.auto_open_on_complete) {
            if (utils::openFolder(result.dir)) {
                log::info("[归档] 已按配置自动打开归档目录（output.auto_open_on_complete = true）");
            }
            else {
                log::warn("[归档] 自动打开归档目录失败：" + result.dir);
            }
        }
    }
    else {
        log::warn("[归档] 失败：" + result.error);
        log::workflow("[归档-失败] " + result.error);
    }
}

std::string EditorState::runStatusText() const
{
    if (executor.state() == engine::ExecState::Idle) {
        return {};
    }
    if (!executor.running()) {
        return executor.summary();
    }
    std::ostringstream stream;
    stream << "运行中 | " << executor.finishedCount() << "/" << executor.totalCount() << " | "
           << std::fixed << std::setprecision(1) << executor.elapsedSeconds() << "s";
    const std::string current = executor.currentNode();
    if (!current.empty()) {
        stream << " | 当前 " << current;
        // PB-08：流式进度（读模型里的增量字节数；无增量时不显示）
        if (const engine::RunNodeView* live = run_snapshot.find(current)) {
            if (live->delta_bytes > 0) {
                stream << "（生成中… " << live->delta_bytes << " 字）";
            }
        }
    }
    return stream.str();
}

void EditorState::abortRunIfAny(const std::string& reason)
{
    if (!executor.running()) {
        return;
    }
    log::warn("运行中" + reason + " → 已终止当前运行");
    stopRunAsync(); // PB-01：先 join 工作线程（再动 Executor 状态，避免竞争）
    executor.cancel();
    executor.reset();
    for (engine::Node& node : graph.nodes) {
        node.state = engine::NodeState::Idle;
        node.error_message.clear();
    }
    setStatus("运行已终止（" + reason + "）");
}

// ----------------------------------------------------------- 自动保存（PC-01）----
// 时机与判据（落点 engine::autosave_*；本层只回答「现在写不写」）：
//  * 1 Hz 探测：engine::graph_content_signature(graph) 与「上次写入时的指纹」比较
//    —— 指纹不含 viewport ⇒ 画布平移 / 缩放不会触发写盘
//  * 首次发现变更时记时刻；**静默 ≥ autosave_idle_sec 秒**（默认 3）才写一次
//  * 无变更 ⇒ 一次盘都不写；写失败 ⇒ 退避 10 秒（避免每帧重试砸盘 + 刷日志）
void EditorState::pauseAutosave(bool paused)
{
    autosave_paused = paused;
    if (!paused) {
        autosave_last_probe_ = std::chrono::steady_clock::now(); // 解闸后重新计时
    }
}

void EditorState::markAutosaveBaseline()
{
    autosave_signature_   = engine::graphContentSignature(graph);
    autosave_change_seen_ = false;
    autosave_last_probe_  = std::chrono::steady_clock::now();
    autosave_retry_after_ = {};
}

void EditorState::tickAutosave()
{
    if (autosave_paused) {
        return;
    }
    const Config& config = appConfig();
    if (!config.ui.autosave_enabled) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < autosave_retry_after_) {
        return; // 写失败退避中
    }
    if (now - autosave_last_probe_ < std::chrono::milliseconds(1000)) {
        return;
    }
    autosave_last_probe_ = now;

    const std::string signature = engine::graphContentSignature(graph);
    if (signature != autosave_signature_) {
        autosave_change_seen_ = true;
        autosave_last_change_ = now; // 持续变更 ⇒ 时刻不断后移（= 「静默」判据）
        return;
    }
    if (!autosave_change_seen_) {
        return;
    }

    const int idle_seconds = std::max(1, config.ui.autosave_idle_sec);
    if (now - autosave_last_change_ < std::chrono::seconds(idle_seconds)) {
        return;
    }

    engine::AutosaveInfo info;
    std::string          error;
    if (engine::autosave_write(graph, current_workflow_path, config.ui.autosave_keep, &info,
                               &error)) {
        autosave_signature_   = signature;
        autosave_change_seen_ = false;
        log::info("[自动保存] 已写入快照（节点 " + std::to_string(graph.nodes.size()) + " / 连线 " +
                  std::to_string(graph.edges.size()) +
                  (current_workflow_path.empty() ? "，未命名工作流"
                                                 : "，关联 " + current_workflow_path) +
                  "）");
        setStatus("已自动保存：未保存的更改已暂存（崩溃/退出后可恢复）");
        return;
    }

    log::warn("[自动保存] 写入失败：" + error + "（10 秒后重试）");
    autosave_retry_after_ = now + std::chrono::seconds(10);
}

bool EditorState::restoreAutosave(std::string* error)
{
    engine::AutosaveInfo info = engine::autosaveProbe();
    engine::Graph        loaded;
    std::string          load_error;
    if (!engine::autosaveLoad(loaded, &load_error)) {
        if (error != nullptr) {
            *error = load_error;
        }
        log::error("[自动保存] 恢复失败：" + load_error);
        return false;
    }

    abortRunIfAny("恢复自动保存");
    graph = std::move(loaded);
    undo.clear();
    selected_nodes.clear();
    selected_links.clear();
    selected_node.clear();
    request_navigate_to_content = true;

    // PC-02：**不覆盖原文件** —— 恢复后当前路径留空 ⇒ 保存必然走「另存为」（用户显式确认目标）
    restored_from_autosave_path = info.workflow_path;
    current_workflow_path.clear();

    markAutosaveBaseline(); // 恢复内容 = 当前状态（否则会立刻再写一份同样的快照）
    log::info("[自动保存] 已恢复快照（节点 " + std::to_string(graph.nodes.size()) + " / 连线 " +
              std::to_string(graph.edges.size()) + "）" +
              (restored_from_autosave_path.empty() ? "（快照未关联工作流文件）"
                                                   : "（原文件 " + restored_from_autosave_path +
                                                         "，保存将另存为新文件）"));
    setStatus("已恢复未保存的更改（另存为新文件，不会覆盖原工作流）");
    return true;
}

void EditorState::flushAutosaveOnExit()
{
    if (autosave_paused) {
        return;
    }
    const Config& config = appConfig();
    if (!config.ui.autosave_enabled) {
        return;
    }
    if (engine::graphContentSignature(graph) == autosave_signature_) {
        return; // 没有未保存的更改 ⇒ 不写盘
    }

    engine::AutosaveInfo info;
    std::string          error;
    if (engine::autosave_write(graph, current_workflow_path, config.ui.autosave_keep, &info,
                               &error)) {
        log::info("[自动保存] 退出前已补写快照（下次启动会询问是否恢复未保存的更改）");
    }
    else {
        log::warn("[自动保存] 退出前补写失败：" + error);
    }
}

// ------------------------------------------------------------- 工作流文件 ----
bool EditorState::saveWorkflowTo(const std::string& path, std::string* error)
{
    if (path.empty()) {
        if (error != nullptr) {
            *error = "路径为空";
        }
        return false;
    }

    std::string save_error;
    if (!engine::saveWorkflow(graph, path, &save_error)) {
        if (error != nullptr) {
            *error = save_error;
        }
        log::error("保存工作流失败: " + save_error);
        log::workflow("[工作流-保存失败] " + path + "（" + save_error + "）");
        return false;
    }

    current_workflow_path = path;
    engine::pushRecentFile(path, graph.name);
    log::info("工作流已保存: " + path);
    log::workflow("[工作流-保存] " + path + "（节点 " + std::to_string(graph.nodes.size()) +
                  "，连线 " + std::to_string(graph.edges.size()) + "）");

    // PC-01：显式保存成功 ⇒ 「已无未保存的更改」：清掉快照 + 重设基线（不会紧接着又写一份）
    {
        std::string autosave_error;
        engine::autosaveClear(&autosave_error);
        if (!autosave_error.empty()) {
            log::warn("[自动保存] 清空旧快照失败：" + autosave_error);
        }
        markAutosaveBaseline();
    }

    setStatus("已保存：" + path);
    return true;
}

bool EditorState::openWorkflowFrom(const std::string& path, std::string* error)
{
    if (path.empty()) {
        if (error != nullptr) {
            *error = "路径为空";
        }
        return false;
    }

    engine::Graph loaded;
    std::string   load_error;
    if (!engine::loadWorkflow(path, loaded, &load_error)) {
        if (error != nullptr) {
            *error = load_error;
        }
        log::error("打开工作流失败: " + path + " → " + load_error);
        log::workflow("[工作流-加载失败] " + path + " → " + load_error);
        return false;
    }

    // 加载校验（三级校验的第二级）：不合法则拒绝并保持原图不变
    engine::ValidationMessages issues;
    if (!engine::validateWorkflow(loaded, &issues)) {
        const std::string first = issues.empty() ? std::string("未知问题") : issues.front();
        if (error != nullptr) {
            *error = "加载校验失败：" + first;
        }
        log::error("打开工作流被拒绝（加载校验）: " + path + " → " + first);
        log::workflow("[工作流-加载拒绝] " + path + " → " + first);
        return false;
    }

    abortRunIfAny("切换工作流");
    graph = std::move(loaded);
    undo.clear(); // 设计 §6.6：加载工作流清空撤销栈
    selected_nodes.clear();
    selected_links.clear();
    selected_node.clear();
    request_navigate_to_content = true; // 视图跟随新内容
    current_workflow_path       = path;
    engine::pushRecentFile(path, graph.name);

    // PC-01/02：换图**不删**快照（任何情况下都不丢稿；「快照 vs 工作流」的新旧由 mtime 判定），
    //  只把当前图设为「已保存」基线 ⇒ 不因换图本身触发一次写盘
    markAutosaveBaseline();

    log::info("工作流已打开: " + path);
    log::workflow("[工作流-加载] " + path + "（节点 " + std::to_string(graph.nodes.size()) +
                  "，连线 " + std::to_string(graph.edges.size()) + "）");
    logSummary("[工作流] 已加载");
    setStatus("已打开：" + path);
    return true;
}

std::vector<engine::RecentEntry> EditorState::recentWorkflows() const
{
    return engine::loadRecentFiles();
}

// ----------------------------------------------------- 文件对话框入口 -------
namespace {

const std::vector<utils::FileFilter>& workflow_filters()
{
    static const std::vector<utils::FileFilter> filters = {{"工作流 JSON", "json"},
                                                           {"所有文件", "*"}};
    return filters;
}

} // namespace

bool openWorkflowDialog()
{
    const std::string path = utils::open_file(workflow_filters(), paths::workflowsDir().string());
    if (path.empty()) {
        return false; // 用户取消
    }

    std::string error;
    if (!editor().openWorkflowFrom(path, &error)) {
        editor().setStatus("打开失败：" + error);
        return false;
    }
    return true;
}

bool saveWorkflowDialog()
{
    EditorState&      state = editor();
    const std::string default_name =
        (state.graph.name.empty() ? std::string("workflow") : state.graph.name) + ".json";
    const std::string path =
        utils::save_file(workflow_filters(), paths::workflowsDir().string(), default_name);
    if (path.empty()) {
        return false; // 用户取消
    }

    std::string error;
    if (!state.saveWorkflowTo(path, &error)) {
        state.setStatus("保存失败：" + error);
        return false;
    }
    return true;
}

} // namespace aiwrite::ui
