#pragma once

// ============================================================================
//  编辑器状态（UI 层共享）
//
//  * 持有唯一的工作流图 Graph 与撤销栈 UndoStack
//  * 提供"修改前自动压快照"的编辑操作，供画布 / 节点库 / 工具栏 / 菜单调用
//  * 所有可验证的逻辑都在 engine::Graph 内，本层只做编排与日志
// ============================================================================

#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "executor.h"
#include "graph.h"
#include "recent_files.h"
#include "undo_stack.h"

namespace aiwrite::ui {

struct EditorState {
    engine::Graph      graph;
    engine::UndoStack  undo;

    // -------------------------------------------------------------- 运行 -----
    // 执行会话（M2-04）：分帧推进由主循环调用 tick_run()；
    //  * 运行态端口值保存在 Executor 内部，**不写回 Graph**
    //  * 节点状态写在 Graph 上（画布就地染状态色）
    engine::Executor executor;

    // 启动运行：先做运行前校验（失败 → 每条错误写 Console 与状态栏，不启动）
    bool start_run();
    // PB-01：线程化运行入口（GUI 用）——校验后由执行器在工作线程推进；
    //  主循环每帧 tick_run() → pump_run_events() 应用事件（写 Graph 状态 + 维护 run_snapshot）
    bool start_run_async();
    void pump_run_events();
    void stop_run_async();   // 退出 / 切图前调用（join，幂等）
    bool session_active() const { return session_active_; }
    void cancel_run();
    void tick_run();                       // 主循环每帧调用一次
    std::string run_status_text() const;    // 状态栏文本（Idle 时为空）
    void abort_run_if_any(const std::string& reason); // 运行中修改/切换工作流时终止运行

    // ------------------------------------------------------- 工作流文件 -----
    // 保存（is_secret 参数不落盘：设计 §8.4）+ 记录最近列表
    bool save_workflow_to(const std::string& path, std::string* error);
    // 打开：加载 → **加载校验**（不合法则拒绝并保持原图）→ 清空撤销栈 → 记录最近列表
    bool open_workflow_from(const std::string& path, std::string* error);
    std::vector<engine::RecentEntry> recent_workflows() const; // 读取 ~/.brain-ai/recent.json

    std::string current_workflow_path;   // 当前文件路径（保存对话框的默认名用）

    // ---- PC-01：自动保存（节流；主循环每帧调用 tick_autosave()）----
    //  判据与落点见 engine/autosave.h；本层只负责**何时**写：
    //   * 1 Hz 用 engine::graph_content_signature() 探测（视口平移 / 缩放不算变更）
    //   * 从首次发现变更起，**静默 ≥ config.ui.autosave_idle_sec 秒**（默认 3）才写一次
    //   * 仅变更时写（无变更 ⇒ 一次盘都不写）
    //   * 启动时 `autosave_paused = true`（PC-02：先问「恢复未保存的更改？」再放行）
    //     —— 否则启动时创建的示例工作流会在 3 秒后把真快照冲掉
    void tick_autosave();                       // 主循环每帧调用一次（内部 1 Hz 探测）
    void pause_autosave(bool paused);           // true = 暂停（不写、不探测）
    void mark_autosave_baseline();              // 以当前图为「已保存」基线（不写盘）
    void flush_autosave_on_exit();              // 退出前补写一次（有未保存更改时）
    // PC-02：把快照恢复进当前图（**不覆盖原文件** —— 恢复后 `current_workflow_path` 留空，
    //  保存必然走「另存为」，原文件由用户显式决定；原路径记在 restored_from_autosave_path）
    bool restore_autosave(std::string* error);
    std::string restored_from_autosave_path;    // 快照记录的原工作流路径（展示用；空 = 未命名）
    bool autosave_paused = true;

    // ---- 运行输出归档（PC-05）----
    // 运行结束自动写入 outputs/<时间>-<工作流名>/（每节点 .txt + run.json）
    // 参数取 config.output.*（archive_dir / keep_history / max_history / ttl_days）
    // ---- PB-01：线程化运行的 UI 读模型 ----
    //  * 运行中由事件维护（节点状态/输出全文/结束统计）；结束时由 makeSnapshot 用权威数据落定
    //  * UI 只读它，永不跨线程读 Executor 运行态
    engine::RunSnapshot run_snapshot;
    const engine::RunSnapshot& run_snapshot_view() const { return run_snapshot; }

    std::string last_archive_dir;        // 最近一次归档目录（输出面板显示 + 自检断言）
    std::string workflow_display_name() const; // 工作流名（文件名去扩展名；空 → 未命名）

    // ---- PA-06 轻量错误条 ----
    // 说明：运行结束时刷新 —— 记录**首个失败节点**（状态栏红条 + 点击定位）；
    //       成功运行会清空；连续失败只覆盖显示，不堆积历史
    struct LastError {
        bool        active = false;
        std::string node_id;
        std::string message;
        std::string at; // 本地时间 HH:MM:SS
        // ---- M_patchAB §2.2（`PB-07`）：会话失效 → 界面「重新登录」入口 ----
        //  * 由 `refresh_last_error()` 用纯函数 `web::classify_session_failure_text()` **反查**
        //    失败原因得出（只有判据命中才给入口 —— 不猜、不误报）
        //  * 按钮打开的是**该失败节点生效条目**的登录窗口（`resolve_effective_provider` 解析）
        bool        relogin_available   = false; // 错误条是否显示入口
        bool        relogin_needed      = false; // true = 会话已失效（必须重登）/ false = 请求被拒（验证类）
        std::string relogin_provider_id;         // 生效条目 id（按钮据此开对应站点窗口）
        std::string relogin_text;                // 可操作文案（含判据与下一步）
    };
    LastError last_error;
    void      clear_last_error();
    void      refresh_last_error();

    // ---- M_patchAB §3.2 / §3.4（`PB2-07` / `PB2-30②`）：后台诊断结果回收 ----
    //  * 界面「测试连接 / 测试选择器」在工作线程执行（**界面不阻塞**）：
    //    细节走 Console（`log::info`），结论经本接口投递给主线程写状态栏
    //  * 工作线程只写、主线程每帧取一次（`std::mutex` 保护）；未取走时后来的结论**覆盖**前者
    void post_background_status(const std::string& text);
    bool take_background_status(std::string* out);

    // 画布每帧同步的选择信息
    std::vector<std::string> selected_nodes;
    std::vector<std::string> selected_links;       // 选中的连线（左键点连线 / 框选）
    std::string              selected_node;        // 单选时用于参数面板

    std::string status;                       // 状态栏提示

    // 一次性请求（由画布消费）
    bool request_focus_title          = false; // 双击节点标题 → 参数面板聚焦标题输入框
    bool request_navigate_to_content  = false; // 新建/粘贴后视图跟随内容
    bool request_focus_selection      = false; // P7a-17：错误条定位 → 视图**居中并放大**到选中节点

    // ------------------------------------------------------------- 编辑 -----
    void        snapshot(const std::string& label);   // 修改前压快照
    bool        undo_once();
    bool        redo_once();
    void        clear_workflow();
    void        create_sample_workflow();
    std::string create_node(const std::string& type, float x, float y); // 返回新节点 id

    bool        delete_selected();
    bool        delete_node(const std::string& node_id);

    // ---- 复制/粘贴 ----
    // 暂停接线（UI 入口已下线，见 CHANGELOG「复制/粘贴暂停」）：逻辑与剪贴板代码保留，
    // M2/M3 重做画布交互时接回；接回前必须先修好画布位置同步（node_canvas.cpp sync_positions）。
    void        copy_selection();
    bool        paste_clipboard();
    bool        has_clipboard() const { return !clipboard_nodes_.empty(); }
    bool        has_selection() const { return !selected_nodes.empty() || !selected_links.empty(); }
    void        set_status(const std::string& text);

    void        log_summary(const std::string& prefix) const;

    // PC-05：把本次运行结果归档到 config.output.archive_dir（失败只记日志）
    void        archive_run_outputs();

private:
    // PB-01：会话是否活动（异步运行中，或结束事件尚未全部应用）
    bool session_active_ = false;
    // PB-01：把结束事件后的收尾（join → 权威快照 → 归档 → 错误条）集中一处
    void finish_run_session();
    // PB-01：取/建 run_snapshot 中某节点的条目
    engine::RunNodeView* snapshot_entry(const std::string& node_id);

    std::vector<engine::Node> clipboard_nodes_;                        // 复制的节点
    std::vector<engine::Edge> clipboard_edges_;                        // 选区内部的连线

    // ---- M_patchAB（PB2-07 / PB2-30②）：后台诊断结论（工作线程写、主线程取）----
    std::mutex  background_mutex_;
    std::string background_status_;
    bool        background_pending_ = false;

    // ---- PC-01：自动保存节流状态（说明见上方 tick_autosave 注释）----
    std::string                           autosave_signature_;      // 上次写入快照时的内容指纹
    std::chrono::steady_clock::time_point autosave_last_probe_{};    // 上次探测时刻（1 Hz）
    std::chrono::steady_clock::time_point autosave_last_change_{};   // 最近一次「发现变更」时刻
    std::chrono::steady_clock::time_point autosave_retry_after_{};   // 写失败后的退避截止
    bool                                  autosave_change_seen_ = false;
};

// 全局单例（UI 生命周期内唯一）
EditorState& editor();

// ---- 系统文件对话框入口（菜单与工具栏共用）----
// 成功返回 true；用户取消返回 false（不改动任何状态）
bool open_workflow_dialog();
bool save_workflow_dialog();

} // namespace aiwrite::ui
