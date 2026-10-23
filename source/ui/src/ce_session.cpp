#include "ce_session.h"

#include "ce_internal.h"
#include "composite_editor.h"
#include "editor_controller.h"
#include "log.h"

#include <algorithm>
#include <array>
#include <string>

namespace aiwrite::ui {

// ============================================================ CeSession ====
// ARCH-Z2：`CeSession` 对称拆分 —— 会话级门面（原 `CompositeEditorState` 的会话字段）。
//  * 会话编排（打开 / 关闭 / 保存 / 缓冲同步）留在本对象
//  * 规格工作副本 + 撤销栈 → CeModel；规格标记 → CeSpec；选中 → CeSelection；文件 → CeFile
CeSession& ceSession()
{
    static CeSession session;
    return session;
}

void CeSession::open(const std::string& composite_id)
{
    std::string open_error;
    if (!file_.open(composite_id, &open_error)) {
        active    = false;
        ask_close = false;
        error     = open_error;
        return;
    }

    active    = true;
    ask_close = false;
    error.clear();
    model_.clearHistory();
    selection_.clear();
    const auto copy_to = [](std::array<char, 256>& dst, const std::string& src) {
        const std::size_t n = std::min<std::size_t>(src.size(), dst.size() - 1);
        src.copy(dst.data(), n);
        dst[n] = '\0';
    };
    copy_to(name_buf, model_.spec().name);
    copy_to(desc_buf, model_.spec().description);
    model_.markSaved();
}

void CeSession::requestClose()
{
    if (model_.dirty()) {
        ask_close = true;
    }
    else {
        forceClose();
    }
}

void CeSession::forceClose()
{
    compositeEditorStopRun(); // 试运行未结束则先 join（关闭窗口不能留后台线程）
    active    = false;
    ask_close = false;
    error.clear();
    run_status.clear();
    run_snapshot = engine::RunSnapshot{};
    model_.markSaved();
    model_.clearHistory();
    selection_.clear();
    spec_.reset_name_buffers();
}

bool CeSession::save(std::string* error_out, EditorController& main_editor)
{
    syncNameBuffers();
    std::string save_error;
    if (!file_.save(name_buf.data(), desc_buf.data(), &save_error, main_editor)) {
        if (error_out != nullptr) {
            *error_out = save_error;
        }
        this->error = save_error;
        return false;
    }
    this->error.clear();
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

// --------------------------------------------------------- 对外 API 转发 ----
// 对外签名保持不变（composite_editor.h）；实现全部转发到会话对象。
void compositeEditorOpen(const std::string& composite_id)
{
    ceSession().open(composite_id);
}

void compositeEditorRequestClose()
{
    ceSession().requestClose();
}

void compositeEditorForceClose()
{
    ceSession().forceClose();
}

bool compositeEditorSave(std::string* error, EditorController& main_editor)
{
    return ceSession().save(error, main_editor);
}

void CeSession::syncNameBuffers()
{
    // 名称/描述输入缓冲 → 工作副本（结构类修改才进撤销栈，同原语义）
    model_.spec().name        = name_buf.data();
    model_.spec().description = desc_buf.data();
}

// ------------------------------------------------------------ 编辑 API ----
// 内部图单点编辑（对称 WorkflowModel 的 createNode / deleteNode）：
//  * 压快照策略与原实现一致：addNode / deleteNode / add_edge 各自压一次；
//    ceRemoveNode **不压**（调用方按批次压一次：右键菜单 / Delete 键批量删除）
void compositeEditorSnapshot(const std::string& label)
{
    ceSession().snapshot(label);
}

// 编辑会话内创建内部节点（落点 = 调用方传入的画布坐标 x/y；UI 层保证为背景右键位置）
std::string compositeEditorAddNode(const std::string& type, float x, float y)
{
    CeSession& session = ceSession();
    session.snapshot("添加内部节点 " + type);

    std::string       error;
    const std::string id = session.model().graph().addNode(type, x, y, &error);
    if (id.empty()) {
        log::error("[上级节点] 添加内部节点失败: " + error);
        session.error = error;
        return {};
    }
    return id;
}

// 删除内部节点（不压快照）：移除节点 + 清理暴露引用残留；快照由调用方决定
// （右键菜单 / Delete 键批量删除复用，保证「一次删除批次 = 一次撤销快照」）
bool ceRemoveNode(const std::string& node_id)
{
    CeSession& session = ceSession();
    if (!session.model().graph().removeNode(node_id)) {
        return false;
    }
    session.spec_marks().drop_node_refs(node_id); // 输入 / 输出 / 暴露参数三处残留
    if (session.selection().is_selected(node_id)) {
        session.selection().clear(); // 被删节点不再保持选中（参数编辑区回落「未选中」）
    }
    return true;
}

bool compositeEditorDeleteNode(const std::string& node_id)
{
    ceSession().snapshot("删除内部节点 " + node_id);
    return ceRemoveNode(node_id);
}

std::string composite_editor_add_edge(const std::string& from_node, const std::string& from_port,
                                      const std::string& to_node, const std::string& to_port,
                                      std::string* error)
{
    CeSession&  session = ceSession();
    std::string reason;
    if (!session.model().graph().canConnect(from_node, from_port, to_node, to_port, &reason)) {
        if (error != nullptr) {
            *error = reason;
        }
        return {};
    }
    session.snapshot("添加内部连线");
    return session.model().graph().addEdge(from_node, from_port, to_node, to_port, error);
}

} // namespace aiwrite::ui
