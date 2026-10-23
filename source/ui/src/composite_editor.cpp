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
// ARCH-A5：跨帧状态（上下文 / 改名缓冲 / 吸附开关 / 小地图 / 上一帧位置）全部收进
// ceUiState() 对象成员（定义见 ce_internal.h）；本 TU 不再有命名空间作用域可变变量。
// kCeSnap* 为本 TU 只读常量。


// ------------------------------------------------------------------ 手柄 ----
// 手柄映射（node_handle / link_handle / pin_handle / resolve_pin / PinRef）已上移
// canvas_view.{h,cpp}（CN_editor_refactor CEA-01：内部图 id 同为 "n1" 风格，与主画布
// 共用同一套整数手柄方案，杜绝映射逻辑漂移）。

// ------------------------------------------------------------------ 上下文 ----
void ensureCeContext()
{
    CeUiState& ui = ceUiState();
    if (ui.context != nullptr) {
        return;
    }
    ed::Config config;
    config.SettingsFile = nullptr; // 内部图编辑器不落盘设置（随会话丢弃）
    ui.context          = ed::CreateEditor(&config);
    ed::SetCurrentEditor(ui.context);
    // CEA-D5：上级节点编辑器画布应用「差异主题」（画布/节点背景偏蓝，与主画布一眼区分；
    // 样式按编辑器上下文隔离，不影响主画布）
    canvasApplyEditorStyle(canvasThemeComposite());
    ed::EnableShortcuts(false);
}

// ------------------------------------------------- G2/G6 内部节点改名 ----
// 重命名弹窗状态（跨帧存活）= ceUiState().rename_id / .rename_buf（ARCH-A5）

// 打开「重命名内部节点」弹窗（预填当前标题；须在 ed 上下文内调用）
void ceOpenRename(const std::string& node_id)
{
    CeUiState&          ui = ceUiState();
    ui.rename_id           = node_id;
    const engine::Node* node =
        compositeEditorState().working.internal_graph.findNode(node_id);
    const std::string& title = (node != nullptr) ? node->title : std::string();
    const std::size_t  n     = std::min<std::size_t>(title.size(), ui.rename_buf.size() - 1);
    title.copy(ui.rename_buf.data(), n);
    ui.rename_buf[n] = '\0';
    ImGui::OpenPopup("##ce_rename_menu");
}

// 应用改名（先压快照再改值；可撤销）
void ceApplyRename()
{
    CeUiState&            ui    = ceUiState();
    CompositeEditorState& state = compositeEditorState();
    engine::Node* node = state.working.internal_graph.findNode(ui.rename_id);
    const std::string  new_title(ui.rename_buf.data());
    if (node != nullptr && new_title != node->title) {
        compositeEditorSnapshot("重命名内部节点 " + ui.rename_id);
        node->title = new_title;
    }
    ui.rename_id.clear();
    ImGui::CloseCurrentPopup();
}

// ----------------------------------------------- G3 添加节点搜索/分类 ----
// 搜索过滤缓冲（打开菜单时清空）= ceUiState().add_filter（ARCH-A5）

// ------------------------------------------- G4/G5/G7 画布辅助（L3 移植）----
// 开关（小地图 / 对齐吸附 / 网格吸附 / 网格显示 / 适配视图请求）= ceUiState() 成员。
//   小地图输入屏蔽快照（blocking/clicked/down/mouse）= ceUiState().minimap（CLN-09）。

// 小地图（左下角缩略图）已上移 canvas_view::draw_minimap 共享（CLN-09），
// 上级编辑器在 ed::End 之后以 canvas_theme_composite() + 固定蓝紫着色回调调用（见 draw 循环尾）。

// ---- G5/G7：拖动中的对齐吸附（辅助线）+ 网格 16px 吸附 ----
constexpr float kCeSnapDistance = 8.0f;  // 对齐吸附距离（屏幕像素，换算画布坐标用当前缩放）
constexpr float kCeGridSize     = 16.0f; // 网格步长（画布坐标）

// 上一帧节点位置（判断「谁在拖动」：imgui-node-editor 无公开拖动查询 API）= ceUiState().prev_positions。
// CLN-10：拖拽判定（find_dragged_node）/ 对齐核心（apply_alignment_snap_core）/
//         辅助线（draw_snap_lines）已上移 canvas_view 单一来源，本 TU 保留
//         kCeSnapDistance / kCeGridSize / 网格吸附差异。
// 对齐吸附核心已上移 canvas_view::apply_alignment_snap_core 共享（CLN-10）：
// 三边对（左/右/中心X、上/下/中心Y）吸附逻辑与辅助线端点换算与主画布同源。
// 吸附入口（G5/G7）：找拖动中的节点 → 对齐吸附优先，未命中再就近吸附 16px 网格。
// 每次调用更新 prev_positions（供 find_dragged_node 下一帧判断）。
// CLN-10：调用共享 find_dragged_node / apply_alignment_snap_core；网格吸附为本 TU 独有差异。
SnapResult ceApplySnap(const engine::Graph& graph)
{
    CeUiState&  ui = ceUiState();
    SnapResult  result;
    if (!ui.align_snap && !ui.grid_snap) {
        ui.prev_positions.clear();
        return result;
    }
    const std::string dragged = find_dragged_node(graph, false, ui.prev_positions);
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
    if (!result.applied && ui.grid_snap) {
        const float gx = std::round(dpos.x / kCeGridSize) * kCeGridSize;
        const float gy = std::round(dpos.y / kCeGridSize) * kCeGridSize;
        if (std::fabs(gx - dpos.x) > 0.001f || std::fabs(gy - dpos.y) > 0.001f) {
            ed::SetNodePosition(nodeHandle(dragged), ImVec2(gx, gy));
            result.applied = true;
        }
    }

    for (const engine::Node& n : graph.nodes) {
        ui.prev_positions[n.id] = ed::GetNodePosition(nodeHandle(n.id));
    }
    return result;
}


} // namespace aiwrite::ui
