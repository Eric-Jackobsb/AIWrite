#pragma once

// ============================================================================
//  工作流文件服务（ARCH-A1：应用级服务收口 → AppContext · 六个服务之一）
//
//  架构落点：ARCHITECTURE.md §1.8 `AppContext` / §1.8.9 `WorkflowFile`
//   * 保存读 graph()，加载调 model().load()，记录最近列表（持有 RecentFiles），
//     运行结束写归档（PC-05）—— 编排（选中清空 / autosave 基线 / 状态栏）仍归 EditorState
//   * 依赖 `WorkflowModel`（文档级模型），不依赖 EditorState / UI 面板 ⇒ 拆出为独立
//     对象后随 ARCH-A3 editor() 退场可直接被 app 层注入
//   * 归口数据：current_workflow_path / restored_from_autosave_path / last_archive_dir
//     （原 EditorState 公有成员，经访问器读取）
// ============================================================================

#include <functional>
#include <string>
#include <vector>

#include "recent_files_service.h"
#include "workflow_model.h"

namespace aiwrite::engine {
class Executor;
} // namespace aiwrite::engine

namespace aiwrite::ui {

class WorkflowFile {
public:
    explicit WorkflowFile(WorkflowModel& model);

    // 保存：写文件（is_secret 参数不落盘：设计 §8.4）+ 记录最近列表 + 更新当前路径。
    // 成功返回 true；失败写 *error 与日志。autosave 清快照 / 基线由调用方编排。
    bool save_workflow_to(const std::string& path, std::string* error);

    // 打开：加载 → **加载校验**（不合法则拒绝并保持原图）→ before_replace（调用方注入的
    // 运行守卫等，在整图替换**之前**执行）→ model().load → 记录最近列表。
    bool open_workflow_from(const std::string& path, std::string* error,
                            const std::function<void()>& before_replace = {});

    // 读取 ~/.brain-ai/recent.json
    std::vector<engine::RecentEntry> recent_workflows() const;

    // ---- 归口数据（原 EditorState 公有成员）----
    const std::string& current_workflow_path() const { return current_workflow_path_; }
    void               set_current_workflow_path(std::string path) { current_workflow_path_ = std::move(path); }
    const std::string& restored_from_autosave_path() const { return restored_from_autosave_path_; }
    void               set_restored_from_autosave_path(std::string path) { restored_from_autosave_path_ = std::move(path); }
    const std::string& last_archive_dir() const { return last_archive_dir_; }
    void               set_last_archive_dir(std::string dir) { last_archive_dir_ = std::move(dir); }

    // 工作流名（当前文件路径去扩展名；空 → 未命名）
    std::string workflow_display_name() const;

    // PC-05：运行结束归档（每节点 .txt + run.json）；参数取 appConfig().output.*。
    // 成功写 last_archive_dir_；失败只记日志（不打断运行结束流程）。
    void archive_run_outputs(const engine::Executor& executor);

    // 最近列表服务（「文件 → 最近打开」菜单与清理入口用）
    RecentFiles&       recent_files() { return recent_files_; }
    const RecentFiles& recent_files() const { return recent_files_; }

private:
    WorkflowModel& model_;
    RecentFiles    recent_files_;
    std::string    current_workflow_path_;
    std::string    restored_from_autosave_path_;
    std::string    last_archive_dir_;
};

} // namespace aiwrite::ui
