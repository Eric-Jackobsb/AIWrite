#pragma once

// ============================================================================
//  工作流执行器（M2-04；设计 §9.1 / §9.3 / §10）
//
//  设计要点
//   * 单线程分帧：tick() 每帧推进一个节点（UI 调用）；无 UI 场景用 runToCompletion()
//   * 状态机：Waiting → Running → Done / Error；失败节点的下游标 Skipped，无关分支继续（§10.4）
//   * 取消：cancel() 置位后不再开始新节点（同步执行无法中断"正在执行"的节点，
//     该限制由 M4 的独立 HTTP 线程解决）
//   * 运行态**值**只存在于 Executor（NodeOutputs），不进入 Graph —— Graph 是"文档"，
//     因此撤销快照与工作流 JSON 不受执行结果影响；节点**状态**仍写在 Graph 上（画布就地绘制状态色）
//   * 节点实现按类型注册在 NodeExecutorRegistry（`nodes::registerAllExecutors()` 填充）
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/graph.h"

namespace aiwrite::engine {

// ---------------------------------------------------------------- 运行态值 ----
// node_id → port_id → json
//  * Text 端口：字符串；Number：数字；Image：文件路径字符串；Provider：配置对象
class NodeOutputs {
public:
    void                   set(const std::string& node_id, const std::string& port_id,
                               nlohmann::json value);
    const nlohmann::json*  find(const std::string& node_id, const std::string& port_id) const;
    void                   clear();
    std::size_t            size() const;

private:
    std::unordered_map<std::string, std::unordered_map<std::string, nlohmann::json>> values_;
};

// 节点执行错误（设计 §9.3）：执行器捕获 → 该节点 Error，下游 Skipped
class NodeError : public std::runtime_error {
public:
    explicit NodeError(const std::string& message) : std::runtime_error(message) {}
};

struct ExecutionContext;

// 节点执行函数（设计 §9.1）
//  * inputs：输入端口 id → 值（变长端口为数组，按边插入序）
//  * params：参数 id → 值
//  * 返回值：单输出节点 → 直接是该端口的值；多输出节点 → {"端口id": 值, ...}
using NodeExecutor = std::function<nlohmann::json(const nlohmann::json& inputs,
                                                  const nlohmann::json& params,
                                                  ExecutionContext& ctx)>;

struct ExecutionContext {
    NodeOutputs*                            outputs   = nullptr; // 运行态值（只读旁路）
    std::atomic<bool>*                      cancelled = nullptr; // 协作式取消检查
    std::function<void(const std::string&)> on_console;          // 输出/日志（P3-4 接 workflow.log）
    std::function<void(const std::string&, NodeState)> on_node_state; // UI 状态刷新（P3-3 接画布）

    bool is_cancelled() const { return cancelled != nullptr && cancelled->load(); }
    void console(const std::string& text) const
    {
        if (on_console) {
            on_console(text);
        }
    }
};

// 节点执行函数注册表
class NodeExecutorRegistry {
public:
    static NodeExecutorRegistry& instance();

    void                registerExecutor(const std::string& type, NodeExecutor executor);
    const NodeExecutor* find(const std::string& type) const;
    std::size_t         size() const;
    void                clear();

private:
    std::unordered_map<std::string, NodeExecutor> executors_;
};

// ---------------------------------------------------------------- 执行会话 ----
enum class ExecState { Idle, Running, Finished, Cancelled, Failed };

const char* execStateName(ExecState state);

class Executor {
public:
    Executor();

    // 输出/日志回调（P3-3 接 Console 面板、P3-4 接 workflow.log；自检用于捕获输出）
    void setConsoleHandler(std::function<void(const std::string&)> handler);
    // 节点状态回调（UI 刷新画布；自检用于校验状态流转）
    void setStateHandler(std::function<void(const std::string&, NodeState)> handler);

    // 运行前校验 + 拓扑排序 + 检查执行实现；成功时把全部节点状态重置为 Waiting
    bool start(Graph& graph, std::string* error);

    // 推进一帧（执行一个节点）；返回 false 表示本次未推进（未启动/已结束）
    bool tick(Graph* graph);

    // 无 UI 驱动：循环 tick 直到结束（自检 / 将来的命令行运行）
    bool runToCompletion(Graph* graph, int max_steps = 4096);

    void cancel();
    void reset();

    ExecState       state() const { return state_; }
    bool            running() const { return state_ == ExecState::Running; }
    int             finishedCount() const { return finished_; }
    int             failedCount() const { return failed_; }
    int             skippedCount() const { return skipped_; }
    int             totalCount() const { return static_cast<int>(plan_.size()); }
    std::string     currentNode() const { return current_node_; }
    double          elapsedSeconds() const;
    std::string     summary() const;
    const std::vector<std::string>& plan() const { return plan_; }
    const NodeOutputs&              outputs() const { return outputs_; }

private:
    bool                 executeNode(Graph& graph, const std::string& node_id);
    nlohmann::json       collectInputs(const Graph& graph, const Node& node) const;
    static nlohmann::json paramObject(const Node& node);
    void                 markDownstreamSkipped(Graph& graph, const std::string& node_id);
    void                 notifyState(const std::string& node_id, NodeState state) const;

    std::vector<std::string> plan_;
    std::size_t              cursor_ = 0;
    int                      finished_ = 0;
    int                      failed_ = 0;
    int                      skipped_ = 0;
    ExecState                state_ = ExecState::Idle;
    std::string              current_node_;
    std::atomic<bool>        cancelled_{false};
    NodeOutputs              outputs_;
    ExecutionContext         ctx_;
    std::chrono::steady_clock::time_point start_time_{};
    std::chrono::steady_clock::time_point end_time_{};
};

} // namespace aiwrite::engine
