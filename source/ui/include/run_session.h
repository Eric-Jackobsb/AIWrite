#pragma once

// ============================================================================
//  运行会话（ARCH-R1：Executor + 快照从全局编辑器单例拆出）
//
//  架构落点：ARCHITECTURE.md §1.5【会话级】/ §1.8.2 `RunSession`
//   * 持有执行器（ARCH-D20）与读模型快照；每次运行重置内部状态（ARCH-D21 常驻）
//   * 运行前校验归 RunSession::start()（ARCH-D27）
//   * 不持有 Graph：校验 / 状态写回 / 权威快照用的 Graph 由调用方以方法参数传入
//   * 不碰 UI：状态栏文案 / 错误条 / 归档 / 自动导出由调用方在收尾阶段处理
//
//  线程模型（PB-01）：执行器工作线程推进；本类方法全部在主线程调用：
//     pump() 应用事件（写 Graph 节点状态 + 维护快照）；finish() 先 join 再落定权威快照。
//
//  ARCH-R2（RunEvent 订阅化）：复用现有 executor 事件队列扩展为**订阅模式**（ARCH-D10）——
//    * pumpEvents() 泵出每个事件并应用后，按 FIFO 在主线程分发给全部订阅者（形态 1：通知）
//    * 订阅只做「变化通知」，具体数据仍直接读 snapshot() / executor()（规则 3 直接读）
//    * 订阅回调内**不得**再调用本会话方法（防重入）
// ============================================================================

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "executor.h"

namespace aiwrite::ui {

class RunSession {
public:
    // ARCH-R2：RunEvent 订阅者回调（收到 pumpEvents 泵出的**每个**事件）
    using RunEventCallback = std::function<void(const engine::RunEvent&)>;
    // 订阅 RunEvent；返回句柄 id（>0）。重复订阅 = 追加（每次事件都会收到）。
    std::size_t subscribeRunEvents(RunEventCallback callback);
    // 按句柄注销；不存在的句柄为空操作。
    void unsubscribeRunEvents(std::size_t handle);
    // ------------------------------------------------------------ 启动 ------
    // 同步运行（GUI 不用；--exec-selftest 用）：校验 → executor.start
    //  * 控制台输出经 console handler 直接写日志
    bool start(engine::Graph& graph, std::string* error);
    // PB-01：线程化运行入口（GUI 用）——校验后由执行器在工作线程推进；
    //  主循环每帧 tick() → pumpEvents() 应用事件（写 Graph 状态 + 维护快照）
    bool startAsync(engine::Graph& graph, std::string* error);
    // PD-06①（FEA-M4-11 · 通用重跑）：从 node_id 开始重跑（复用上游结果）
    //  * scope=NodeOnly → 「重跑该节点」；scope=Downstream → 「重跑该节点及下游」
    //  * 重跑不清空快照（上游结果保持可见）；受影响节点由事件逐帧覆盖
    bool startRerunAsync(engine::Graph& graph, const std::string& node_id,
                         engine::RerunScope scope, std::string* error);

    // ------------------------------------------------------- 主循环驱动 -----
    // 每帧调用一次：异步会话 → 泵事件；同步会话 → 推进一个节点。
    // 返回 true = 会话已结束（Finished 事件 / 同步跑完），调用方应执行收尾。
    bool tick(engine::Graph& graph);
    // 异步：只应用事件（写 Graph 状态 + 维护快照）；返回 true = 泵到 Finished 需收尾
    bool pumpEvents(engine::Graph& graph);

    // 收尾：join → 权威快照落定 → 增量补回 → session_active_ = false
    void finish(engine::Graph& graph);

    // ------------------------------------------------------------ 停止 ------
    void stopAsync();                         // join（幂等；退出 / 切图前必须调用）
    void cancel();                            // 请求停止（正在执行的节点跑完当前步骤）
    void abortIfAny(engine::Graph& graph, const std::string& reason); // 运行中修改/切换工作流 → 终止

    // ------------------------------------------------------------ 查询 ------
    bool session_active() const { return session_active_; }
    bool running() const { return executor_.running(); }
    const engine::RunSnapshot& snapshot() const { return run_snapshot_; }
    std::string statusText() const;           // 状态栏文本（Idle 时为空）
    const engine::Executor& executor() const { return executor_; }
    engine::Executor& executor() { return executor_; } // 过渡：调用方 UI 收尾 / 自检直接读运行态

private:
    // 取/建快照中某节点的条目（type 取自 graph）
    engine::RunNodeView* snapshotEntry(const engine::Graph& graph, const std::string& node_id);

    // ARCH-R2：把单个事件分发给全部订阅者（主线程 FIFO；拷贝列表防回调改容器）
    void dispatchRunEvent(const engine::RunEvent& event);

    bool                session_active_ = false;
    engine::Executor    executor_;
    engine::RunSnapshot run_snapshot_;

    // ARCH-R2：RunEvent 订阅者（句柄 → 回调）
    std::vector<std::pair<std::size_t, RunEventCallback>> run_subscribers_;
    std::size_t                                           next_sub_id_ = 1;
};

} // namespace aiwrite::ui
