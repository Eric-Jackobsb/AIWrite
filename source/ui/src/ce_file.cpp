#include "ce_session.h"

#include "composite.h"
#include "editor_controller.h"
#include "log.h"

#include <algorithm>
#include <array>
#include <string>

namespace aiwrite::ui {

// =============================================================== CeFile ====
// 文件级（对称 WorkflowFile · ARCH-Z2）：打开 / 保存 / 校验 + 注册表 + 主画布实例热重载。
// 自 ce_session.cpp 迁出（逻辑逐行保持，仅把「会话状态」换成 model() 的工作副本）。

bool CeFile::open(const std::string& composite_id, std::string* error)
{
    const engine::CompositeSpec* existing =
        engine::CompositeRegistry::instance().find(composite_id);

    // PS-D9：内置官方预设只读 —— 拦截打开（须先「复制到我的节点库」）
    if (engine::CompositeRegistry::instance().is_builtin(composite_id)) {
        if (error != nullptr) {
            *error = "官方预设为只读：请先在节点库「复制到我的节点库」再编辑";
        }
        log::warn("[上级节点] 拒绝打开内置只读规格 " + composite_id + "（须先复制到用户源）");
        return false;
    }

    if (existing != nullptr) {
        model_.load(*existing);
    }
    else {
        // 规格缺失（内存会话复现路径）：全新空白规格，id 由注册表现场分配
        engine::CompositeSpec fresh;
        fresh.id = engine::CompositeRegistry::instance().makeId();
        model_.load(std::move(fresh));
    }
    return true;
}

bool CeFile::save(const std::string& name, const std::string& description, std::string* error,
                  EditorController& main_editor)
{
    engine::CompositeSpec& working = model_.spec();
    working.name                   = name;
    working.description            = description;
    if (working.id.empty()) {
        working.id = engine::CompositeRegistry::instance().makeId();
    }

    // 保存前的旧规格名（热重载时用于区分「实例标题是否被用户自定义过」）
    const engine::CompositeSpec* old_spec =
        engine::CompositeRegistry::instance().find(working.id);
    const std::string old_name = (old_spec != nullptr) ? old_spec->name : std::string();

    std::string save_error;
    if (!engine::CompositeRegistry::instance().save(working, &save_error)) {
        if (error != nullptr) {
            *error = save_error;
        }
        return false;
    }

    // 热重载：把主画布中已放置的同 id 实例按新规格重建（保留 id/位置/自定义标题/已填参数值）
    EditorController& main_state = main_editor;
    const bool        has_instances =
        std::any_of(main_state.model().graph.nodes.begin(), main_state.model().graph.nodes.end(),
                    [&](const engine::Node& n) { return n.composite_id == working.id; });
    if (has_instances) {
        // 改图前压主画布撤销快照（内部也会终止正在进行的运行）
        main_state.snapshot("热重载上级节点 " + working.name);
    }
    std::string refresh_error;
    const int   refreshed = engine::refresh_composite_instances(
        main_state.model().graph, working.id, old_name, &refresh_error);
    if (refreshed > 0) {
        // 清理可能已失效的连线选中引用（节点 id 保留，选中节点不受影响）
        // ARCH-E4：选中独立对象 —— prune_links 移除失效引用（内置变化检测）
        main_state.selection().prune_links([&](const std::string& eid) {
            return main_state.model().graph.findEdge(eid) != nullptr;
        });
        main_state.setStatus("上级节点已保存：已同步画布中 " + std::to_string(refreshed) + " 个「" +
                             working.name + "」实例");
    }
    if (!refresh_error.empty()) {
        log::warn("[上级节点] 热重载失败: " + refresh_error);
    }

    model_.markSaved();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

} // namespace aiwrite::ui
