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

// ARCH-R2：构造时订阅 RunEvent（UI 改为订阅，不再主动 pumpEvents）
// ARCH-A1：AppContext（app_ctx_）在成员初始化列表中构造 —— 其 AutosaveService 在构造时
// 订阅内容类编辑事件（原本层循环，已迁至 autosave_service.cpp）；model_ 已就绪。
EditorState::EditorState() : app_ctx_(model_)
{
    run_sub_handle_ = run.subscribeRunEvents(
        [this](const engine::RunEvent& event) { onRunEvent(event); });

    // ARCH-E3：修改前钩子（M2-04 守卫）—— 运行中修改工作流 → 先终止本次运行。
    // Undo 订阅在 WorkflowModel 构造时已注册（回调 = 钩子 → undo.push，顺序与 E2 一致）
    model_.setBeforeEditHook([this](const std::string& label) {
        abortRunIfAny("修改工作流（" + label + "）");
    });

    // ARCH-E4：选中变化 → 发 SelectionChanged 编辑事件。SelectionState 无依赖
    //（§1.8.11 第 6 位），变化通知经回调注入；SelectionChanged 为通知形态
    //（无内容消费者，autosave 不订阅），与 E1 `sync_selection` 变化检测语义一致。
    selection_.set_changed_callback(
        [this]() { model_.event_bus().publish(EditEvent{EditEventKind::SelectionChanged}); });
}

EditorState::~EditorState()
{
    // ARCH-A1：autosave 订阅句柄随 app_ctx_（AutosaveService）析构统一注销
    run.unsubscribeRunEvents(run_sub_handle_);
}

// ARCH-R2：RunEvent 订阅回调 —— 只更新「变化通知」字段，供输出面板 / 状态栏读取
void EditorState::onRunEvent(const engine::RunEvent& event)
{
    switch (event.kind) {
    case engine::RunEvent::Kind::NodeOutput:
    case engine::RunEvent::Kind::Delta:
        follow_output_  = true;
        follow_node_id_ = event.node_id;
        break;
    case engine::RunEvent::Kind::Finished:
        follow_output_    = false; // 运行结束：停止跟随滚动
        run_finished_     = true;
        run_last_summary_ = event.summary;
        break;
    default:
        break;
    }
}

// ARCH-A1：autosave 事件回调已迁至 AutosaveService::on_edit_content（autosave_service.cpp，
// 订阅发生在 app_ctx_ 构造时；语义与拆出前逐字一致）。

void EditorState::setStatus(const std::string& text)
{
    status = text;
}

void EditorState::logSummary(const std::string& prefix) const
{
    log::info(prefix + "（节点 " + std::to_string(model_.graph.nodes.size()) + "，连线 " +
              std::to_string(model_.graph.edges.size()) + "）");
}

// ------------------------------------------------------------------ 撤销 -----
// ARCH-E3：核心操作迁 WorkflowModel（model().undoOnce 内部完成整图替换 + diff 事件）；
// 本层保留同签名薄封装（状态栏 / 选中清空），对外调用点零改动。
// ARCH-E2：修改前压快照由 Undo 订阅者完成（WorkflowModel 构造时订阅 before-edit 通道，
// 回调 = 修改前钩子 + undo.push）；snapshot() 只发布「修改前」通知（对外签名不变）。
void EditorState::snapshot(const std::string& label)
{
    model_.snapshot(label);
}

bool EditorState::undoOnce()
{
    std::string label;
    if (!model_.undoOnce(&label)) {
        setStatus("没有可撤销的操作");
        return false;
    }
    selection_.clear_nodes(); // ARCH-E4：选中独立对象（清 nodes + single，保留 links）
    logSummary("[撤销] " + label);
    setStatus("已撤销：" + label);
    return true;
}

bool EditorState::redoOnce()
{
    std::string label;
    if (!model_.redoOnce(&label)) {
        setStatus("没有可重做的操作");
        return false;
    }
    selection_.clear_nodes(); // ARCH-E4：选中独立对象（清 nodes + single，保留 links）
    logSummary("[重做] " + label);
    setStatus("已重做：" + label);
    return true;
}

// ----------------------------------------------------------------- 工作流 ----
void EditorState::clearWorkflow()
{
    model_.clear(); // 清图 + 清撤销栈 + 逐节点 diff 事件
    selection_.clear_nodes(); // ARCH-E4：选中独立对象（清 nodes + single，保留 links）
    app_ctx_.clipboard.clear(); // ARCH-A1：剪贴板归 Clipboard（app_ctx_）
    logSummary("[新建] 已清空画布");
    setStatus("画布已清空（撤销栈也已清空）");

    // PC-01/02：清空画布**不删**快照（保守：不替用户决定丢弃可恢复内容），只重设基线
    markAutosaveBaseline();
}

void EditorState::createSampleWorkflow()
{
    model_.graph.clear();
    model_.undo.clear();

    model_.graph.name        = "默认工作流";
    model_.graph.description = "文本输入 → 提示词模板 → 文本生成 → 文本输出（提供商配置接入文本生成的 provider 输入）";

    createNode("TextInput", -420.0f, -40.0f);
    createNode("PromptTemplate", -120.0f, -40.0f);
    createNode("LLMGenerate", 220.0f, -40.0f);
    createNode("TextOutput", 560.0f, -40.0f);
    // 提供商配置放在文本生成下方，其 provider 输出接到文本生成的 provider 输入
    createNode("ProviderConfig", 220.0f, 260.0f);

    if (model_.graph.nodes.size() == 5) {
        const std::string input_id    = model_.graph.nodes[0].id;
        const std::string prompt_id   = model_.graph.nodes[1].id;
        const std::string llm_id      = model_.graph.nodes[2].id;
        const std::string output_id   = model_.graph.nodes[3].id;
        const std::string provider_id = model_.graph.nodes[4].id;

        std::string error;
        const std::string e_input_prompt =
            model_.graph.addEdge(input_id, "text", prompt_id, "vars", &error);
        if (!e_input_prompt.empty()) {
            model_.publish(EditEvent::edge(EditEventKind::EdgeAdded, input_id, "text",
                                           prompt_id, "vars", e_input_prompt));
        }
        const std::string e_prompt_llm =
            model_.graph.addEdge(prompt_id, "text", llm_id, "prompt", &error);
        if (!e_prompt_llm.empty()) {
            model_.publish(EditEvent::edge(EditEventKind::EdgeAdded, prompt_id, "text",
                                           llm_id, "prompt", e_prompt_llm));
        }
        const std::string e_llm_output =
            model_.graph.addEdge(llm_id, "text", output_id, "text", &error);
        if (!e_llm_output.empty()) {
            model_.publish(EditEvent::edge(EditEventKind::EdgeAdded, llm_id, "text",
                                           output_id, "text", e_llm_output));
        }
        // 设计 §9.2：ProviderConfig 输出 provider 句柄 → 推理节点的 provider 输入
        const std::string e_provider_llm =
            model_.graph.addEdge(provider_id, "provider", llm_id, "provider", &error);
        if (!e_provider_llm.empty()) {
            model_.publish(EditEvent::edge(EditEventKind::EdgeAdded, provider_id, "provider",
                                           llm_id, "provider", e_provider_llm));
        }

        if (engine::Node* input = model_.graph.findNode(input_id)) {
            if (engine::Param* param = input->findParam("text")) {
                param->value = std::string("从前有座山，山里有座庙。");
            }
        }
    }

    // 默认启动工作流落盘：~/.brain-ai/workflows/default.json（设计 §11.3 Workflow 存储）
    // 注意：密钥类参数（is_secret，如 API Key）不会写入文件
    const std::filesystem::path default_file = paths::workflowsDir() / "default.json";
    std::string                 save_error;
    if (engine::saveWorkflow(model_.graph, default_file, &save_error)) {
        log::info("[默认工作流] 已保存到 " + default_file.string());
    }
    else {
        log::warn("[默认工作流] 保存失败: " + save_error);
    }

    model_.undo.clear(); // 默认工作流作为初始状态，不进入撤销历史
    selection_.clear_nodes(); // ARCH-E4：选中独立对象（清 nodes + single，保留 links）
    request_navigate_to_content = true;
    logSummary("[默认工作流] 已创建");
    setStatus("已创建默认工作流（含提供商配置 → 文本生成），并保存到 default.json");
}

// ------------------------------------------------------------------ 节点 -----
std::string EditorState::createNode(const std::string& type, float x, float y)
{
    std::string error;
    const std::string id = model_.createNode(type, x, y, &error); // addNode + NodeAdded 事件
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
    const engine::Node* node = model_.graph.findNode(node_id);
    if (node == nullptr) {
        return false;
    }
    const std::string title = node->title;

    // model_.deleteNode：removeNode + NodeRemoved 事件（连带其连线由 NodeRemoved 隐含）
    if (!model_.deleteNode(node_id)) {
        return false;
    }

    // ARCH-E4：从选中集合移除（若单选节点正是它 → single 一并清空）
    selection_.remove_node(node_id);
    log::info("删除节点 " + node_id + "（" + title + "）");
    return true;
}

bool EditorState::deleteSelected()
{
    if (selection_.empty()) {
        setStatus("没有选中的节点或连线");
        return false;
    }

    const std::size_t node_total = selection_.nodes().size();
    const std::size_t link_total = selection_.links().size();

    // ---- 节点：直接删除（连带其连线，内部已按需压快照）----
    if (selection_.has_nodes()) {
        snapshot("删除选中节点");
        const std::vector<std::string> targets = selection_.nodes();
        for (const std::string& id : targets) {
            deleteNode(id);
        }
        selection_.clear_nodes();
    }

    // ---- 连线：同样直接删模型 ----
    // 不能用 ed::DeleteLink()：DeleteItemsAction::Add() 在 GetCurrentAction() != nullptr 时
    // **静默返回 false**（右键菜单打开期间当前动作就是上下文菜单动作），因此删除不会入队。
    // 本工程以 Graph 为唯一真相：删掉边后画布不再提交该连线，编辑器会在本帧 End() 时
    // 因"未被提交（!m_IsLive）"自动回收内部连线对象（见 imgui_node_editor.cpp L1307）。
    if (selection_.has_links()) {
        snapshot("删除选中连线");
        for (const std::string& edge_id : selection_.links()) {
            const engine::Edge* edge = model_.graph.findEdge(edge_id);
            if (model_.graph.removeEdge(edge_id)) {
                // ARCH-E1：编辑事件通知（删除前取边数据作负载）
                if (edge != nullptr) {
                    model_.publish(EditEvent::edge(EditEventKind::EdgeRemoved, *edge));
                }
                log::info("删除连线 " + edge_id);
            }
        }
        selection_.clear_links();
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
    // ARCH-A1：剪贴板数据与注入归 Clipboard（app_ctx_）；本层只做选中读取与状态栏
    app_ctx_.clipboard.clear();

    if (selection_.nodes().empty()) {
        setStatus("没有选中的节点");
        return;
    }

    const std::size_t copied = app_ctx_.clipboard.copy(selection_.nodes(), model_.graph);
    log::info("复制 " + std::to_string(copied) + " 个节点到剪贴板");
    setStatus("已复制 " + std::to_string(copied) + " 个节点");
}

bool EditorState::pasteClipboard()
{
    // ARCH-A1：新建节点 / 连线注入归 Clipboard::paste；事件发布与选中更新留本层编排
    if (!app_ctx_.clipboard.has_content()) {
        setStatus("剪贴板为空");
        return false;
    }

    snapshot("粘贴节点");

    const Clipboard::PasteResult result = app_ctx_.clipboard.paste(model_.graph);

    // ARCH-E1：粘贴创建的每个节点发 NodeAdded / 每条连线发 EdgeAdded（与 createNode 同构）
    for (const std::string& id : result.node_ids) {
        model_.publish(EditEvent::node(EditEventKind::NodeAdded, id));
    }
    for (const engine::Edge& edge : result.added_edges) {
        model_.publish(EditEvent::edge(EditEventKind::EdgeAdded, edge));
    }

    // ARCH-E4：粘贴出的节点全部选中（single = 首个）
    selection_.set_nodes(result.node_ids);
    selection_.set_single(result.node_ids.empty() ? std::string() : result.node_ids.front());
    request_navigate_to_content = true;

    logSummary("[粘贴] 已粘贴 " + std::to_string(result.node_ids.size()) + " 个节点");
    setStatus("已粘贴 " + std::to_string(result.node_ids.size()) + " 个节点");
    return !result.node_ids.empty();
}

// ------------------------------------------------------------------ 运行 -----
// ARCH-R1：执行器 + 快照已拆到 RunSession（run_session.h）；本层只做编排与 UI 收尾。
//  * 值只在 Executor 内部；只有节点状态写回 Graph（画布就地染状态色）
//  * 控制台输出走 log::info，Console 面板读同一环形缓冲（设计 §12.4）

engine::Executor& EditorState::executor() { return run.executor(); }
const engine::Executor& EditorState::executor() const { return run.executor(); }

// 同步运行（--exec-selftest 用）：校验与执行归 RunSession；本层负责状态栏文案
bool EditorState::startRun()
{
    std::string error;
    if (!run.start(model_.graph, &error)) {
        setStatus(error);
        return false;
    }
    setStatus("运行中…");
    return true;
}

// PD-06①（FEA-M4-11 · 通用重跑）：从 node_id 开始重跑（复用上游结果）
bool EditorState::startRerunAsync(const std::string& node_id, engine::RerunScope scope)
{
    std::string error;
    if (!run.startRerunAsync(model_.graph, node_id, scope, &error)) {
        setStatus(error);
        return false;
    }
    // ARCH-R2：新会话开始 → 重置订阅通知（等待新事件置位）
    follow_output_ = false;
    run_finished_  = false;
    const char* scope_text = scope == engine::RerunScope::Downstream ? "及下游" : "";
    setStatus("重跑中：" + node_id + scope_text + "…");
    return true;
}

void EditorState::cancelRun()
{
    if (!run.running()) {
        setStatus("当前没有正在运行的执行会话");
        return;
    }
    run.cancel();
    setStatus("正在停止…");
}

void EditorState::tickRun()
{
    // 委托 RunSession：异步会话泵事件 / 同步会话推进一节点；
    //  返回 true = 会话结束（Finished 事件 / 同步跑完）→ 本层做 UI 收尾
    if (run.tick(model_.graph)) {
        finishRunSession();
    }
}

// ---------------------------------------------------------------- PB-01 -----
// 线程化运行：校验与执行归 RunSession；本层负责状态栏文案
bool EditorState::startRunAsync()
{
    std::string error;
    if (!run.startAsync(model_.graph, &error)) {
        setStatus(error);
        return false;
    }
    // ARCH-R2：新会话开始 → 重置订阅通知（等待新事件置位）
    follow_output_ = false;
    run_finished_  = false;
    setStatus("运行中…（界面保持响应；停止在节点边界生效）");
    return true;
}

void EditorState::stopRunAsync()
{
    run.stopAsync(); // join（幂等；退出 / 切换工作流前必须调用）
}

// 运行收尾：join 与权威快照落定归 RunSession::finish；本层只做 UI 收尾
void EditorState::finishRunSession()
{
    run.finish(model_.graph); // join → 权威快照 → 增量补回 → session_active_ = false

    const engine::RunSnapshot& snapshot = run.snapshot();
    setStatus("运行结束：" + snapshot.summary);
    log::workflow("===== 运行结束：" + snapshot.summary + " =====");
    // PA-04：逐节点耗时明细（补 M4-13 登记项）
    if (!run.executor().runInfos().empty()) {
        log::workflow(run_detail_text(run.executor()));
    }
    // PC-05：运行结果归档
    archiveRunOutputs();
    // PA-06：刷新错误条
    refreshLastError();

    // M_textio P5 + M_8nodes_extension L1-b（§4.8）：TextOutput 的「运行后自动导出」
    for (const engine::Node& node : model_.graph.nodes) {
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
            for (const auto& view : snapshot.nodes) {
                if (view.state == engine::NodeState::Done && !view.text.empty()) {
                    targets.push_back(&view);
                }
            }
        }
        else {
            const engine::RunNodeView* view = snapshot.find(node.id);
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
    const std::vector<engine::FailureSummary> failures = engine::collectFailures(model_.graph);
    if (failures.empty()) {
        last_error = LastError{}; // 本次运行无失败 → 清空（成功运行不留旧错误）
        return;
    }

    last_error.active       = true;
    last_error.failed_count = static_cast<int>(failures.size());
    last_error.summary      = engine::summarizeFailures(model_.graph);
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
        const engine::Node* failed_node = model_.graph.findNode(failures.front().node_id);
        if (failed_node != nullptr) {
            const engine::EffectiveProvider effective =
                engine::resolveEffectiveProvider(model_.graph, *failed_node);
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
// ARCH-A1：互斥通道已迁入 app_ctx().diagnostics（Diagnostics::post_status / take_status）
void EditorState::postBackgroundStatus(const std::string& text)
{
    app_ctx_.diagnostics.post_status(text);
}

bool EditorState::takeBackgroundStatus(std::string* out)
{
    return app_ctx_.diagnostics.take_status(out);
}

std::string EditorState::workflowDisplayName() const
{
    // ARCH-A1：实现归 WorkflowFile（app_ctx().workflow_file）
    return app_ctx_.workflow_file.workflow_display_name();
}

void EditorState::archiveRunOutputs()
{
    // ARCH-A1：实现归 WorkflowFile::archive_run_outputs（§1.8.9；含 last_archive_dir 写回）
    app_ctx_.workflow_file.archive_run_outputs(run.executor());
}

void EditorState::abortRunIfAny(const std::string& reason)
{
    if (!run.running()) {
        return;
    }
    run.abortIfAny(model_.graph, reason); // 终止 + 清节点状态（日志在内）
    setStatus("运行已终止（" + reason + "）");
}

// ----------------------------------------------------------- 自动保存（PC-01）----
// 时机与判据（落点 engine::autosave_*；本层只回答「现在写不写」）：
//  * ARCH-E2：变更感知 = 订阅内容类 EditEvent（AutosaveService::on_edit_content 标记
//    dirty），不再每帧算 content signature；**静默 ≥ autosave_idle_sec 秒**（默认 3）才写一次
//  * 无变更（无事件）⇒ 一次盘都不写；写失败 ⇒ 退避 10 秒（避免每帧重试砸盘 + 刷日志）
// ARCH-A1：实现归 app_ctx().autosave（AutosaveService）；本层只做状态栏编排
void EditorState::pauseAutosave(bool paused)
{
    app_ctx_.autosave.pause(paused);
}

void EditorState::markAutosaveBaseline()
{
    app_ctx_.autosave.mark_baseline();
}

void EditorState::tickAutosave()
{
    if (app_ctx_.autosave.tick(app_ctx_.workflow_file.current_workflow_path())) {
        setStatus("已自动保存：未保存的更改已暂存（崩溃/退出后可恢复）");
    }
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
    // ARCH-E3：整图替换（替换数据 + 清空撤销栈 + 逐节点 diff 事件）归 WorkflowModel::load
    model_.load(std::move(loaded));
    selection_.clear(); // ARCH-E4：选中独立对象（全清：nodes + links + single）
    request_navigate_to_content = true;

    // PC-02：**不覆盖原文件** —— 恢复后当前路径留空 ⇒ 保存必然走「另存为」（用户显式确认目标）
    // ARCH-A1：路径归口 app_ctx().workflow_file（current / restored_from）
    app_ctx_.workflow_file.set_restored_from_autosave_path(info.workflow_path);
    app_ctx_.workflow_file.set_current_workflow_path("");

    // ARCH-E2：整图替换通知已在 model_.load 内发出（在 baseline 之前发）
    app_ctx_.autosave.mark_baseline(); // 恢复内容 = 当前状态（否则会立刻再写一份同样的快照）
    const std::string& restored_path = app_ctx_.workflow_file.restored_from_autosave_path();
    log::info("[自动保存] 已恢复快照（节点 " + std::to_string(model_.graph.nodes.size()) +
              " / 连线 " + std::to_string(model_.graph.edges.size()) + "）" +
              (restored_path.empty() ? "（快照未关联工作流文件）"
                                     : "（原文件 " + restored_path + "，保存将另存为新文件）"));
    setStatus("已恢复未保存的更改（另存为新文件，不会覆盖原工作流）");
    return true;
}

void EditorState::flushAutosaveOnExit()
{
    app_ctx_.autosave.flush(app_ctx_.workflow_file.current_workflow_path());
}

// ------------------------------------------------------------- 工作流文件 ----
// ARCH-A1：保存 / 打开 / 最近归 `app_ctx().workflow_file`（WorkflowFile，§1.8.9）；
// 本层保留同签名薄封装（编排选中清空 / autosave 基线 / 状态栏），对外调用点零改动。
bool EditorState::saveWorkflowTo(const std::string& path, std::string* error)
{
    if (!app_ctx_.workflow_file.save_workflow_to(path, error)) {
        return false;
    }

    // PC-01：显式保存成功 ⇒ 「已无未保存的更改」：清掉快照 + 重设基线（不会紧接着又写一份）
    app_ctx_.autosave.clear_snapshot();
    app_ctx_.autosave.mark_baseline();

    setStatus("已保存：" + path);
    return true;
}

bool EditorState::openWorkflowFrom(const std::string& path, std::string* error)
{
    // 运行守卫注入给 WorkflowFile：加载校验通过后、整图替换**之前**终止运行
    if (!app_ctx_.workflow_file.open_workflow_from(
            path, error, [this]() { abortRunIfAny("切换工作流"); })) {
        return false;
    }

    selection_.clear(); // ARCH-E4：选中独立对象（全清：nodes + links + single）
    request_navigate_to_content = true; // 视图跟随新内容

    // ARCH-E2：整图重置通知已在 model_.load 内发出（在 baseline 之前发）
    // PC-01/02：换图**不删**快照（任何情况下都不丢稿；「快照 vs 工作流」的新旧由 mtime 判定），
    //  只把当前图设为「已保存」基线 ⇒ 不因换图本身触发一次写盘
    app_ctx_.autosave.mark_baseline();

    logSummary("[工作流] 已加载");
    setStatus("已打开：" + path);
    return true;
}

std::vector<engine::RecentEntry> EditorState::recentWorkflows() const
{
    // ARCH-A1：实现归 WorkflowFile::recent_workflows（≈ engine::loadRecentFiles）
    return app_ctx_.workflow_file.recent_workflows();
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
        (state.model().graph.name.empty() ? std::string("workflow") : state.model().graph.name) +
        ".json";
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
