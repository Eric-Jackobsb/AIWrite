#include "workflow_file.h"

#include "config.h"      // appConfig()：进程级配置缓存（归档参数 output.*）
#include "file_dialog.h" // utils::openFolder（output.auto_open_on_complete）
#include "log.h"
#include "output_archive.h" // utils::ArchiveRequest / archiveRun（PC-05 归档）
#include "output_panel.h"   // node_output_text（与输出面板同源的节点结果文本）
#include "validate.h"       // engine::validateWorkflow（打开前加载校验）
#include "workflow_io.h"    // engine::saveWorkflow / loadWorkflow

#include <filesystem>

namespace aiwrite::ui {

WorkflowFile::WorkflowFile(WorkflowModel& model) : model_(model) {}

bool WorkflowFile::save_workflow_to(const std::string& path, std::string* error)
{
    if (path.empty()) {
        if (error != nullptr) {
            *error = "路径为空";
        }
        return false;
    }

    std::string save_error;
    if (!engine::saveWorkflow(model_.graph, path, &save_error)) {
        if (error != nullptr) {
            *error = save_error;
        }
        log::error("保存工作流失败: " + save_error);
        log::workflow("[工作流-保存失败] " + path + "（" + save_error + "）");
        return false;
    }

    current_workflow_path_ = path;
    recent_files_.push(path, model_.graph.name);
    log::info("工作流已保存: " + path);
    log::workflow("[工作流-保存] " + path + "（节点 " +
                  std::to_string(model_.graph.nodes.size()) + "，连线 " +
                  std::to_string(model_.graph.edges.size()) + "）");
    return true;
}

bool WorkflowFile::open_workflow_from(const std::string& path, std::string* error,
                                      const std::function<void()>& before_replace)
{
    if (path.empty()) {
        if (error != nullptr) {
            *error = "路径为空";
        }
        return false;
    }

    engine::Graph loaded;
    std::string   load_error;
    if (!engine::loadWorkflow(path, loaded, &load_error)) {
        if (error != nullptr) {
            *error = load_error;
        }
        log::error("打开工作流失败: " + path + " → " + load_error);
        log::workflow("[工作流-加载失败] " + path + " → " + load_error);
        return false;
    }

    // 加载校验（三级校验的第二级）：不合法则拒绝并保持原图不变
    engine::ValidationMessages issues;
    if (!engine::validateWorkflow(loaded, &issues)) {
        const std::string first = issues.empty() ? std::string("未知问题") : issues.front();
        if (error != nullptr) {
            *error = "加载校验失败：" + first;
        }
        log::error("打开工作流被拒绝（加载校验）: " + path + " → " + first);
        log::workflow("[工作流-加载拒绝] " + path + " → " + first);
        return false;
    }

    // 运行守卫等前置回调（在整图替换**之前**执行）
    if (before_replace) {
        before_replace();
    }
    // ARCH-E3：整图替换（替换数据 + 清空撤销栈 + 逐节点 diff 事件）归 WorkflowModel::load
    model_.load(std::move(loaded));
    current_workflow_path_ = path;
    recent_files_.push(path, model_.graph.name);

    log::info("工作流已打开: " + path);
    log::workflow("[工作流-加载] " + path + "（节点 " +
                  std::to_string(model_.graph.nodes.size()) + "，连线 " +
                  std::to_string(model_.graph.edges.size()) + "）");
    return true;
}

std::vector<engine::RecentEntry> WorkflowFile::recent_workflows() const
{
    return recent_files_.load();
}

std::string WorkflowFile::workflow_display_name() const
{
    if (current_workflow_path_.empty()) {
        return "未命名";
    }
    const std::filesystem::path path(current_workflow_path_);
    const std::string           stem = path.stem().string();
    return stem.empty() ? std::string("未命名") : stem;
}

void WorkflowFile::archive_run_outputs(const engine::Executor& executor)
{
    // 节点条目与输出面板同源（runInfos + outputs）→ 归档内容与面板一致
    std::vector<utils::ArchiveNode> nodes;
    nodes.reserve(executor.runInfos().size());
    for (const engine::NodeRunInfo& info : executor.runInfos()) {
        utils::ArchiveNode item;
        item.node_id     = info.node_id;
        item.type        = info.type;
        item.state       = engine::nodeStateName(info.state);
        item.duration_ms = info.duration_ms;
        item.error       = info.error;
        item.text        = node_output_text(model_.graph, executor, info.node_id);
        nodes.push_back(std::move(item));
    }

    const Config& config = appConfig();

    utils::ArchiveRequest request;
    request.workflow_name = workflow_display_name();
    request.archive_dir   = config.output.archive_dir;
    request.keep_history  = config.output.keep_history;
    request.max_history   = config.output.max_history;
    request.ttl_days      = config.output.ttl_days;

    const utils::ArchiveResult result = utils::archiveRun(request, nodes, executor.summary());
    if (result.ok) {
        last_archive_dir_ = result.dir;
        log::info("[归档] 运行输出已写入 " + result.dir + "（" + std::to_string(result.files) +
                  " 个文件）");
        log::workflow("[归档] " + result.dir + "（" + std::to_string(result.files) + " 个文件）");

        // ---- M6 接线：`output.auto_open_on_complete`（完成后自动打开归档目录）----
        //  * 默认 false ⇒ 行为与改造前**逐字一致**（零副作用）
        //  * 打开失败只写日志（不打断运行结束流程，也不改归档结果）
        if (config.output.auto_open_on_complete) {
            if (utils::openFolder(result.dir)) {
                log::info("[归档] 已按配置自动打开归档目录（output.auto_open_on_complete = true）");
            }
            else {
                log::warn("[归档] 自动打开归档目录失败：" + result.dir);
            }
        }
    }
    else {
        log::warn("[归档] 失败：" + result.error);
        log::workflow("[归档-失败] " + result.error);
    }
}

} // namespace aiwrite::ui
