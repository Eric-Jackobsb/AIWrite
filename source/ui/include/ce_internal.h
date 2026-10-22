#pragma once

// ============================================================================
//  上级节点编辑器内部共享声明（CLN-13 拆分）
//
//  * 仅供 composite_editor.cpp / ce_state.cpp / ce_session.cpp / ce_run.cpp /
//    ce_pack.cpp / ce_draw.cpp 内部使用；不进公共头 composite_editor.h
//    （对外 API 与链接符号保持不变，ce_* 短前缀仍模块私有）。
//  * 定义方：画布域（composite_editor.cpp）· 撤销域（ce_state.cpp）·
//    会话域（ce_session.cpp）；其余 TU 经此头 extern 声明访问。
// ============================================================================

#include <array>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_node_editor.h>

#include "canvas_view.h"      // MinimapState / SnapResult
#include "composite.h"        // engine::CompositeSpec
#include "composite_editor.h" // CompositeEditorState

namespace aiwrite::ui {

// ---- 画布域全局（定义于 composite_editor.cpp；ce_draw.cpp 跨 TU 访问）----
extern ax::NodeEditor::EditorContext* g_ce_context;
extern std::string                    g_ce_rename_id;
extern std::array<char, 128>          g_ce_rename_buf;
extern char                           g_ce_add_filter[64];
extern bool                           g_ce_show_minimap;
extern bool                           g_ce_align_snap;
extern bool                           g_ce_grid_snap;
extern bool                           g_ce_show_grid;
extern bool                           g_ce_request_fit;
extern MinimapState                   g_ce_minimap;

// ---- 画布域函数（定义于 composite_editor.cpp；ce_draw.cpp 跨 TU 调用）----
void ensureCeContext();
void ceOpenRename(const std::string& node_id);
void ceApplyRename();
SnapResult ceApplySnap(const engine::Graph& graph);

// ---- 撤销域（定义于 ce_state.cpp；ce_session/ce_draw 跨 TU 调用）----
struct CeUndoEntry {
    engine::CompositeSpec spec;
    std::string           label;
};
extern std::vector<CeUndoEntry> g_ce_undo;
extern std::vector<CeUndoEntry> g_ce_redo;
void cePushSnapshot(const std::string& label);

// ---- 会话域（定义于 ce_session.cpp；ce_draw 跨 TU 调用）----
bool ceRemoveNode(CompositeEditorState& state, const std::string& node_id);

} // namespace aiwrite::ui
