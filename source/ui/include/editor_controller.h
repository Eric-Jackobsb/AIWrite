#pragma once

// ============================================================================
//  编辑器控制器（ARCH-A3：全局编辑器单例退场后的 UI 编排层）
//
//  架构落点：ARCHITECTURE.md §1.8.10 `AppContext` / §1.8.11 依赖图 / §4 验收判据 6
//   * **不持有任何数据或服务**：8 个引用（WorkflowModel / SelectionState / RunSession /
//     Config / Clipboard / Diagnostics / AutosaveService / WorkflowFile）由 app/main 启动时
//     按 §1.8.11 顺序组装后注入；本层只做**编排**（状态栏 / 选中清空 / autosave 基线 /
//     运行收尾 / 错误条）与 UI 瞬时状态（status / last_error / request_* / follow_*）。
//   * **没有全局单例**：全局编辑器单例已随本批移除；调用方经构造参数 / 命令上下文
//     （CommandContext）拿到本对象引用，运行时不再向 `AppContext` 取任何东西（判据 8）。
//   * **没有 `app_ctx()`**：各服务直接经 `model()` / `selection()` / `run()` / `config()` /
//     `clipboard()` / `diagnostics()` / `autosave()` / `workflow_file()` 访问 ——
//     UI 模块需要哪一项就拿哪一项（§1.7 模块依赖矩阵）。
//   * 生命周期：与 AppContext 同寿（启动构造，退出析构）；析构时注销 RunEvent 订阅。
// ============================================================================

#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "autosave_service.h"    // ARCH-A1/A2：自动保存（应用级）
#include "clipboard.h"           // ARCH-A1：复制/粘贴剪贴板（应用级）
#include "config.h"              // ARCH-A1：应用配置（应用级）
#include "diagnostics_service.h" // ARCH-A1：诊断（应用级）
#include "event_bus.h"
#include "graph.h"
#include "recent_files.h"
#include "composite_editor.h" // ARCH-A6：新建 / 打包对话框状态（UI 瞬时状态）
#include "console_panel.h"   // ARCH-A6：Console 面板状态（UI 瞬时状态）
#include "node_library.h"    // ARCH-A6：节点库状态（UI 瞬时状态）
#include "node_canvas.h"     // ARCH-A6：主画布状态（NodeCanvasState）
#include "run_session.h"     // ARCH-R1：运行会话（会话级）
#include "selection_state.h" // ARCH-E4：选中状态独立对象（§1.8.4）
#include "output_panel.h"    // ARCH-A6：输出面板状态（UI 瞬时状态）
#include "settings_panel.h"  // ARCH-A6：设置面板状态（UI 瞬时状态）
#include "texture_cache.h"   // ARCH-A6：纹理缓存（渲染资源，单实例）
#include "undo_stack.h"
#include "workflow_file.h"  // ARCH-A1：工作流文件（应用级）
#include "workflow_model.h" // ARCH-E3：文档级工作流模型

namespace aiwrite::ui {

class EditorController {
public:
    // 启动注入（唯一构造点 = AppContext；§1.8.11 顺序由 AppContext 成员声明顺序保证）
    EditorController(WorkflowModel& model, SelectionState& selection, RunSession& run,
                     Config& config, Clipboard& clipboard, Diagnostics& diagnostics,
                     AutosaveService& autosave, WorkflowFile& workflow_file);
    ~EditorController();

    EditorController(const EditorController&)            = delete;
    EditorController& operator=(const EditorController&) = delete;

    // ---- 服务访问（注入后各模块直接使用；**不是**聚合入口 —— 各取所需）----
    WorkflowModel&       model() { return model_; }
    const WorkflowModel& model() const { return model_; }

    SelectionState&       selection() { return selection_; }
    const SelectionState& selection() const { return selection_; }

    RunSession&       run() { return run_; }
    const RunSession& run() const { return run_; }

    Config&       config() { return config_; }
    const Config& config() const { return config_; }

    Clipboard& clipboard() { return clipboard_; }

    Diagnostics&       diagnostics() { return diagnostics_; }
    const Diagnostics& diagnostics() const { return diagnostics_; }

    AutosaveService& autosave() { return autosave_; }

    WorkflowFile&       workflow_file() { return workflow_file_; }
    const WorkflowFile& workflow_file() const { return workflow_file_; }

    // ARCH-A6：设置面板的跨帧状态（原 `settings_panel.cpp` 19 条文件级 static）
    //  * 唯一实例由本对象持有：菜单命令 / 首启引导 / 主循环绘图三处取同一份
    SettingsPanelState&       settings_panel() { return settings_panel_; }
    const SettingsPanelState& settings_panel() const { return settings_panel_; }

    // ARCH-A6：输出面板的跨帧状态（原 `output_panel.cpp` 2 条文件级 static）
    OutputPanelState&       output_panel() { return output_panel_; }
    const OutputPanelState& output_panel() const { return output_panel_; }

    // ARCH-A6：纹理缓存（原 `texture_cache.cpp` 12 条文件级 static）
    //  * 与 GL 上下文同寿：`beginFrame()` 每帧一次、`releaseAll()` 在上下文销毁前
    TextureCache&       textures() { return textures_; }
    const TextureCache& textures() const { return textures_; }

    // ARCH-A6：主画布跨帧状态（原 `node_canvas.cpp` 11 条文件级 static + 3 条函数内 static）
    NodeCanvasState&       node_canvas() { return node_canvas_; }
    const NodeCanvasState& node_canvas() const { return node_canvas_; }

    // ARCH-A6：新建 / 打包上级节点对话框状态（原 `ce_draw.cpp` 7 条文件级 static）
    CompositeDialogsState&       composite_dialogs() { return composite_dialogs_; }
    const CompositeDialogsState& composite_dialogs() const { return composite_dialogs_; }

    // ARCH-A6：Console 面板状态（原 `console_panel.cpp` 3 条文件级 static）
    ConsolePanelState&       console_panel() { return console_panel_; }
    const ConsolePanelState& console_panel() const { return console_panel_; }

    // ARCH-A6：节点库状态（原 `node_library.cpp` 6 条文件级 static）
    NodeLibraryState&       node_library() { return node_library_; }
    const NodeLibraryState& node_library() const { return node_library_; }

    EventBus&       event_bus() { return model_.event_bus(); }
    const EventBus& event_bus() const { return model_.event_bus(); }

    // -------------------------------------------------------------- 运行 -----
    // ARCH-R1：执行器 + 快照在 RunSession（§1.8.2）—— 本层只做编排
    engine::Executor&          executor() { return run_.executor(); }
    const engine::Executor&    executor() const { return run_.executor(); }
    const engine::RunSnapshot& run_snapshot_view() const { return run_.snapshot(); }

    // 启动运行：先做运行前校验（失败 → 每条错误写 Console 与状态栏，不启动）
    bool startRun();
    // PB-01：线程化运行入口（GUI 用）
    bool startRunAsync();
    // PD-06①（FEA-M4-11 · 通用重跑）：从 node_id 开始重跑（复用上游结果）
    bool startRerunAsync(const std::string& node_id, engine::RerunScope scope);
    void stopRunAsync();   // 退出 / 切图前调用（join，幂等）
    bool session_active() const { return run_.session_active(); }
    void cancelRun();
    void tickRun();                       // 主循环每帧调用一次
    std::string runStatusText() const { return run_.statusText(); } // 状态栏文本（Idle 时为空）
    void abortRunIfAny(const std::string& reason); // 运行中修改/切换工作流时终止运行

    // ------------------------------------------------------- 工作流文件 -----
    // ARCH-A1：保存 / 打开归 `workflow_file()`（§1.8.9）；本层只编排
    //（选中清空 / autosave 基线 / 状态栏）
    bool saveWorkflowTo(const std::string& path, std::string* error);
    bool openWorkflowFrom(const std::string& path, std::string* error);

    std::string workflowDisplayName() const; // 工作流名（文件名去扩展名；空 → 未命名）

    // ---- PA-06 轻量错误条 ----
    struct LastError {
        bool        active = false;
        std::string node_id;   // 首个失败节点（点击定位 / relogin 反查）
        std::string message;   // 首个失败节点的错误消息
        std::string at;        // 本地时间 HH:MM:SS
        int         failed_count = 0;   // 失败节点总数（0/1 = 单失败，>1 = 多失败汇总）
        std::string summary;   // 汇总文案
        std::string details;   // 每节点一行 "id：消息"
        bool        relogin_available   = false; // 错误条是否显示入口
        bool        relogin_needed      = false; // true = 会话已失效
        std::string relogin_provider_id;         // 生效条目 id
        std::string relogin_text;                // 可操作文案
    };
    LastError last_error;
    void      clearLastError();
    void      refreshLastError();

    // ---- M_patchAB §3.2 / §3.4：后台诊断结果回收（工作线程写、主线程取）----
    void postBackgroundStatus(const std::string& text);
    bool takeBackgroundStatus(std::string* out);

    // 状态栏提示（ARCH-E4：选中集合已拆到 selection()）
    std::string status;

    // ---- ARCH-R2：RunEvent 订阅通知（构造时订阅 run，面板读这些字段）----
    bool        follow_output_   = false;     // 输出面板：跟随滚动
    std::string follow_node_id_;              // 输出面板：要跟随的节点 id
    bool        run_finished_    = false;     // 状态栏：本次会话收到过 Finished 事件
    std::string run_last_summary_;            // 状态栏：最后一次 Finished 的 summary

    // 一次性请求（由画布消费）
    bool request_focus_title          = false; // 双击节点标题 → 参数面板聚焦标题输入框
    bool request_navigate_to_content  = false; // 新建/粘贴后视图跟随内容
    bool request_focus_selection      = false; // P7a-17：错误条定位 → 视图居中并放大到选中节点

    // ------------------------------------------------------------- 编辑 -----
    // ARCH-E3：编辑核心操作在 WorkflowModel；本层保留同签名薄封装并做 UI 编排
    // ARCH-E2：修改前压快照由 Undo 订阅者完成；snapshot() 只发布「修改前」通知
    void        snapshot(const std::string& label);
    bool        undoOnce();
    bool        redoOnce();
    void        clearWorkflow();
    void        createSampleWorkflow();
    std::string createNode(const std::string& type, float x, float y); // 返回新节点 id

    bool        deleteSelected();
    bool        deleteNode(const std::string& node_id);

    // ---- 复制/粘贴 ----（暂停接线；逻辑与剪贴板代码保留）
    void        copySelection();
    bool        pasteClipboard();
    bool        has_clipboard() const { return clipboard_.has_content(); }
    bool        has_selection() const { return selection_.has_nodes() || selection_.has_links(); }
    void        setStatus(const std::string& text);

    void        logSummary(const std::string& prefix) const;

private:
    // ARCH-R1：运行结束的收尾（run.finish → 状态栏 → 明细日志 → 归档 → 错误条 → 自动导出）
    void finishRunSession();
    // ARCH-R2：RunEvent 订阅回调
    void onRunEvent(const engine::RunEvent& event);
    // PC-05：把本次运行结果归档到 config.output.archive_dir（失败只记日志）
    void archiveRunOutputs();

    // ---- 注入的服务（不拥有；§1.8.11 构造顺序由 AppContext 保证）----
    WorkflowModel&   model_;
    SelectionState&  selection_;
    RunSession&      run_;
    Config&          config_;
    Clipboard&       clipboard_;
    Diagnostics&     diagnostics_;
    AutosaveService& autosave_;
    WorkflowFile&    workflow_file_;

    // ARCH-A6：UI 瞬时状态（设置面板；打开态 / Config 工作副本 / 两个新建模态草稿）
    SettingsPanelState settings_panel_;
    // ARCH-A6：输出面板（大图预览弹窗请求 / 目标路径）
    OutputPanelState output_panel_;
    // ARCH-A6：纹理缓存（GR：GL 资源；本对象析构前须由 app 调 releaseAll）
    TextureCache textures_;
    // ARCH-A6：主画布（编辑器上下文 / 小地图 / 吸附 / 拖拽 / 右键菜单落点）
    NodeCanvasState node_canvas_;
    // ARCH-A6：新建 / 打包上级节点对话框（开关 / 名称缓冲 / 错误文案）
    CompositeDialogsState composite_dialogs_;
    // ARCH-A6：Console 面板（级别过滤 / 搜索框 / 自动滚动）
    ConsolePanelState console_panel_;
    // ARCH-A6：节点库（右键菜单 / 删除·复制二次确认）
    NodeLibraryState node_library_;

    std::size_t run_sub_handle_ = 0; // ARCH-R2：run.subscribeRunEvents 返回的句柄
};

// ---- 系统文件对话框入口（菜单与工具栏共用）----
// 成功返回 true；用户取消返回 false（不改动任何状态）
bool openWorkflowDialog(EditorController& editor);
bool saveWorkflowDialog(EditorController& editor);

} // namespace aiwrite::ui
