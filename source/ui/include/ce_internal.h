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
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <imgui_node_editor.h>

#include "canvas_view.h"      // MinimapState / SnapResult
#include "composite.h"        // engine::CompositeSpec
#include "composite_editor.h" // CompositeEditorState

namespace aiwrite::ui {

// ---- 跨帧状态（ARCH-A5：原 12 条跨 TU extern 全部收进本对象，R1 归零）----
//  * 唯一状态容器 = `ceUiState()`（定义于 ce_state.cpp，唯一定义点；函数内 static ⇒
//    对象成员持有状态，符合 §1.1 规则 6「跨帧可变状态必须是某个对象成员」）。
//  * 各域（画布 / 改名 / 吸附 / 小地图 / 撤销）只读写成员，**禁止**再引入文件级全局。
struct CeUndoEntry {                       // 撤销快照（整份 CompositeSpec + 展示标签）
    engine::CompositeSpec spec;
    std::string           label;
};

struct CeUiState {
    // 画布域（原 composite_editor.cpp 的 g_ce_* 定义）
    ax::NodeEditor::EditorContext* context = nullptr;   // 内部图编辑器上下文（惰性创建）
    std::string                    rename_id;           // 重命名目标节点 id
    std::array<char, 128>          rename_buf{};        // 重命名输入缓冲
    char                           add_filter[64] = {}; // G3：添加节点搜索缓冲
    bool show_minimap = true;                           // G4：小地图（左下角）
    bool align_snap   = true;                           // G5：对齐吸附 + 辅助线
    bool grid_snap    = true;                           // G7：网格 16px 吸附
    bool show_grid    = true;                           // G7：网格显示
    bool request_fit  = false;                          // G8：适配视图请求
    MinimapState                   minimap;             // CLN-09：小地图输入屏蔽快照
    std::unordered_map<std::string, ImVec2> prev_positions; // 上一帧节点位置（拖拽判定）
    bool window_focused = false;                        // ce 窗口聚焦/悬停（ARCH-R4 Ctrl+S 作用域）

    // 撤销域（原 ce_state.cpp 的 g_ce_undo / g_ce_redo 定义）
    std::vector<CeUndoEntry> undo;
    std::vector<CeUndoEntry> redo;
};

CeUiState& ceUiState();

// ---- 画布域函数（定义于 composite_editor.cpp；ce_draw.cpp 跨 TU 调用）----
void ensureCeContext();
void ceOpenRename(const std::string& node_id);
void ceApplyRename();
SnapResult ceApplySnap(const engine::Graph& graph);

// ---- 撤销域（定义于 ce_state.cpp；ce_session/ce_draw 跨 TU 调用）----
void cePushSnapshot(const std::string& label);

// ---- 会话域（定义于 ce_session.cpp；ce_draw 跨 TU 调用）----
bool ceRemoveNode(CompositeEditorState& state, const std::string& node_id);

} // namespace aiwrite::ui
