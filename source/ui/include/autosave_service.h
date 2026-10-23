#pragma once

// ============================================================================
//  自动保存服务（ARCH-A1：应用级服务收口 → AppContext · 六个服务之一）
//
//  架构落点：ARCHITECTURE.md §1.8 `AppContext` / §1.8.11 依赖图
//   * 依赖 `WorkflowModel`（+ 经 model.event_bus() 的 EventBus），不依赖 EditorState /
//     UI 面板 ⇒ 拆出为独立对象后随 ARCH-A3 editor() 退场可直接被 app 层注入
//   * 时机与判据（落点 engine::autosave_*；本层只回答「现在写不写」）：
//     - 变更感知 = 订阅内容类 EditEvent（on_edit_content 标记 dirty），不再每帧
//       算 content signature；**静默 ≥ config.ui.autosave_idle_sec 秒**（默认 3）才写一次
//     - 无变更（无事件）⇒ 一次盘都不写；写失败 ⇒ 退避 10 秒（避免每帧重试砸盘 + 刷日志）
//   * `paused_` 默认 true（PC-02：启动先问「恢复未保存的更改？」再放行 —— 否则启动时
//     刚创建的示例工作流会在 3 秒后把真快照冲掉）
//   * 配置经进程缓存 `appConfig()` 读取（与拆出前一致，保持「设置面板保存后下次启动生效」）
// ============================================================================

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "workflow_model.h"

namespace aiwrite::ui {

class AutosaveService {
public:
    explicit AutosaveService(WorkflowModel& model);
    ~AutosaveService();

    // 主循环每帧调用一次：workflow_path = 当前工作流文件路径（空 = 未命名）。
    // 返回 true = 本次真的写了快照（调用方据此写状态栏）。
    bool tick(const std::string& workflow_path);

    // true = 暂停（不写、不探测）；解闸后重新计时「静默」
    void pause(bool paused);
    // 以当前图为「已保存」基线（不写盘）
    void mark_baseline();
    // 自检钩子：是否有未保存变更（内容类编辑事件置位；SelectionChanged 不触发）
    bool dirty() const { return change_seen_; }
    // 退出前补写一次（有未保存更改时）
    void flush(const std::string& workflow_path);
    // 显式保存成功 → 清掉快照（语义 = 「已无未保存更改」）；失败只记日志
    void clear_snapshot();

private:
    void on_edit_content(); // 内容类编辑事件 → 标记「有未保存变更」

    WorkflowModel& model_;
    std::vector<std::size_t> sub_handles_; // 内容类事件订阅句柄集合

    std::chrono::steady_clock::time_point last_change_{}; // 最近一次「发现变更」时刻（事件回调刷新）
    std::chrono::steady_clock::time_point retry_after_{}; // 写失败后的退避截止
    bool change_seen_ = false;                            // 有未保存变更（事件驱动标记）
    bool paused_      = true;                             // 启动先暂停（PC-02）
};

} // namespace aiwrite::ui
