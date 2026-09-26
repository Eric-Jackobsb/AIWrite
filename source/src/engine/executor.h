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
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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

// 单节点运行信息（PA-01：只读暴露）
//  * **不进** Graph / 撤销快照 / 工作流 JSON —— 运行态值原则（M_patchA §0.3）
//  * 用途：输出面板 / 画布节点摘要 / 参数面板「运行结果」/ workflow.log 明细
//  * delta_bytes：Patch B 的流式增量字节数（预留，只增不改）
struct NodeRunInfo {
    std::string node_id;
    std::string type;
    NodeState   state       = NodeState::Waiting;
    double      duration_ms = 0.0;
    std::string error;
    std::size_t delta_bytes = 0;
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
    // PB-03：流式增量回调（节点实现按段吐出；执行器转成事件给主线程）
    std::function<void(const std::string&)> on_delta;
    // PB-05：只读图引用 + 当前节点 id —— 推理节点据此读取「提供商配置」等配置类节点的参数
    // （例如官方 API 的 api_key）；仅运行期使用，绝不写入 Graph / 不落盘 / 不打印
    const Graph* graph = nullptr;
    std::string  current_node_id;

    bool is_cancelled() const { return cancelled != nullptr && cancelled->load(); }
    void console(const std::string& text) const
    {
        if (on_console) {
            on_console(text);
        }
    }
    // PB-03：向 UI 吐一段增量（无回调时空操作）
    void delta(const std::string& text) const
    {
        if (on_delta && !text.empty()) {
            on_delta(text);
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

// ------------------------------------------------------------------ PB-01 ----
// 线程化运行的事件（工作线程 → 主线程；UI 只在主线程消费，绝不跨线程读运行态）
struct RunEvent {
    enum class Kind { NodeState, Console, NodeOutput, Delta, Finished };
    Kind        kind        = Kind::NodeState;
    std::string node_id;
    std::string text;                          // Console 文本 / 节点输出全文（NodeOutput）
    std::string error;                         // NodeState::Error 时的错误文本
    NodeState   state       = NodeState::Waiting;
    ExecState   final_state = ExecState::Idle; // Finished 事件用
    std::string summary;                       // Finished 事件用
};

// 运行结果只读快照（PB-01：UI 侧读模型）
//  * 线程安全：由事件构建（运行中）或运行结束后一次性构建（`makeSnapshot`），UI 只读快照
//  * 与 Executor 的权威数据同源：`--run-selftest` 断言「快照 == runInfos()/outputs()」
struct RunNodeView {
    std::string node_id;
    std::string type;
    std::string text;                        // 节点输出全文（增量时代入，结束时落定为权威值）
    NodeState   state       = NodeState::Waiting;
    double      duration_ms = 0.0;
    std::string error;
    std::size_t delta_bytes = 0;             // 流式增量累计（PB-03/PB-08）
    // M5-03：Image 端口的结果（本地图片路径，按端口顺序）—— 输出面板据此渲染缩略图
    std::vector<std::string> images;
};

struct RunSnapshot {
    std::vector<RunNodeView> nodes;
    std::string              summary;
    bool                     running     = false;
    ExecState                final_state = ExecState::Idle;

    const RunNodeView* find(const std::string& node_id) const;
};

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

    // ---- PB-01：线程化运行（GUI 用；自检/无界面仍用同步 start()/tick()）----
    //  * 工作线程在 **Graph 副本**上推进 tick，只向事件队列推送（不触碰主线程的 Graph / ImGui / 日志）
    //  * 主线程每帧 pumpEvents() 取事件：写 Graph 节点状态 + 刷新 UI 读模型
    //  * startAsync 会拦截既有的 console / state 回调（改为事件），调用方应从事件侧处理输出
    bool        startAsync(Graph* graph, std::string* error);
    void        pumpEvents(std::vector<RunEvent>* out); // 主线程调用
    void        requestStop();                          // 置位 + 唤醒（真取消见 PB-02）
    void        stopAsync();                            // 请求停止 + join（退出/切图前必调）
    bool        asyncMode() const { return async_mode_.load(); }
    std::size_t eventsPushed() const { return events_pushed_.load(); }

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
    // PA-01：本次运行的逐节点信息（按执行顺序；只读，供 UI 与日志）
    const std::vector<NodeRunInfo>& runInfos() const { return run_infos_; }

private:
    bool                 executeNode(Graph& graph, const std::string& node_id);
    nlohmann::json       collectInputs(const Graph& graph, const Node& node) const;
    static nlohmann::json paramObject(const Node& node);
    void                 markDownstreamSkipped(Graph& graph, const std::string& node_id);
    void                 notifyState(const std::string& node_id, NodeState state) const;
    // PA-01：记录一次节点执行（完成 / 失败 / 跳过）
    void recordRun(const Node& node, NodeState state, double duration_ms, std::string error);

    std::vector<std::string> plan_;
    std::size_t              cursor_ = 0;
    int                      finished_ = 0;
    int                      failed_ = 0;
    int                      skipped_ = 0;
    ExecState                state_ = ExecState::Idle;
    std::string              current_node_;
    std::atomic<bool>        cancelled_{false};
    NodeOutputs              outputs_;
    std::vector<NodeRunInfo> run_infos_; // PA-01：逐节点运行信息（只读暴露）
    ExecutionContext         ctx_;
    std::chrono::steady_clock::time_point start_time_{};
    std::chrono::steady_clock::time_point end_time_{};

    // ---- PB-01：线程化运行 ----
    void  workerMain();
    void  pushEvent(RunEvent event);
    void  handleDelta(const std::string& text); // PB-03：增量 → Delta 事件（主线程追加到读模型）

    Graph                    worker_graph_;              // 工作线程专用副本（避免跨线程写 Graph）
    std::thread              worker_;
    std::atomic<bool>        async_mode_{false};
    std::atomic<bool>        stop_requested_{false};
    std::atomic<std::size_t> events_pushed_{0};
    mutable std::mutex       events_mutex_;
    std::condition_variable  events_cv_;
    mutable std::vector<RunEvent> events_;
};

// PB-01：从执行器当前数据生成快照（主线程在运行结束/空闲时调用）
//  * 需要 graph 以便把「节点输出全文」一并落定（与 ui::node_output_text 同源）
RunSnapshot makeSnapshot(const Graph& graph, const Executor& executor);

// 节点输出文本（引擎侧唯一实现；`ui::node_output_text` 委托到它，保证多线程只有一份规则）
std::string nodeOutputText(const Graph& graph, const NodeOutputs& outputs, const std::string& node_id);

} // namespace aiwrite::engine
