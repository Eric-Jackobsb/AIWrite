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

// ---- 本地撤销（快照整份 CompositeSpec，深度 30）----
// 撤销栈 / CeUndoEntry 跨 TU 共享（ce_session.cpp 的 open / force_close 清栈）；
// CeUndoEntry 结构定义于 ce_internal.h，本文件提供 g_ce_undo / g_ce_redo 唯一定义。
std::vector<CeUndoEntry> g_ce_undo;
std::vector<CeUndoEntry> g_ce_redo;

namespace {
constexpr std::size_t kCeUndoDepth = 30;
} // namespace

// ce_push_snapshot（跨 TU：ce_session / ce_draw 调用；ce_internal.h 声明）
void cePushSnapshot(const std::string& label)
{
    CeUndoEntry entry;
    entry.spec  = compositeEditorState().working;
    entry.label = label;
    g_ce_undo.push_back(std::move(entry));
    if (g_ce_undo.size() > kCeUndoDepth) {
        g_ce_undo.erase(g_ce_undo.begin());
    }
    g_ce_redo.clear();
    compositeEditorState().dirty = true;
}

namespace {

void ce_undo()
{
    CompositeEditorState& state = compositeEditorState();
    if (g_ce_undo.empty()) {
        return;
    }
    CeUndoEntry entry = g_ce_undo.back();
    g_ce_undo.pop_back();

    CeUndoEntry current;
    current.spec  = state.working;
    current.label = entry.label;
    g_ce_redo.push_back(std::move(current));

    state.working = std::move(entry.spec);
    state.error.clear();
}

void ce_redo()
{
    CompositeEditorState& state = compositeEditorState();
    if (g_ce_redo.empty()) {
        return;
    }
    CeUndoEntry entry = g_ce_redo.back();
    g_ce_redo.pop_back();

    CeUndoEntry current;
    current.spec  = state.working;
    current.label = entry.label;
    g_ce_undo.push_back(std::move(current));

    state.working = std::move(entry.spec);
    state.error.clear();
}

} // namespace

// 编辑器内撤销/重做（供窗口按钮调用）
bool compositeEditorCanUndo() { return !g_ce_undo.empty(); }
bool compositeEditorCanRedo() { return !g_ce_redo.empty(); }
void compositeEditorUndoOnce() { ce_undo(); }
void compositeEditorRedoOnce() { ce_redo(); }

} // namespace aiwrite::ui
