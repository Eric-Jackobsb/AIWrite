#include "composite_editor.h"
#include "ce_internal.h"
#include "composite.h"
#include "editor_controller.h"
#include "log.h"

#include <algorithm>
#include <array>
#include <string>

namespace aiwrite::ui {

// ------------------------------------------------------------ 会话操作 ----
void compositeEditorOpen(const std::string& composite_id)
{
    CompositeEditorState& state = compositeEditorState();
    const engine::CompositeSpec* existing =
        engine::CompositeRegistry::instance().find(composite_id);

    // PS-D9：内置官方预设只读 —— 拦截打开（须先「复制到我的节点库」）
    if (engine::CompositeRegistry::instance().is_builtin(composite_id)) {
        state.active = false;
        state.ask_close = false;
        state.error     = "官方预设为只读：请先在节点库「复制到我的节点库」再编辑";
        log::warn("[上级节点] 拒绝打开内置只读规格 " + composite_id + "（须先复制到用户源）");
        return;
    }

    state.active    = true;
    state.ask_close = false;
    state.error.clear();
    ceUiState().undo.clear();
    ceUiState().redo.clear();
    if (existing != nullptr) {
        state.working = *existing;
    }
    else {
        engine::CompositeSpec fresh;
        fresh.id = engine::CompositeRegistry::instance().makeId();
        state.working = std::move(fresh);
    }
    const auto copy_to = [](std::array<char, 256>& dst, const std::string& src) {
        const std::size_t n = std::min<std::size_t>(src.size(), dst.size() - 1);
        src.copy(dst.data(), n);
        dst[n] = '\0';
    };
    copy_to(state.name_buf, state.working.name);
    copy_to(state.desc_buf, state.working.description);
    state.dirty = false;
}

void compositeEditorRequestClose()
{
    CompositeEditorState& state = compositeEditorState();
    if (state.dirty) {
        state.ask_close = true;
    }
    else {
        compositeEditorForceClose();
    }
}

void compositeEditorForceClose()
{
    CompositeEditorState& state = compositeEditorState();
    compositeEditorStopRun(); // 试运行未结束则先 join（关闭窗口不能留后台线程）
    state.active    = false;
    state.ask_close = false;
    state.dirty     = false;
    state.error.clear();
    state.run_status.clear();
    state.run_snapshot = engine::RunSnapshot{};
    ceUiState().undo.clear();
    ceUiState().redo.clear();
}


bool compositeEditorSave(std::string* error, EditorController& main_editor)
{
    CompositeEditorState& state = compositeEditorState();
    // 同步名称/描述输入缓冲 → 工作副本（id 首次保存时才生成）
    state.working.name        = state.name_buf.data();
    state.working.description = state.desc_buf.data();
    if (state.working.id.empty()) {
        state.working.id = engine::CompositeRegistry::instance().makeId();
    }

    // 保存前的旧规格名（热重载时用于区分「实例标题是否被用户自定义过」）
    const engine::CompositeSpec* old_spec =
        engine::CompositeRegistry::instance().find(state.working.id);
    const std::string old_name = (old_spec != nullptr) ? old_spec->name : std::string();

    std::string save_error;
    if (!engine::CompositeRegistry::instance().save(state.working, &save_error)) {
        if (error != nullptr) {
            *error = save_error;
        }
        state.error = save_error;
        return false;
    }

    // 热重载：把主画布中已放置的同 id 实例按新规格重建（保留 id/位置/自定义标题/已填参数值）
    EditorController& main_state = main_editor;
    const bool   has_instances =
        std::any_of(main_state.model().graph.nodes.begin(), main_state.model().graph.nodes.end(),
                    [&](const engine::Node& n) { return n.composite_id == state.working.id; });
    if (has_instances) {
        // 改图前压主画布撤销快照（内部也会终止正在进行的运行）
        main_state.snapshot("热重载上级节点 " + state.working.name);
    }
    std::string refresh_error;
    const int   refreshed = engine::refresh_composite_instances(
        main_state.model().graph, state.working.id, old_name, &refresh_error);
    if (refreshed > 0) {
        // 清理可能已失效的连线选中引用（节点 id 保留，选中节点不受影响）
        // ARCH-E4：选中独立对象 —— prune_links 移除失效引用（内置变化检测）
        main_state.selection().prune_links([&](const std::string& eid) {
            return main_state.model().graph.findEdge(eid) != nullptr;
        });
        main_state.setStatus("上级节点已保存：已同步画布中 " + std::to_string(refreshed) +
                              " 个「" + state.working.name + "」实例");
    }
    if (!refresh_error.empty()) {
        log::warn("[上级节点] 热重载失败: " + refresh_error);
    }

    state.dirty = false;
    state.error.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void compositeEditorSnapshot(const std::string& label)
{
    cePushSnapshot(label);
}

// 编辑会话内创建内部节点（落点 = 调用方传入的画布坐标 x/y；UI 层保证为背景右键位置）
std::string compositeEditorAddNode(const std::string& type, float x, float y)
{
    CompositeEditorState& state = compositeEditorState();
    cePushSnapshot("添加内部节点 " + type);

    std::string error;
    const std::string id = state.working.internal_graph.addNode(type, x, y, &error);
    if (id.empty()) {
        log::error("[上级节点] 添加内部节点失败: " + error);
        state.error = error;
        return {};
    }
    return id;
}

// 删除内部节点（不压快照）：移除节点 + 清理暴露引用残留；快照由调用方决定
// （右键菜单 / Delete 键批量删除复用，保证「一次删除批次 = 一次撤销快照」）
bool ceRemoveNode(CompositeEditorState& state, const std::string& node_id)
{
    if (!state.working.internal_graph.removeNode(node_id)) {
        return false;
    }
    // 清除该节点在暴露引用中的残留
    const auto drop = [&node_id](std::vector<engine::CompositePortRef>& refs) {
        refs.erase(std::remove_if(refs.begin(), refs.end(),
                                  [&node_id](const engine::CompositePortRef& r) {
                                      return r.internal_node == node_id;
                                  }),
                   refs.end());
    };
    drop(state.working.inputs);
    drop(state.working.outputs);
    state.working.exposed_params.erase(
        std::remove_if(state.working.exposed_params.begin(), state.working.exposed_params.end(),
                       [&node_id](const engine::CompositeParamRef& r) {
                           return r.internal_node == node_id;
                       }),
        state.working.exposed_params.end());
    return true;
}

bool compositeEditorDeleteNode(const std::string& node_id)
{
    CompositeEditorState& state = compositeEditorState();
    cePushSnapshot("删除内部节点 " + node_id);
    return ceRemoveNode(state, node_id);
}

std::string composite_editor_add_edge(const std::string& from_node, const std::string& from_port,
                                      const std::string& to_node, const std::string& to_port,
                                      std::string* error)
{
    CompositeEditorState& state = compositeEditorState();
    std::string reason;
    if (!state.working.internal_graph.canConnect(from_node, from_port, to_node, to_port, &reason)) {
        if (error != nullptr) {
            *error = reason;
        }
        return {};
    }
    cePushSnapshot("添加内部连线");
    const std::string edge_id =
        state.working.internal_graph.addEdge(from_node, from_port, to_node, to_port, error);
    return edge_id;
}


} // namespace aiwrite::ui
