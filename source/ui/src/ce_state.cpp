#include "composite_editor.h"
#include "ce_internal.h"

#include <string>
#include <vector>

namespace aiwrite::ui {

CompositeEditorState& compositeEditorState()
{
    static CompositeEditorState state;
    return state;
}

// ---- 跨帧状态容器（ARCH-A5：原 g_ce_* 全局的唯一定义点）----
// 规则 6：跨帧可变状态必须是对象成员 —— 撤销栈 / 画布状态全部落在 CeUiState 上。
// 这里的函数级 static 就是承载它们的对象；命名空间作用域不再有可变变量。
CeUiState& ceUiState()
{
    static CeUiState state;
    return state;
}

// ---- 本地撤销（快照整份 CompositeSpec，深度 30）----
// 撤销栈挂 ceUiState().undo / .redo（ce_session.cpp 的 open / force_close 清栈）。
namespace {
constexpr std::size_t kCeUndoDepth = 30;
} // namespace

// cePushSnapshot（跨 TU：ce_session / ce_draw 调用；ce_internal.h 声明）
void cePushSnapshot(const std::string& label)
{
    CeUiState& ui = ceUiState();
    CeUndoEntry entry;
    entry.spec  = compositeEditorState().working;
    entry.label = label;
    ui.undo.push_back(std::move(entry));
    if (ui.undo.size() > kCeUndoDepth) {
        ui.undo.erase(ui.undo.begin());
    }
    ui.redo.clear();
    compositeEditorState().dirty = true;
}

namespace {

void ce_undo()
{
    CeUiState&            ui    = ceUiState();
    CompositeEditorState& state = compositeEditorState();
    if (ui.undo.empty()) {
        return;
    }
    CeUndoEntry entry = ui.undo.back();
    ui.undo.pop_back();

    CeUndoEntry current;
    current.spec  = state.working;
    current.label = entry.label;
    ui.redo.push_back(std::move(current));

    state.working = std::move(entry.spec);
    state.error.clear();
}

void ce_redo()
{
    CeUiState&            ui    = ceUiState();
    CompositeEditorState& state = compositeEditorState();
    if (ui.redo.empty()) {
        return;
    }
    CeUndoEntry entry = ui.redo.back();
    ui.redo.pop_back();

    CeUndoEntry current;
    current.spec  = state.working;
    current.label = entry.label;
    ui.undo.push_back(std::move(current));

    state.working = std::move(entry.spec);
    state.error.clear();
}

} // namespace

// 编辑器内撤销/重做（供窗口按钮调用）
bool compositeEditorCanUndo() { return !ceUiState().undo.empty(); }
bool compositeEditorCanRedo() { return !ceUiState().redo.empty(); }
void compositeEditorUndoOnce() { ce_undo(); }
void compositeEditorRedoOnce() { ce_redo(); }

} // namespace aiwrite::ui
