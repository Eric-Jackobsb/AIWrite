#include "engine/executor.h"

#include "engine/validate.h"

#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace aiwrite::engine {

namespace {

// PA-01：节点耗时（毫秒）
double ms_since(const std::chrono::steady_clock::time_point& start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

// PA-04：耗时文本（<100ms 保留两位小数，便于看清本地节点的瞬时耗时）
std::string format_ms(double ms)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(ms < 100.0 ? 2 : 0) << ms << " ms";
    return stream.str();
}

} // namespace

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
}

// ---------------------------------------------------------------- Executor ----
Executor::Executor()
{
    ctx_.outputs   = &outputs_;
    ctx_.cancelled = &cancelled_;
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
    plan_.clear();
    cursor_ = 0;
    finished_ = 0;
    failed_ = 0;
    skipped_ = 0;
    state_ = ExecState::Idle;
    current_node_.clear();
    cancelled_ = false;
    outputs_.clear();
    run_infos_.clear();
    start_time_ = {};
    end_time_ = {};
}

void Executor::cancel()
{
    if (state_ == ExecState::Running) {
        cancelled_ = true;
    }
}

bool Executor::start(Graph& graph, std::string* error)
{
    reset();

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
    ctx_.console("[执行器] 开始执行：共 " + std::to_string(plan_.size()) + " 个节点");
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
        ctx_.console("[执行器] 已取消：" + summary());
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

bool Executor::runToCompletion(Graph* graph, int max_steps)
{
    if (state_ != ExecState::Running) {
        return false;
    }
    int steps = 0;
    while (state_ == ExecState::Running && steps < max_steps) {
        if (!tick(graph)) {
            break;
        }
        ++steps;
    }
    if (state_ == ExecState::Running) {
        ctx_.console("[执行器] 超过最大步数 " + std::to_string(max_steps) + "，已中止");
        state_    = ExecState::Failed;
        end_time_ = std::chrono::steady_clock::now();
        return false;
    }
    return state_ == ExecState::Finished;
}

bool Executor::executeNode(Graph& graph, const std::string& node_id)
{
    Node* node = graph.findNode(node_id);
    if (node == nullptr) {
        return false;
    }

    node->state = NodeState::Running;
    node->error_message.clear();
    current_node_ = node_id;
    notifyState(node_id, NodeState::Running);
    ctx_.console("[" + node_id + "] 开始执行（" + node->type + " · " + node->title + "）");
    const auto started = std::chrono::steady_clock::now(); // PA-01：节点耗时起点

    try {
        const nlohmann::json inputs = collectInputs(graph, *node);
        const nlohmann::json params = paramObject(*node);
        const NodeExecutor*  executor = NodeExecutorRegistry::instance().find(node->type);
        if (executor == nullptr) {
            throw NodeError("节点类型缺少执行实现: " + node->type);
        }

        nlohmann::json output = (*executor)(inputs, params, ctx_);

        // 约定：单输出节点 → 返回值即该端口的值；多输出节点 → {"端口id": 值}
        if (node->outputs.size() == 1) {
            outputs_.set(node_id, node->outputs[0].id, std::move(output));
        }
        else if (!node->outputs.empty()) {
            if (!output.is_object()) {
                throw NodeError("多输出节点的执行结果必须是 {\"端口id\": 值} 对象");
            }
            for (const Port& port : node->outputs) {
                if (output.contains(port.id)) {
                    outputs_.set(node_id, port.id, output[port.id]);
                }
            }
        }

        node->state = NodeState::Done;
        notifyState(node_id, NodeState::Done);
        ++finished_;
        const double done_ms = ms_since(started);
        recordRun(*node, NodeState::Done, done_ms, {});
        ctx_.console("[" + node_id + "] 完成（" + format_ms(done_ms) + "）");
        current_node_.clear();
        return true;
    }
    catch (const std::exception& ex) {
        node->state         = NodeState::Error;
        node->error_message = ex.what();
        notifyState(node_id, NodeState::Error);
        ++failed_;
        const double failed_ms = ms_since(started);
        recordRun(*node, NodeState::Error, failed_ms, node->error_message);
        ctx_.console("[" + node_id + "] 失败（" + format_ms(failed_ms) + "）：" + node->error_message);
        markDownstreamSkipped(graph, node_id);
        current_node_.clear();
        return false;
    }
}

} // namespace aiwrite::engine

