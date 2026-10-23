#include "executor.h"

#include "composite.h"
#include "validate.h"

#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace aiwrite::engine {


// ---------------------------------------------------------------- 运行态值 ----
void NodeOutputs::set(const std::string& node_id, const std::string& port_id, nlohmann::json value)
{
    values_[node_id][port_id] = std::move(value);
}

const nlohmann::json* NodeOutputs::find(const std::string& node_id,
                                       const std::string& port_id) const
{
    const auto node_it = values_.find(node_id);
    if (node_it == values_.end()) {
        return nullptr;
    }
    const auto port_it = node_it->second.find(port_id);
    return port_it == node_it->second.end() ? nullptr : &port_it->second;
}

void NodeOutputs::clear()
{
    values_.clear();
}

void NodeOutputs::erase(const std::string& node_id)
{
    values_.erase(node_id);
}

std::size_t NodeOutputs::size() const
{
    std::size_t total = 0;
    for (const auto& node : values_) {
        total += node.second.size();
    }
    return total;
}

// ------------------------------------------------------------ 会话状态名 ------
const char* execStateName(ExecState state)
{
    switch (state) {
    case ExecState::Idle:      return "idle";
    case ExecState::Running:   return "running";
    case ExecState::Finished:  return "finished";
    case ExecState::Cancelled: return "cancelled";
    case ExecState::Failed:    return "failed";
    }
    return "idle";
}

// -------------------------------------------------------- 执行函数注册表 -----
NodeExecutorRegistry& NodeExecutorRegistry::instance()
{
    static NodeExecutorRegistry registry;
    return registry;
}

void NodeExecutorRegistry::registerExecutor(const std::string& type, NodeExecutor executor)
{
    executors_[type] = std::move(executor);
}

const NodeExecutor* NodeExecutorRegistry::find(const std::string& type) const
{
    const auto it = executors_.find(type);
    return it == executors_.end() ? nullptr : &it->second;
}

std::size_t NodeExecutorRegistry::size() const
{
    return executors_.size();
}

void NodeExecutorRegistry::clear()
{
    executors_.clear();
}

// ------------------------------------------------------------- 参数 / 输入 ----
nlohmann::json Executor::paramObject(const Node& node)
{
    nlohmann::json params = nlohmann::json::object();
    for (const Param& param : node.params) {
        params[param.id] = param.value;
    }
    return params;
}

nlohmann::json Executor::collectInputs(const Graph& graph, const Node& node) const
{
    nlohmann::json inputs = nlohmann::json::object();
    for (const Port& port : node.inputs) {
        if (port.is_variadic) {
            // 变长输入：按边插入序收集全部上游值
            nlohmann::json list = nlohmann::json::array();
            for (const Edge& edge : graph.edges) {
                if (edge.to_node != node.id || edge.to_port != port.id) {
                    continue;
                }
                if (const nlohmann::json* value = outputs_.find(edge.from_node, edge.from_port)) {
                    list.push_back(*value);
                }
            }
            if (!list.empty()) {
                inputs[port.id] = std::move(list);
            }
            continue;
        }

        const Edge* edge = graph.findEdgeIntoInput(node.id, port.id);
        if (edge == nullptr) {
            continue; // 未连接（必填性由运行前校验保证）
        }
        const nlohmann::json* value = outputs_.find(edge->from_node, edge->from_port);
        if (value == nullptr) {
            if (port.is_optional) {
                continue;
            }
            throw NodeError("输入「" + port.display_name + "」的上游（" + edge->from_node + "." +
                            edge->from_port + "）没有可用的输出值");
        }
        inputs[port.id] = *value;
    }
    return inputs;
}

// ------------------------------------------------------ 下游跳过 / 状态通知 ---
void Executor::markDownstreamSkipped(Graph& graph, const std::string& node_id)
{
    std::vector<std::string> pending{node_id};
    while (!pending.empty()) {
        const std::string current = pending.back();
        pending.pop_back();
        for (const Edge& edge : graph.edges) {
            if (edge.from_node != current) {
                continue;
            }
            Node* target = graph.findNode(edge.to_node);
            if (target == nullptr || target->state != NodeState::Waiting) {
                continue; // 已执行 / 已跳过的不重复处理
            }
            target->state         = NodeState::Skipped;
            target->error_message = "上游 " + current + " 执行失败，已跳过";
            notifyState(edge.to_node, NodeState::Skipped);
            ctx_.console("[跳过] " + edge.to_node + "（上游 " + current + " 失败）");
            pending.push_back(edge.to_node);
        }
    }
}

void Executor::notifyState(const std::string& node_id, NodeState state) const
{
    // PB-01：异步模式下由 startAsync 拦截的回调统一推事件（此处不重复推送，避免状态事件翻倍）
    if (ctx_.on_node_state) {
        ctx_.on_node_state(node_id, state);
    }
}

// PA-01：记录一次节点执行结果（只读暴露；不进 Graph / 快照）
void Executor::recordRun(const Node& node, NodeState state, double duration_ms, std::string error)
{
    NodeRunInfo info;
    info.node_id     = node.id;
    info.type        = node.type;
    info.state       = state;
    info.duration_ms = duration_ms < 0.0 ? 0.0 : duration_ms;
    info.error       = std::move(error);
    run_infos_.push_back(std::move(info));

    // PB-01：异步模式下把"该节点输出全文"作为事件推给主线程（UI 读模型来源）
    if (async_mode_.load()) {
        RunEvent event;
        event.kind    = RunEvent::Kind::NodeOutput;
        event.node_id = node.id;
        event.state   = state;
        event.text    = nodeOutputText(worker_graph_, outputs_, node.id);
        pushEvent(std::move(event));
    }
}

// ---------------------------------------------------------------- Executor ----
Executor::Executor()
{
    ctx_.outputs      = &outputs_;
    ctx_.cancelled    = &cancelled_;
    ctx_.cancel_token = cancel_token_; // M_patchAB（PB-02）：节点把令牌交给 HTTP 层
    // PB-03：节点实现按段吐出的增量 → 统一在异步模式下转成事件
    ctx_.on_delta  = [this](const std::string& text) { handleDelta(text); };
}

void Executor::handleDelta(const std::string& text)
{
    if (text.empty() || !async_mode_.load()) {
        return; // 同步模式：结果由节点返回值给出（自检路径不变）
    }
    RunEvent event;
    event.kind    = RunEvent::Kind::Delta;
    event.node_id = current_node_;
    event.text    = text;
    pushEvent(std::move(event));
}

void Executor::setConsoleHandler(std::function<void(const std::string&)> handler)
{
    ctx_.on_console = std::move(handler);
}

void Executor::setStateHandler(std::function<void(const std::string&, NodeState)> handler)
{
    ctx_.on_node_state = std::move(handler);
}

void Executor::reset()
{
    resetSession();  // PD-06①：会话级复位（保留 outputs_）；全量运行再清空运行态值
    outputs_.clear();
}

// PD-06①：会话级复位 —— 与 reset() 的区别是**保留 outputs_**（供「重跑该节点」复用上游结果）
void Executor::resetSession()
{
    plan_.clear();
    cursor_ = 0;
    finished_ = 0;
    failed_ = 0;
    skipped_ = 0;
    state_ = ExecState::Idle;
    current_node_.clear();
    cancelled_ = false;
    // PB-02：每次运行换一枚**新令牌**（上一轮的取消不影响下一轮）
    cancel_token_     = std::make_shared<ai::CancelToken>();
    ctx_.cancel_token = cancel_token_;
    run_infos_.clear();
    start_time_ = {};
    end_time_ = {};
}

void Executor::cancel()
{
    if (state_ == ExecState::Running) {
        cancelled_ = true;
        if (cancel_token_ != nullptr) {
            cancel_token_->request_cancel(); // PB-02：唤醒阻塞中的 HTTP 调用（主动关 socket）
        }
    }
}

bool Executor::start(Graph& graph, std::string* error)
{
    reset();

    // CN（Composition_node）展开执行：工作图就地展开上级节点
    //  * 展开后图中不再有 Composite 节点 ⇒ 内部节点成为父图一部分，按同一拓扑序推进
    //  * 运行前校验/执行实现检查因此在**展开后的图**上执行（内部必填端口未接会被正确拦截）
    //  * 展开失败（如规格缺失）直接判定运行失败
    if (!expand_composites(graph, nullptr, error)) {
        state_ = ExecState::Failed;
        return false;
    }

    ValidationMessages errors;
    ValidationMessages warnings;
    if (!validateBeforeRun(graph, &errors, &warnings)) {
        state_ = ExecState::Failed;
        if (error != nullptr) {
            *error = errors.empty() ? std::string("运行前校验失败") : errors.front();
        }
        return false;
    }

    // 每个节点都必须有执行实现（否则跑到一半才发现，体验更差）
    for (const Node& node : graph.nodes) {
        if (NodeExecutorRegistry::instance().find(node.type) == nullptr) {
            state_ = ExecState::Failed;
            if (error != nullptr) {
                *error = "[" + node.id + "] 节点类型缺少执行实现: " + node.type +
                         "（请先调用 nodes::registerAllExecutors()）";
            }
            return false;
        }
    }

    if (!graph.topologicalOrder(&plan_, error)) {
        state_ = ExecState::Failed;
        return false;
    }

    // 节点状态重置为 Waiting（设计 §10.2 状态机起点）
    for (Node& node : graph.nodes) {
        node.state = NodeState::Waiting;
        node.error_message.clear();
    }

    state_      = ExecState::Running;
    start_time_ = std::chrono::steady_clock::now();
    // M_patchC（PW-01 · 归属）：运行会话 id —— 同一次运行内稳定，供 web::WebOwner 标注
    ctx_.run_id = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(start_time_.time_since_epoch()).count());
    ctx_.console("[执行器] 开始执行：共 " + std::to_string(plan_.size()) + " 个节点");
    for (const std::string& text : warnings) {
        ctx_.console("[警告] " + text);
    }
    return true;
}

// ------------------------------------------------------ PD-06① 通用重跑 ------
// 收集 start 节点的全部下游（含目标本身；沿 edges 的 from→to 单向可达）
void Executor::collectDownstream(const Graph& graph, const std::string& start,
                                 std::unordered_set<std::string>* out) const
{
    if (out == nullptr || graph.findNode(start) == nullptr) {
        return;
    }
    std::vector<std::string> pending{start};
    while (!pending.empty()) {
        const std::string current = pending.back();
        pending.pop_back();
        for (const Edge& edge : graph.edges) {
            if (edge.from_node != current) {
                continue;
            }
            if (out->insert(edge.to_node).second) {
                pending.push_back(edge.to_node);
            }
        }
    }
}

// 「重跑该节点」/「重跑该节点及下游」（FEA-M4-11 · 复用上游结果）
//  * 计划 = 全图拓扑序 ∩ 受影响集合（保持执行顺序）；上游不在计划里 ⇒ 运行态值原样复用
//  * 重跑前清除受影响节点的运行态结果与状态（运行态值，不进撤销快照 / 工作流 JSON）
bool Executor::startFrom(Graph& graph, const RerunSpec& rerun, std::string* error)
{
    resetSession(); // 保留 outputs_（复用上游）

    const Node* target_node = graph.findNode(rerun.target_node);
    if (target_node == nullptr) {
        state_ = ExecState::Failed;
        if (error != nullptr) {
            *error = "重跑目标节点不存在: " + rerun.target_node;
        }
        return false;
    }
    // CN：重跑目标是上级节点时，展开后需要映射到其内部节点
    const bool target_is_composite = !target_node->composite_id.empty();

    // CN 展开执行：与 start() 一致，先就地展开上级节点
    std::unordered_map<std::string, std::vector<std::string>> replaced;
    if (!expand_composites(graph, &replaced, error)) {
        state_ = ExecState::Failed;
        return false;
    }

    // 受影响集合：NodeOnly = {target}；Downstream = {target} ∪ 其全部下游
    //  * 上级节点：展开后以内部节点为受影响集（NodeOnly = 全部内部节点）
    std::unordered_set<std::string> affected;
    if (target_is_composite) {
        const auto it = replaced.find(rerun.target_node);
        if (it == replaced.end() || it->second.empty()) {
            state_ = ExecState::Failed;
            if (error != nullptr) {
                *error = "重跑目标上级节点未展开: " + rerun.target_node;
            }
            return false;
        }
        for (const std::string& id : it->second) {
            affected.insert(id);
        }
        if (rerun.scope == RerunScope::Downstream) {
            for (const std::string& id : it->second) {
                collectDownstream(graph, id, &affected);
            }
        }
    }
    else {
        affected.insert(rerun.target_node);
        if (rerun.scope == RerunScope::Downstream) {
            collectDownstream(graph, rerun.target_node, &affected);
        }
    }

    // 与全量运行一致：整图运行前校验 + 执行实现齐全检查
    ValidationMessages errors;
    ValidationMessages warnings;
    if (!validateBeforeRun(graph, &errors, &warnings)) {
        state_ = ExecState::Failed;
        if (error != nullptr) {
            *error = errors.empty() ? std::string("运行前校验失败") : errors.front();
        }
        return false;
    }
    for (const Node& node : graph.nodes) {
        if (NodeExecutorRegistry::instance().find(node.type) == nullptr) {
            state_ = ExecState::Failed;
            if (error != nullptr) {
                *error = "[" + node.id + "] 节点类型缺少执行实现: " + node.type +
                         "（请先调用 nodes::registerAllExecutors()）";
            }
            return false;
        }
    }

    // 计划 = 全图拓扑序 ∩ 受影响集合（保持执行顺序）
    std::vector<std::string> full;
    if (!graph.topologicalOrder(&full, error)) {
        state_ = ExecState::Failed;
        return false;
    }
    plan_.clear();
    for (const std::string& id : full) {
        if (affected.count(id) != 0) {
            plan_.push_back(id);
        }
    }

    // 受影响节点状态复位为 Waiting + 清错误（画布就地刷新）；非受影响节点保留原状态
    for (Node& node : graph.nodes) {
        if (affected.count(node.id) != 0) {
            node.state = NodeState::Waiting;
            node.error_message.clear();
        }
    }

    // 清除受影响节点的运行态值（复用上游；受影响节点不得残留旧值）
    for (const std::string& id : affected) {
        outputs_.erase(id);
    }

    state_      = ExecState::Running;
    start_time_ = std::chrono::steady_clock::now();
    // M_patchC（PW-01 · 归属）：运行会话 id —— 同一次运行内稳定，供 web::WebOwner 标注
    ctx_.run_id = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(start_time_.time_since_epoch()).count());
    const char* scope_text =
        rerun.scope == RerunScope::Downstream ? "该节点及下游" : "仅该节点";
    ctx_.console("[执行器] 重跑「" + rerun.target_node + "」（" + scope_text + "，共 " +
                 std::to_string(plan_.size()) + " 个节点；上游结果复用）");
    for (const std::string& text : warnings) {
        ctx_.console("[警告] " + text);
    }
    return true;
}

double Executor::elapsedSeconds() const
{
    if (start_time_ == std::chrono::steady_clock::time_point{}) {
        return 0.0;
    }
    const auto end = (state_ == ExecState::Running) ? std::chrono::steady_clock::now() : end_time_;
    return std::chrono::duration<double>(end - start_time_).count();
}

std::string Executor::summary() const
{
    std::ostringstream stream;
    stream << "完成 " << finished_ << "/" << plan_.size() << "，失败 " << failed_ << "，跳过 "
           << skipped_ << "，耗时 " << std::fixed << std::setprecision(2) << elapsedSeconds() << "s"
           << "（" << execStateName(state_) << "）";
    return stream.str();
}

bool Executor::tick(Graph* graph)
{
    if (graph == nullptr || state_ != ExecState::Running) {
        return false;
    }
    if (cancelled_.load()) {
        state_    = ExecState::Cancelled;
        end_time_ = std::chrono::steady_clock::now();
        current_node_.clear();
        // PB-02：取消记录（`AB-01`：日志里必须能看出「谁在什么时候被停了」）
        ctx_.console("[执行器] 已取消（用户点了「停止」）：" + summary());
        return false;
    }
    if (cursor_ >= plan_.size()) {
        state_    = ExecState::Finished;
        end_time_ = std::chrono::steady_clock::now();
        current_node_.clear();
        ctx_.console("[执行器] 执行结束：" + summary());
        return false;
    }

    const std::string node_id = plan_[cursor_++];
    Node*             node    = graph->findNode(node_id);
    if (node == nullptr) {
        return true; // 计划与图不一致（理论不可达）
    }
    if (node->state == NodeState::Skipped) {
        ++skipped_; // 上游失败导致本节点跳过
        recordRun(*node, NodeState::Skipped, 0.0, node->error_message);
        return true;
    }
    executeNode(*graph, node_id);
    return true;
}

// ---------------------------------------------------------------- PB-01 ----
// 线程化运行：工作线程在 Graph 副本上推进 tick，只向事件队列推送；
// 主线程每帧 pumpEvents() 取事件（写 Graph 状态 / 刷新 UI 读模型）——「Graph 唯一写者」原则。
bool Executor::startAsync(Graph* graph, std::string* error)
{
    return startAsyncImpl(graph, nullptr, error);
}

// PD-06①：异步通用重跑（从某节点开始，复用上游结果）
bool Executor::startAsyncFrom(Graph* graph, const RerunSpec& rerun, std::string* error)
{
    return startAsyncImpl(graph, &rerun, error);
}

// startAsync / startAsyncFrom 的公共实现（rerun 为空 = 全量运行）
bool Executor::startAsyncImpl(Graph* graph, const RerunSpec* rerun, std::string* error)
{
    if (graph == nullptr) {
        if (error != nullptr) {
            *error = "graph 为空";
        }
        return false;
    }
    if (worker_.joinable()) {
        if (error != nullptr) {
            *error = "已有异步运行在进行（请先 stopAsync）";
        }
        return false;
    }

    worker_graph_ = *graph; // 副本：工作线程的节点状态/错误只落在副本上，主线程从事件应用
    if (rerun == nullptr) {
        if (!start(worker_graph_, error)) {
            return false;
        }
    }
    else if (!startFrom(worker_graph_, *rerun, error)) {
        return false;
    }

    // 拦截既有回调：改推事件（工作线程不直接写日志 / 触碰 ImGui）
    ctx_.on_console = [this](const std::string& text) {
        RunEvent event;
        event.kind = RunEvent::Kind::Console;
        event.text = text;
        pushEvent(std::move(event));
    };
    ctx_.on_node_state = [this](const std::string& node_id, NodeState state) {
        RunEvent event;
        event.kind    = RunEvent::Kind::NodeState;
        event.node_id = node_id;
        event.state   = state;
        if (state == NodeState::Error) {
            if (const Node* worker_node = worker_graph_.findNode(node_id)) {
                event.error = worker_node->error_message; // 错误文本随状态一起给主线程
            }
        }
        pushEvent(std::move(event));
    };

    stop_requested_.store(false);
    async_mode_.store(true);
    worker_ = std::thread([this]() { workerMain(); });
    return true;
}

void Executor::workerMain()
{
    while (!stop_requested_.load()) {
        const bool advanced = tick(&worker_graph_);
        if (state_ != ExecState::Running) {
            break; // 会话已结束（Finished / Cancelled / Failed）
        }
        if (!advanced) {
            break; // 未推进（理论上不应发生）
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 节拍：不空转占满 CPU
    }

    RunEvent finished;
    finished.kind        = RunEvent::Kind::Finished;
    finished.summary     = summary();
    finished.final_state = state_;
    pushEvent(std::move(finished));
    async_mode_.store(false);
}

void Executor::pushEvent(RunEvent event)
{
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        events_.push_back(std::move(event));
    }
    events_pushed_.fetch_add(1);
    events_cv_.notify_all();
}

void Executor::pumpEvents(std::vector<RunEvent>* out)
{
    std::lock_guard<std::mutex> lock(events_mutex_);
    if (out != nullptr) {
        out->insert(out->end(), events_.begin(), events_.end());
    }
    events_.clear();
}

void Executor::requestStop()
{
    stop_requested_.store(true);
    cancel(); // 既有语义：不再开始新节点（HTTP 级真取消见 PB-02）
    events_cv_.notify_all();
}

void Executor::stopAsync()
{
    if (!worker_.joinable()) {
        return;
    }
    requestStop();
    worker_.join();
}
} // namespace aiwrite::engine
