#include "composite_editor.h"

#include "canvas_view.h"   // CN_editor_refactor：共享只读绘制层（CEA-01 / CEA-02）
#include "composite.h"
#include "editor_controller.h"
#include "log.h"
#include "nodes.h" // 试运行：registerAllExecutors
#include "node_registry.h"
#include "paths.h"
#include "property_panel.h" // PS-09-G1：复用公共参数控件 draw_param_widget
#include "string_util.h"   // CLN-05：findCi 收敛
#include "theme.h"
#include "validate.h" // 试运行：运行前校验 validateBeforeRun

#include <imgui.h>
#include <imgui_node_editor.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "ce_internal.h"

namespace aiwrite::ui {
namespace ed = ax::NodeEditor;

// ---------------------------------------------------------------- 画布域 ----
// 上级编辑器上下文 / 改名 / 画布辅助 / 对齐吸附（CLN-13 拆分后本 TU 保留部分）。
// 跨 TU 成员（g_ce_* / ensure_ce_context / ce_open_rename / ce_apply_rename /
// ce_apply_snap）经 ce_internal.h 与 ce_state/ce_session/ce_run/ce_pack/ce_draw 共享；
// kCeSnap* / g_ce_prev_positions 仅本 TU 可见。


// ------------------------------------------------------------------ 手柄 ----
// 手柄映射（node_handle / link_handle / pin_handle / resolve_pin / PinRef）已上移
// canvas_view.{h,cpp}（CN_editor_refactor CEA-01：内部图 id 同为 "n1" 风格，与主画布
// 共用同一套整数手柄方案，杜绝映射逻辑漂移）。

// ------------------------------------------------------------------ 上下文 ----
ed::EditorContext* g_ce_context = nullptr;

void ensureCeContext()
{
    if (g_ce_context != nullptr) {
        return;
    }
    ed::Config config;
    config.SettingsFile = nullptr; // 内部图编辑器不落盘设置（随会话丢弃）
    g_ce_context        = ed::CreateEditor(&config);
    ed::SetCurrentEditor(g_ce_context);
    // CEA-D5：上级节点编辑器画布应用「差异主题」（画布/节点背景偏蓝，与主画布一眼区分；
    // 样式按编辑器上下文隔离，不影响主画布）
    canvasApplyEditorStyle(canvasThemeComposite());
    ed::EnableShortcuts(false);
}

// ------------------------------------------------- G2/G6 内部节点改名 ----
// 重命名弹窗状态（跨帧存活）
std::string              g_ce_rename_id;
std::array<char, 128>    g_ce_rename_buf;

// 打开「重命名内部节点」弹窗（预填当前标题；须在 ed 上下文内调用）
void ceOpenRename(const std::string& node_id)
{
    g_ce_rename_id = node_id;
    const engine::Node* node =
        compositeEditorState().working.internal_graph.findNode(node_id);
    const std::string& title = (node != nullptr) ? node->title : std::string();
    const std::size_t  n     = std::min<std::size_t>(title.size(), g_ce_rename_buf.size() - 1);
    title.copy(g_ce_rename_buf.data(), n);
    g_ce_rename_buf[n] = '\0';
    ImGui::OpenPopup("##ce_rename_menu");
}

// 应用改名（先压快照再改值；可撤销）
void ceApplyRename()
{
    CompositeEditorState& state = compositeEditorState();
    engine::Node* node = state.working.internal_graph.findNode(g_ce_rename_id);
    const std::string  new_title(g_ce_rename_buf.data());
    if (node != nullptr && new_title != node->title) {
        compositeEditorSnapshot("重命名内部节点 " + g_ce_rename_id);
        node->title = new_title;
    }
    g_ce_rename_id.clear();
    ImGui::CloseCurrentPopup();
}

// ----------------------------------------------- G3 添加节点搜索/分类 ----
// 搜索过滤缓冲（打开菜单时清空）
char g_ce_add_filter[64] = "";

// ------------------------------------------- G4/G5/G7 画布辅助（L3 移植）----
bool g_ce_show_minimap = true;  // G4：小地图（左下角）
bool g_ce_align_snap   = true;  // G5：对齐吸附 + 辅助线
bool g_ce_grid_snap    = true;  // G7：网格 16px 吸附
bool g_ce_show_grid    = true;  // G7：网格显示
bool g_ce_request_fit  = false; // G8：「适配视图」请求（ed::Begin 内执行 NavigateToContent）

// 小地图输入屏蔽快照（鼠标进入小地图区域 → 事件只归小地图，画布不得参与）。
// CLN-09：几何 / 输入屏蔽 / 绘制已上移 canvas_view 单一来源，本 TU 只留开关与状态。
MinimapState g_ce_minimap; // CLN-09：输入屏蔽快照（blocking/clicked/down/mouse）

// 小地图（左下角缩略图）已上移 canvas_view::draw_minimap 共享（CLN-09），
// 上级编辑器在 ed::End 之后以 canvas_theme_composite() + 固定蓝紫着色回调调用（见 draw 循环尾）。

// ---- G5/G7：拖动中的对齐吸附（辅助线）+ 网格 16px 吸附 ----
constexpr float kCeSnapDistance = 8.0f;  // 对齐吸附距离（屏幕像素，换算画布坐标用当前缩放）
constexpr float kCeGridSize     = 16.0f; // 网格步长（画布坐标）

// 上一帧节点位置（判断「谁在拖动」：imgui-node-editor 无公开拖动查询 API）。
// CLN-10：拖拽判定（find_dragged_node）/ 对齐核心（apply_alignment_snap_core）/
//         辅助线（draw_snap_lines）已上移 canvas_view 单一来源，本 TU 保留
//         kCeSnapDistance / kCeGridSize / 网格吸附差异。
std::unordered_map<std::string, ImVec2> g_ce_prev_positions;
// 对齐吸附核心已上移 canvas_view::apply_alignment_snap_core 共享（CLN-10）：
// 三边对（左/右/中心X、上/下/中心Y）吸附逻辑与辅助线端点换算与主画布同源。
// 吸附入口（G5/G7）：找拖动中的节点 → 对齐吸附优先，未命中再就近吸附 16px 网格。
// 每次调用更新 g_ce_prev_positions（供 find_dragged_node 下一帧判断）。
// CLN-10：调用共享 find_dragged_node / apply_alignment_snap_core；网格吸附为本 TU 独有差异。
SnapResult ceApplySnap(const engine::Graph& graph)
{
    SnapResult result;
    if (!g_ce_align_snap && !g_ce_grid_snap) {
        g_ce_prev_positions.clear();
        return result;
    }
    const std::string dragged = find_dragged_node(graph, false, g_ce_prev_positions);
    if (dragged.empty()) {
        return result;
    }
    const engine::Node* node = graph.findNode(dragged);
    if (node == nullptr) {
        return result;
    }

    const float zoom        = ed::GetCurrentZoom();
    const float snap_canvas = (zoom > 1e-6f) ? kCeSnapDistance / zoom : kCeSnapDistance;

    const ImVec2 dpos  = ed::GetNodePosition(nodeHandle(dragged));
    const ImVec2 dsize = ed::GetNodeSize(nodeHandle(dragged));
    const float  dw    = dsize.x > 1.0f ? dsize.x : 80.0f;
    const float  dh    = dsize.y > 1.0f ? dsize.y : 40.0f;

    // ① 对齐吸附（G5）：命中即应用（辅助线一并产出）
    result = apply_alignment_snap_core(graph, dragged, dpos, dw, dh, snap_canvas);

    // ② 网格 16px 吸附（G7）：未命中对齐时才生效，避免两种吸附互相拉扯
    if (!result.applied && g_ce_grid_snap) {
        const float gx = std::round(dpos.x / kCeGridSize) * kCeGridSize;
        const float gy = std::round(dpos.y / kCeGridSize) * kCeGridSize;
        if (std::fabs(gx - dpos.x) > 0.001f || std::fabs(gy - dpos.y) > 0.001f) {
            ed::SetNodePosition(nodeHandle(dragged), ImVec2(gx, gy));
            result.applied = true;
        }
    }

    for (const engine::Node& n : graph.nodes) {
        g_ce_prev_positions[n.id] = ed::GetNodePosition(nodeHandle(n.id));
    }
    return result;
}


} // namespace aiwrite::ui
