#include "run_session.h"

#include "nodes.h"
#include "validate.h"
#include "log.h"

#include <iomanip>
#include <sstream>
#include <vector>

namespace aiwrite::ui {

// ------------------------------------------------------------------ 启动 -----
// 同步运行（GUI 不用；--exec-selftest 用）：校验 → executor.start。
//  * 控制台输出经 handler 直接写日志（异步版改走 RunEvent::Console 事件）
bool RunSession::start(engine::Graph& graph, std::string* error)
{
    aiwrite::nodes::registerAllExecutors(); // 幂等

    executor_.setConsoleHandler([](const std::string& text) {
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
        if (error != nullptr) {
            *error = "运行被拒绝：" + (errors.empty() ? std::string("未知原因") : errors.front());
        }
        return false;
    }
    for (const std::string& text : warnings) {
        log::warn("[运行前校验] " + text);
        log::workflow("[运行前校验-警告] " + text);
    }

    std::string start_error;
    if (!executor_.start(graph, &start_error)) {
        log::error("启动运行失败：" + start_error);
        log::workflow("[运行启动失败] " + start_error);
        if (error != nullptr) {
            *error = "启动运行失败：" + start_error;
        }
        return false;
    }

    log::info("开始运行工作流：计划 " + std::to_string(executor_.totalCount()) + " 个节点");
    log::workflow("===== 运行开始（计划 " + std::to_string(executor_.totalCount()) + " 个节点）=====");
    return true;
}

// PB-01：线程化运行入口（GUI 用）——校验 → startAsync → session_active
//  主循环每帧 tick() → pumpEvents() 应用事件（写 Graph 状态 + 维护 run_snapshot）
bool RunSession::startAsync(engine::Graph& graph, std::string* error)
{
    aiwrite::nodes::registerAllExecutors(); // 幂等

    engine::ValidationMessages errors;
    engine::ValidationMessages warnings;
    if (!engine::validateBeforeRun(graph, &errors, &warnings)) {
        for (const std::string& text : errors) {
            log::error("[运行前校验] " + text);
            log::workflow("[运行前校验-失败] " + text);
        }
        if (error != nullptr) {
            *error = "运行被拒绝：" + (errors.empty() ? std::string("未知原因") : errors.front());
        }
        return false;
    }
    for (const std::string& text : warnings) {
        log::warn("[运行前校验] " + text);
        log::workflow("[运行前校验-警告] " + text);
    }

    std::string start_error;
    if (!executor_.startAsync(&graph, &start_error)) {
        log::error("启动异步运行失败：" + start_error);
        log::workflow("[运行启动失败] " + start_error);
        if (error != nullptr) {
            *error = "启动运行失败：" + start_error;
        }
        return false;
    }

    run_snapshot_        = engine::RunSnapshot{}; // 新会话：清空读模型
    run_snapshot_.running = true;
    session_active_       = true;
    log::info("开始运行工作流（后台线程）：计划 " + std::to_string(executor_.totalCount()) + " 个节点");
    log::workflow("===== 运行开始（计划 " + std::to_string(executor_.totalCount()) +
                  " 个节点，异步）=====");
    return true;
}

// PD-06①（FEA-M4-11 · 通用重跑）：从 node_id 开始重跑（复用上游结果）
bool RunSession::startRerunAsync(engine::Graph& graph, const std::string& node_id,
                                 engine::RerunScope scope, std::string* error)
{
    aiwrite::nodes::registerAllExecutors(); // 幂等

    engine::ValidationMessages errors;
    engine::ValidationMessages warnings;
    if (!engine::validateBeforeRun(graph, &errors, &warnings)) {
        for (const std::string& text : errors) {
            log::error("[运行前校验] " + text);
            log::workflow("[运行前校验-失败] " + text);
        }
        if (error != nullptr) {
            *error = "重跑被拒绝：" + (errors.empty() ? std::string("未知原因") : errors.front());
        }
        return false;
    }
    for (const std::string& text : warnings) {
        log::warn("[运行前校验] " + text);
        log::workflow("[运行前校验-警告] " + text);
    }

    engine::RerunSpec rerun;
    rerun.target_node = node_id;
    rerun.scope       = scope;

    std::string start_error;
    if (!executor_.startAsyncFrom(&graph, rerun, &start_error)) {
        log::error("启动重跑失败：" + start_error);
        log::workflow("[重跑启动失败] " + start_error);
        if (error != nullptr) {
            *error = "启动重跑失败：" + start_error;
        }
        return false;
    }

    // 重跑**不清空** run_snapshot：上游结果保持可见；受影响节点由事件逐帧覆盖
    run_snapshot_.running = true;
    run_snapshot_.summary.clear();
    session_active_ = true;
    const char* scope_text = scope == engine::RerunScope::Downstream ? "及下游" : "";
    log::info("开始重跑节点 " + node_id + scope_text + "（后台线程）：计划 " +
              std::to_string(executor_.totalCount()) + " 个节点");
    log::workflow("===== 重跑开始（节点 " + node_id + scope_text + " · 计划 " +
                  std::to_string(executor_.totalCount()) + " 个节点，异步）=====");
    return true;
}

// ------------------------------------------------------------ 事件泵与推进 ------
engine::RunNodeView* RunSession::snapshotEntry(const engine::Graph& graph,
                                               const std::string& node_id)
{
    for (engine::RunNodeView& view : run_snapshot_.nodes) {
        if (view.node_id == node_id) {
            return &view;
        }
    }
    engine::RunNodeView view;
    view.node_id = node_id;
    if (const engine::Node* node = graph.findNode(node_id)) {
        view.type = node->type;
    }
    run_snapshot_.nodes.push_back(std::move(view));
    return &run_snapshot_.nodes.back();
}

// 异步会话：只应用事件（写 Graph 状态 + 维护 run_snapshot）
//  返回 true = 本次泵到了 Finished 事件（调用方应执行收尾）
bool RunSession::pumpEvents(engine::Graph& graph)
{
    std::vector<engine::RunEvent> events;
    executor_.pumpEvents(&events);

    bool need_finish = false;
    for (const engine::RunEvent& event : events) {
        switch (event.kind) {
        case engine::RunEvent::Kind::NodeState: {
            if (engine::Node* node = graph.findNode(event.node_id)) {
                node->state = event.state; // 主线程写 Graph（唯一写者）
                if (!event.error.empty()) {
                    node->error_message = event.error;
                }
            }
            engine::RunNodeView* view = snapshotEntry(graph, event.node_id);
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
            engine::RunNodeView* view = snapshotEntry(graph, event.node_id);
            view->text                = event.text;
            view->state               = event.state;
            view->delta_bytes         = event.text.size();
            break;
        }
        case engine::RunEvent::Kind::Delta: {
            // PB-03/PB-08：增量按段追加到读模型（UI 逐字呈现的底座）
            engine::RunNodeView* view = snapshotEntry(graph, event.node_id);
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
            run_snapshot_.summary     = event.summary;
            run_snapshot_.final_state = event.final_state;
            run_snapshot_.running     = false;
            log::info("[执行器] " + event.summary);
            log::workflow("[执行器] " + event.summary);
            need_finish = true;
            break;
        }
        }
        // ARCH-R2：每个事件应用后分发给订阅者（主线程 FIFO；「变化通知」）
        dispatchRunEvent(event);
    }
    return need_finish;
}

// ARCH-R2：把单个事件分发给全部订阅者（主线程 FIFO）
//  * 拷贝回调列表再遍历，避免回调里 subscribe/unsubscribe 改容器导致迭代器失效
void RunSession::dispatchRunEvent(const engine::RunEvent& event)
{
    if (run_subscribers_.empty()) {
        return;
    }
    std::vector<RunEventCallback> callbacks;
    callbacks.reserve(run_subscribers_.size());
    for (const auto& [handle, callback] : run_subscribers_) {
        callbacks.push_back(callback);
    }
    for (const RunEventCallback& callback : callbacks) {
        callback(event);
    }
}

// ------------------------------------------------------------- 订阅（ARCH-R2）----
std::size_t RunSession::subscribeRunEvents(RunEventCallback callback)
{
    const std::size_t handle = next_sub_id_++;
    run_subscribers_.emplace_back(handle, std::move(callback));
    return handle;
}

void RunSession::unsubscribeRunEvents(std::size_t handle)
{
    for (auto it = run_subscribers_.begin(); it != run_subscribers_.end(); ++it) {
        if (it->first == handle) {
            run_subscribers_.erase(it);
            return;
        }
    }
}

// 主循环每帧调用一次：异步会话 → 泵事件；同步会话 → 推进一个节点
bool RunSession::tick(engine::Graph& graph)
{
    // PB-01：异步会话 —— 只应用事件（写 Graph 状态 + 维护 run_snapshot）
    if (session_active_) {
        return pumpEvents(graph);
    }
    if (!executor_.running()) {
        return false; // 空闲：不推进
    }
    if (executor_.tick(&graph)) {
        return false; // 本帧推进了一个节点
    }
    return true;      // 会话结束（Finished / Cancelled / Failed）→ 调用方执行收尾
}

// 运行收尾：join → 权威快照落定 → 增量补回 → session_active_ = false
void RunSession::finish(engine::Graph& graph)
{
    executor_.stopAsync(); // 契约：先 join 再读权威数据

    // CN（展开执行）：权威快照以 executor.runInfos 为准（含内部节点），
    // 但 nodeOutputText 在 UI 图上解析不到内部节点端口 → 内部节点正文会变空。
    // 收尾前保留增量快照（运行中逐事件写入，内部节点正文完整），收尾后把空正文补回。
    const engine::RunSnapshot incremental = run_snapshot_;
    run_snapshot_                          = engine::makeSnapshot(graph, executor_);
    if (incremental.nodes.size() > 1) { // 有增量事件才需要补（首次全量快照为空）
        for (auto& view : run_snapshot_.nodes) {
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
}

// ----------------------------------------------------------------- 停止 ------
void RunSession::stopAsync()
{
    executor_.stopAsync(); // join（幂等；退出 / 切换工作流前必须调用）
    session_active_ = false;
}

void RunSession::cancel()
{
    if (!executor_.running()) {
        return;
    }
    executor_.requestStop(); // PB-01：置位 + 唤醒（HTTP 级真取消见 PB-02）
    log::warn("已请求停止运行（正在执行的节点会跑完当前步骤）");
}

// 运行中修改 / 切换工作流时终止运行（M2-04 守卫）
void RunSession::abortIfAny(engine::Graph& graph, const std::string& reason)
{
    if (!executor_.running()) {
        return;
    }
    log::warn("运行中" + reason + " → 已终止当前运行");
    stopAsync();            // PB-01：先 join 工作线程（再动 Executor 状态，避免竞争）
    executor_.cancel();
    executor_.reset();
    for (engine::Node& node : graph.nodes) {
        node.state = engine::NodeState::Idle;
        node.error_message.clear();
    }
}

// ------------------------------------------------------------- 状态栏文本 ------
std::string RunSession::statusText() const
{
    if (executor_.state() == engine::ExecState::Idle) {
        return {};
    }
    if (!executor_.running()) {
        return executor_.summary();
    }
    std::ostringstream stream;
    stream << "运行中 | " << executor_.finishedCount() << "/" << executor_.totalCount() << " | "
           << std::fixed << std::setprecision(1) << executor_.elapsedSeconds() << "s";
    const std::string current = executor_.currentNode();
    if (!current.empty()) {
        stream << " | 当前 " << current;
        // PB-08：流式进度（读模型里的增量字节数；无增量时不显示）
        if (const engine::RunNodeView* live = run_snapshot_.find(current)) {
            if (live->delta_bytes > 0) {
                stream << "（生成中… " << live->delta_bytes << " 字）";
            }
        }
    }
    return stream.str();
}

} // namespace aiwrite::ui

