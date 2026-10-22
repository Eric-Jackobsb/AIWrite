#include "composite_editor.h"

#include "composite.h"
#include "editor_state.h"
#include "log.h"
#include "node_registry.h"
#include "paths.h"
#include "property_panel.h" // PS-09-G1：复用公共参数控件 draw_param_widget
#include "theme.h"

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

namespace aiwrite::ui {
namespace ed = ax::NodeEditor;

namespace {

// ------------------------------------------------------------------ 手柄 ----
// 内部图节点 id 为 "n1" 风格 ⇒ 与主画布共用同一套整数手柄方案（见 node_canvas.cpp）
ed::NodeId ce_node_handle(const std::string& node_id)
{
    return ed::NodeId(static_cast<uintptr_t>(engine::numericIdOf(node_id)));
}

ed::LinkId ce_link_handle(const std::string& edge_id)
{
    return ed::LinkId(static_cast<uintptr_t>(engine::numericIdOf(edge_id)));
}

ed::PinId ce_pin_handle(const engine::Node& node, engine::PortDirection direction, int index)
{
    const uintptr_t base  = static_cast<uintptr_t>(engine::numericIdOf(node.id)) << 8;
    const uintptr_t flags = (direction == engine::PortDirection::Output) ? 0x80u : 0x00u;
    return ed::PinId(base | flags | static_cast<uintptr_t>(index & 0x7F));
}

struct CePinRef {
    std::string             node_id;
    std::string             port_id;
    engine::PortDirection   direction = engine::PortDirection::Input;
    bool                    valid     = false;
};

CePinRef ce_resolve_pin(ed::PinId id, const engine::Graph& graph)
{
    CePinRef ref;
    const uintptr_t value = id.Get();
    if (value == 0) {
        return ref;
    }
    const int node_seq = static_cast<int>((value >> 8) & 0x7FFFFF);
    const engine::PortDirection direction =
        ((value & 0x80u) != 0) ? engine::PortDirection::Output : engine::PortDirection::Input;
    const int index = static_cast<int>(value & 0x7F);

    const engine::Node* node = graph.findNode("n" + std::to_string(node_seq));
    if (node == nullptr) {
        return ref;
    }
    const std::vector<engine::Port>& ports =
        (direction == engine::PortDirection::Input) ? node->inputs : node->outputs;
    if (index < 0 || index >= static_cast<int>(ports.size())) {
        return ref;
    }

    ref.node_id   = node->id;
    ref.port_id   = ports[static_cast<std::size_t>(index)].id;
    ref.direction = direction;
    ref.valid     = true;
    return ref;
}

// ------------------------------------------------------------------ 上下文 ----
ed::EditorContext* g_ce_context = nullptr;

void ensure_ce_context()
{
    if (g_ce_context != nullptr) {
        return;
    }
    ed::Config config;
    config.SettingsFile = nullptr; // 内部图编辑器不落盘设置（随会话丢弃）
    g_ce_context        = ed::CreateEditor(&config);
    ed::SetCurrentEditor(g_ce_context);
    ed::EnableShortcuts(false);
}

// 节点卡片（简化版：标题 + 输入/输出端口圆点；不显示运行态/参数预览）
void ce_draw_node(const engine::Node& node)
{
    ed::BeginNode(ce_node_handle(node.id));
    ImGui::TextUnformatted(node.title.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", node.id.c_str());
    ImGui::Dummy(ImVec2(160.0f, 1.0f));
    ImGui::Separator();

    int index = 0;
    for (const engine::Port& port : node.inputs) {
        ed::BeginPin(ce_pin_handle(node, engine::PortDirection::Input, index),
                     ed::PinKind::Input);
        ImGui::TextUnformatted(port.display_name.c_str());
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(6.0f, 6.0f));
        ed::EndPin();
        ++index;
    }
    for (const engine::Port& port : node.outputs) {
        ed::BeginPin(ce_pin_handle(node, engine::PortDirection::Output, index),
                     ed::PinKind::Output);
        ImGui::Dummy(ImVec2(6.0f, 6.0f));
        ImGui::SameLine();
        ImGui::TextUnformatted(port.display_name.c_str());
        ed::EndPin();
        ++index;
    }

    ed::EndNode();
}

// ------------------------------------------------- G2/G6 内部节点改名 ----
// 重命名弹窗状态（跨帧存活）
std::string              g_ce_rename_id;
std::array<char, 128>    g_ce_rename_buf;

// 打开「重命名内部节点」弹窗（预填当前标题；须在 ed 上下文内调用）
void ce_open_rename(const std::string& node_id)
{
    g_ce_rename_id = node_id;
    const engine::Node* node =
        composite_editor_state().working.internal_graph.findNode(node_id);
    const std::string& title = (node != nullptr) ? node->title : std::string();
    const std::size_t  n     = std::min<std::size_t>(title.size(), g_ce_rename_buf.size() - 1);
    title.copy(g_ce_rename_buf.data(), n);
    g_ce_rename_buf[n] = '\0';
    ImGui::OpenPopup("##ce_rename_menu");
}

// 应用改名（先压快照再改值；可撤销）
void ce_apply_rename()
{
    CompositeEditorState& state = composite_editor_state();
    engine::Node* node = state.working.internal_graph.findNode(g_ce_rename_id);
    const std::string  new_title(g_ce_rename_buf.data());
    if (node != nullptr && new_title != node->title) {
        composite_editor_snapshot("重命名内部节点 " + g_ce_rename_id);
        node->title = new_title;
    }
    g_ce_rename_id.clear();
    ImGui::CloseCurrentPopup();
}

// ----------------------------------------------- G3 添加节点搜索/分类 ----
// 大小写不敏感子串匹配（ASCII 折叠；中文原样字节比较；与节点库 find_ci 同语义）
std::size_t ce_find_ci(const std::string& haystack, const std::string& needle)
{
    if (needle.empty()) {
        return 0;
    }
    if (needle.size() > haystack.size()) {
        return std::string::npos;
    }
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (lower(haystack[i + j]) != lower(needle[j])) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return i;
        }
    }
    return std::string::npos;
}

// 「添加内部节点」菜单的搜索过滤缓冲（打开菜单时清空）
char g_ce_add_filter[64] = "";

// ------------------------------------------- G4/G5/G7 画布辅助（L3 移植）----
bool g_ce_show_minimap = true;  // G4：小地图（左下角）
bool g_ce_align_snap   = true;  // G5：对齐吸附 + 辅助线
bool g_ce_grid_snap    = true;  // G7：网格 16px 吸附
bool g_ce_show_grid    = true;  // G7：网格显示
bool g_ce_request_fit  = false; // G8：「适配视图」请求（ed::Begin 内执行 NavigateToContent）

// 小地图输入屏蔽快照（鼠标进入小地图区域 → 事件只归小地图，画布不得参与）
bool   g_ce_minimap_blocking = false;
bool   g_ce_minimap_clicked  = false;
bool   g_ce_minimap_down     = false;
ImVec2 g_ce_minimap_mouse    = ImVec2(0.0f, 0.0f);

void ce_minimap_rect(ImVec2& out_min, ImVec2& out_max)
{
    const ImVec2 window_pos  = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    const float  margin      = 10.0f;
    out_min = ImVec2(window_pos.x + window_size.x - 190.0f - margin,
                     window_pos.y + window_size.y - 130.0f - margin);
    out_max = ImVec2(out_min.x + 190.0f, out_min.y + 130.0f);
}

bool ce_minimap_hit(const ImVec2& mouse, const ImVec2& min, const ImVec2& max)
{
    return mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
}

// 在 ed::Begin 之前调用（主画布 L3 同款：先快照、再清空，保证小地图拿到完整输入）
void ce_block_minimap_input()
{
    g_ce_minimap_blocking = false;
    g_ce_minimap_clicked  = false;
    g_ce_minimap_down     = false;
    if (!g_ce_show_minimap) {
        return;
    }
    ImVec2 map_min, map_max;
    ce_minimap_rect(map_min, map_max);
    ImGuiIO&     io    = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    if (!ce_minimap_hit(mouse, map_min, map_max)) {
        return;
    }
    g_ce_minimap_blocking = true;
    g_ce_minimap_mouse    = mouse;
    g_ce_minimap_clicked  = io.MouseClicked[0];
    g_ce_minimap_down     = io.MouseDown[0];
    for (int i = 0; i < 5; ++i) {
        io.MouseClicked[i]  = false;
        io.MouseDown[i]     = false;
        io.MouseReleased[i] = false;
    }
    io.MouseWheel  = 0.0f;
    io.MouseWheelH = 0.0f;
    io.MouseDelta  = ImVec2(0.0f, 0.0f);
}

// 小地图（左下角缩略图；点击 / 按住拖动 → 视口中心平移到对应画布位置；右上角 ✕ 关闭）
// 纯导航：绝不选中 / 移动节点 —— 输入已在 ce_block_minimap_input 中与画布隔离。
// 在 ed::Begin 内、ed::End 前调用（需当前编辑器上下文：GetNodeSize / NavigateToCenter）。
void ce_draw_minimap(const engine::Graph& graph)
{
    if (!g_ce_show_minimap) {
        return;
    }
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    ImVec2 map_min, map_max;
    ce_minimap_rect(map_min, map_max);
    draw_list->AddRectFilled(map_min, map_max, IM_COL32(18, 20, 26, 215), 4.0f);
    draw_list->AddRect(map_min, map_max, IM_COL32(130, 130, 145, 210), 4.0f, 0, 1.0f);
    draw_list->AddText(map_min + ImVec2(6.0f, 3.0f), IM_COL32(200, 200, 210, 235), "小地图");

    const ImVec2 close_pos(map_max.x - 18.0f, map_min.y + 2.0f);
    if (g_ce_minimap_blocking && g_ce_minimap_clicked && g_ce_minimap_mouse.x >= close_pos.x &&
        g_ce_minimap_mouse.x <= close_pos.x + 14.0f && g_ce_minimap_mouse.y >= close_pos.y &&
        g_ce_minimap_mouse.y <= close_pos.y + 14.0f) {
        g_ce_show_minimap = false;
        return;
    }
    draw_list->AddText(close_pos, IM_COL32(225, 90, 90, 235), "✕");

    const ImVec2 area_min(map_min.x + 6.0f, map_min.y + 20.0f);
    const ImVec2 area_max(map_max.x - 6.0f, map_max.y - 6.0f);
    if (graph.nodes.empty()) {
        draw_list->AddText(area_min, IM_COL32(120, 120, 135, 200), "空画布");
        return;
    }

    ImVec2 bbox_min(FLT_MAX, FLT_MAX);
    ImVec2 bbox_max(-FLT_MAX, -FLT_MAX);
    for (const engine::Node& node : graph.nodes) {
        const ImVec2 sz = ed::GetNodeSize(ce_node_handle(node.id));
        const float  w  = (sz.x > 1.0f) ? sz.x : 80.0f;
        const float  h  = (sz.y > 1.0f) ? sz.y : 40.0f;
        bbox_min.x = std::min(bbox_min.x, node.x);
        bbox_min.y = std::min(bbox_min.y, node.y);
        bbox_max.x = std::max(bbox_max.x, node.x + w);
        bbox_max.y = std::max(bbox_max.y, node.y + h);
    }
    const ImVec2 bbox_size(std::max(bbox_max.x - bbox_min.x, 1.0f),
                           std::max(bbox_max.y - bbox_min.y, 1.0f));
    const ImVec2 area_size(area_max.x - area_min.x, area_max.y - area_min.y);
    const float  scale = std::min(area_size.x / bbox_size.x, area_size.y / bbox_size.y);

    for (const engine::Node& node : graph.nodes) {
        const ImVec2 sz = ed::GetNodeSize(ce_node_handle(node.id));
        const float  w  = std::max((sz.x > 1.0f ? sz.x : 80.0f) * scale, 3.0f);
        const float  h  = std::max((sz.y > 1.0f ? sz.y : 40.0f) * scale, 3.0f);
        const ImVec2 p(area_min.x + (node.x - bbox_min.x) * scale,
                       area_min.y + (node.y - bbox_min.y) * scale);
        draw_list->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + h),
                                 IM_COL32(190, 195, 220, 200), 1.0f);
        draw_list->AddRect(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + h),
                           IM_COL32(0, 0, 0, 190), 1.0f);
    }

    if (g_ce_minimap_blocking && (g_ce_minimap_clicked || g_ce_minimap_down) &&
        ce_minimap_hit(g_ce_minimap_mouse, area_min, area_max)) {
        const float cx = (g_ce_minimap_mouse.x - area_min.x) / scale + bbox_min.x;
        const float cy = (g_ce_minimap_mouse.y - area_min.y) / scale + bbox_min.y;
        ed::NavigateToCenter(ImVec2(cx, cy), 0.0f);
    }
}

// ---- G5/G7：拖动中的对齐吸附（辅助线）+ 网格 16px 吸附 ----
constexpr float kCeSnapDistance = 8.0f;  // 对齐吸附距离（屏幕像素，换算画布坐标用当前缩放）
constexpr float kCeGridSize     = 16.0f; // 网格步长（画布坐标）

// 上一帧节点位置（判断「谁在拖动」：imgui-node-editor 无公开拖动查询 API）
std::unordered_map<std::string, ImVec2> g_ce_prev_positions;

std::string ce_find_dragged_node(const engine::Graph& graph)
{
    if (!ImGui::IsMouseDown(0)) {
        g_ce_prev_positions.clear();
        return {};
    }
    for (const engine::Node& node : graph.nodes) {
        const auto it = g_ce_prev_positions.find(node.id);
        if (it == g_ce_prev_positions.end()) {
            continue;
        }
        const ImVec2 pos = ed::GetNodePosition(ce_node_handle(node.id));
        if (std::fabs(pos.x - it->second.x) > 0.5f || std::fabs(pos.y - it->second.y) > 0.5f) {
            return node.id;
        }
    }
    return {};
}

struct CeSnapResult {
    bool   applied = false; // 本帧执行了吸附（写回模型会据此标脏）
    bool   line_v  = false; // 垂直辅助线（屏幕坐标）
    bool   line_h  = false; // 水平辅助线
    ImVec2 v_a, v_b, h_a, h_b;
};

// 对齐吸附（G5）：把拖动中的节点吸附到其它节点的左/右/中心X、上/下/中心Y。
// 返回 true = 本帧已应用对齐；辅助线端点写入 out（屏幕坐标，供 ce_draw_snap_lines 绘制）。
bool ce_apply_alignment_snap(const engine::Graph& graph, const std::string& dragged,
                             const ImVec2& dpos, float dw, float dh, float snap_canvas,
                             CeSnapResult& out)
{
    const float dleft   = dpos.x;
    const float dright  = dpos.x + dw;
    const float dcx     = dpos.x + dw * 0.5f;
    const float dtop    = dpos.y;
    const float dbottom = dpos.y + dh;
    const float dcy     = dpos.y + dh * 0.5f;

    float  dx      = 0.0f;
    float  dy      = 0.0f;
    bool   have_x  = false;
    bool   have_y  = false;
    ImVec2 v_a, v_b, h_a, h_b;
    for (const engine::Node& other : graph.nodes) {
        if (other.id == dragged) {
            continue;
        }
        const ImVec2 pos  = ed::GetNodePosition(ce_node_handle(other.id));
        const ImVec2 size = ed::GetNodeSize(ce_node_handle(other.id));
        const float  w    = size.x > 1.0f ? size.x : 80.0f;
        const float  h    = size.y > 1.0f ? size.y : 40.0f;
        const float  left   = pos.x;
        const float  right  = pos.x + w;
        const float  cx     = pos.x + w * 0.5f;
        const float  top    = pos.y;
        const float  bottom = pos.y + h;
        const float  cy     = pos.y + h * 0.5f;

        // X：左/右/中心 三对边取 |delta| 最小者
        const float dx_left  = left - dleft;
        const float dx_right = right - dright;
        const float dx_cx    = cx - dcx;
        float       dx_best  = dx_left;
        if (std::fabs(dx_right) < std::fabs(dx_best)) {
            dx_best = dx_right;
        }
        if (std::fabs(dx_cx) < std::fabs(dx_best)) {
            dx_best = dx_cx;
        }
        if (std::fabs(dx_best) <= snap_canvas && (!have_x || std::fabs(dx_best) < std::fabs(dx))) {
            dx     = dx_best;
            have_x = true;
            const float align_x = dleft + dx_best;
            v_a = ed::CanvasToScreen(ImVec2(align_x, std::min(dtop, top)));
            v_b = ed::CanvasToScreen(ImVec2(align_x, std::max(dbottom, bottom)));
        }

        // Y：上/下/中心 三对边
        const float dy_top    = top - dtop;
        const float dy_bottom = bottom - dbottom;
        const float dy_cy     = cy - dcy;
        float       dy_best   = dy_top;
        if (std::fabs(dy_bottom) < std::fabs(dy_best)) {
            dy_best = dy_bottom;
        }
        if (std::fabs(dy_cy) < std::fabs(dy_best)) {
            dy_best = dy_cy;
        }
        if (std::fabs(dy_best) <= snap_canvas && (!have_y || std::fabs(dy_best) < std::fabs(dy))) {
            dy     = dy_best;
            have_y = true;
            const float align_y = dtop + dy_best;
            h_a = ed::CanvasToScreen(ImVec2(std::min(dleft, left), align_y));
            h_b = ed::CanvasToScreen(ImVec2(std::max(dright, right), align_y));
        }
    }

    if (have_x || have_y) {
        ImVec2 new_pos = dpos;
        if (have_x) {
            new_pos.x    = dpos.x + dx;
            out.line_v = true;
            out.v_a    = v_a;
            out.v_b    = v_b;
        }
        if (have_y) {
            new_pos.y    = dpos.y + dy;
            out.line_h = true;
            out.h_a    = h_a;
            out.h_b    = h_b;
        }
        ed::SetNodePosition(ce_node_handle(dragged), new_pos);
        out.applied = true;
        return true;
    }
    return false;
}

// 吸附入口（G5/G7）：找拖动中的节点 → 对齐吸附优先，未命中再就近吸附 16px 网格。
// 每次调用更新 g_ce_prev_positions（供 ce_find_dragged_node 下一帧判断）。
CeSnapResult ce_apply_snap(const engine::Graph& graph)
{
    CeSnapResult result;
    if (!g_ce_align_snap && !g_ce_grid_snap) {
        g_ce_prev_positions.clear();
        return result;
    }
    const std::string dragged = ce_find_dragged_node(graph);
    if (dragged.empty()) {
        return result;
    }
    const engine::Node* node = graph.findNode(dragged);
    if (node == nullptr) {
        return result;
    }

    const float zoom        = ed::GetCurrentZoom();
    const float snap_canvas = (zoom > 1e-6f) ? kCeSnapDistance / zoom : kCeSnapDistance;

    const ImVec2 dpos  = ed::GetNodePosition(ce_node_handle(dragged));
    const ImVec2 dsize = ed::GetNodeSize(ce_node_handle(dragged));
    const float  dw    = dsize.x > 1.0f ? dsize.x : 80.0f;
    const float  dh    = dsize.y > 1.0f ? dsize.y : 40.0f;

    // ① 对齐吸附（G5）：命中即应用（辅助线一并产出）
    const bool aligned = ce_apply_alignment_snap(graph, dragged, dpos, dw, dh, snap_canvas, result);

    // ② 网格 16px 吸附（G7）：未命中对齐时才生效，避免两种吸附互相拉扯
    if (!aligned && g_ce_grid_snap) {
        const float gx = std::round(dpos.x / kCeGridSize) * kCeGridSize;
        const float gy = std::round(dpos.y / kCeGridSize) * kCeGridSize;
        if (std::fabs(gx - dpos.x) > 0.001f || std::fabs(gy - dpos.y) > 0.001f) {
            ed::SetNodePosition(ce_node_handle(dragged), ImVec2(gx, gy));
            result.applied = true;
        }
    }

    for (const engine::Node& n : graph.nodes) {
        g_ce_prev_positions[n.id] = ed::GetNodePosition(ce_node_handle(n.id));
    }
    return result;
}

// 对齐辅助线（G5）：紫色实线，仅拖动中显示；需在 ed 上下文内（屏幕坐标已由吸附函数换算）
void ce_draw_snap_lines(const CeSnapResult& snap, ImDrawList* draw_list)
{
    if (snap.line_v) {
        draw_list->AddLine(snap.v_a, snap.v_b, IM_COL32(200, 80, 220, 255), 1.0f);
    }
    if (snap.line_h) {
        draw_list->AddLine(snap.h_a, snap.h_b, IM_COL32(200, 80, 220, 255), 1.0f);
    }
}

} // namespace

// ==================================================== 会话状态（全局单例）====
CompositeEditorState& composite_editor_state()
{
    static CompositeEditorState state;
    return state;
}

// ---- 本地撤销（快照整份 CompositeSpec，深度 30）----
namespace {

struct CeUndoEntry {
    engine::CompositeSpec spec;
    std::string           label;
};
std::vector<CeUndoEntry> g_ce_undo;
std::vector<CeUndoEntry> g_ce_redo;

constexpr std::size_t kCeUndoDepth = 30;

void ce_push_snapshot(const std::string& label)
{
    CeUndoEntry entry;
    entry.spec  = composite_editor_state().working;
    entry.label = label;
    g_ce_undo.push_back(std::move(entry));
    if (g_ce_undo.size() > kCeUndoDepth) {
        g_ce_undo.erase(g_ce_undo.begin());
    }
    g_ce_redo.clear();
    composite_editor_state().dirty = true;
}

void ce_undo()
{
    CompositeEditorState& state = composite_editor_state();
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
    CompositeEditorState& state = composite_editor_state();
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

// ------------------------------------------------------------ 会话操作 ----
void composite_editor_open(const std::string& composite_id)
{
    CompositeEditorState& state = composite_editor_state();
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
    g_ce_undo.clear();
    g_ce_redo.clear();
    if (existing != nullptr) {
        state.working = *existing;
    }
    else {
        engine::CompositeSpec fresh;
        fresh.id = engine::CompositeRegistry::instance().make_id();
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

void composite_editor_request_close()
{
    CompositeEditorState& state = composite_editor_state();
    if (state.dirty) {
        state.ask_close = true;
    }
    else {
        composite_editor_force_close();
    }
}

void composite_editor_force_close()
{
    CompositeEditorState& state = composite_editor_state();
    state.active    = false;
    state.ask_close = false;
    state.dirty     = false;
    state.error.clear();
    g_ce_undo.clear();
    g_ce_redo.clear();
}


bool composite_editor_save(std::string* error)
{
    CompositeEditorState& state = composite_editor_state();
    // 同步名称/描述输入缓冲 → 工作副本（id 首次保存时才生成）
    state.working.name        = state.name_buf.data();
    state.working.description = state.desc_buf.data();
    if (state.working.id.empty()) {
        state.working.id = engine::CompositeRegistry::instance().make_id();
    }

    std::string save_error;
    if (!engine::CompositeRegistry::instance().save(state.working, &save_error)) {
        if (error != nullptr) {
            *error = save_error;
        }
        state.error = save_error;
        return false;
    }

    state.dirty = false;
    state.error.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void composite_editor_snapshot(const std::string& label)
{
    ce_push_snapshot(label);
}

// 编辑会话内创建内部节点（位置在画布可视中心附近）
std::string composite_editor_add_node(const std::string& type, float x, float y)
{
    CompositeEditorState& state = composite_editor_state();
    ce_push_snapshot("添加内部节点 " + type);

    std::string error;
    const std::string id = state.working.internal_graph.addNode(type, x, y, &error);
    if (id.empty()) {
        log::error("[上级节点] 添加内部节点失败: " + error);
        state.error = error;
        return {};
    }
    return id;
}

bool composite_editor_delete_node(const std::string& node_id)
{
    CompositeEditorState& state = composite_editor_state();
    ce_push_snapshot("删除内部节点 " + node_id);

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

std::string composite_editor_add_edge(const std::string& from_node, const std::string& from_port,
                                      const std::string& to_node, const std::string& to_port,
                                      std::string* error)
{
    CompositeEditorState& state = composite_editor_state();
    std::string reason;
    if (!state.working.internal_graph.canConnect(from_node, from_port, to_node, to_port, &reason)) {
        if (error != nullptr) {
            *error = reason;
        }
        return {};
    }
    ce_push_snapshot("添加内部连线");
    const std::string edge_id =
        state.working.internal_graph.addEdge(from_node, from_port, to_node, to_port, error);
    return edge_id;
}

// 编辑器内撤销/重做（供窗口按钮调用）
bool composite_editor_can_undo() { return !g_ce_undo.empty(); }
bool composite_editor_can_redo() { return !g_ce_redo.empty(); }
void composite_editor_undo_once() { ce_undo(); }
void composite_editor_redo_once() { ce_redo(); }


// ==================================================== 打包选区为上级节点 ====
//  * internal_graph = 选中节点（深拷贝）+ 选区内部连线
//  * 输入 = 选区边界输入端口（有来自选区外的入边）
//  * 输出 = 边界输出端口（有到选区外的出边）；无边界输出时取选区第一个输出端口
//  * 暴露参数 = 选中节点全部非密钥参数（外部参数 id = 内部参数 id，重复取首）
std::string composite_pack_selection(const std::string& name, std::string* error)
{
    EditorState& state = editor();

    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return std::string();
    };

    if (state.selected_nodes.size() < 2) {
        return fail("打包需要至少选中 2 个节点");
    }
    const auto is_selected = [&state](const std::string& node_id) {
        return std::find(state.selected_nodes.begin(), state.selected_nodes.end(), node_id) !=
               state.selected_nodes.end();
    };

    engine::CompositeSpec spec;
    spec.name = name.empty() ? "组合节点" : name;

    // ---- 1) internal_graph：选中节点（深拷贝，保持 id 与坐标）+ 选区内部连线 ----
    for (const std::string& nid : state.selected_nodes) {
        const engine::Node* n = state.graph.findNode(nid);
        if (n == nullptr) {
            continue;
        }
        if (!n->composite_id.empty()) {
            return fail("选区内不能包含上级节点（不支持嵌套）");
        }
        engine::Node copy = *n;
        copy.state         = engine::NodeState::Idle;
        copy.error_message.clear();
        spec.internal_graph.nodes.push_back(std::move(copy));
    }
    for (const engine::Edge& edge : state.graph.edges) {
        if (is_selected(edge.from_node) && is_selected(edge.to_node)) {
            spec.internal_graph.edges.push_back(edge);
        }
    }
    spec.internal_graph.refreshSequences();

    // ---- 2) 暴露输入：选区节点的输入端口，有来自选区外的入边 ----
    for (const engine::Node& n : spec.internal_graph.nodes) {
        for (const engine::Port& p : n.inputs) {
            bool exposed = false;
            for (const engine::Edge& e : state.graph.edges) {
                if (e.to_node == n.id && e.to_port == p.id && !is_selected(e.from_node)) {
                    exposed = true;
                    break;
                }
            }
            if (!exposed) {
                continue;
            }
            engine::CompositePortRef ref;
            ref.internal_node = n.id;
            ref.internal_port = p.id;
            ref.display_name  = p.display_name;
            ref.type          = p.type;
            spec.inputs.push_back(std::move(ref));
        }
    }

    // ---- 3) 暴露输出：边界输出端口（唯一；编辑器可再调整）----
    {
        std::vector<engine::CompositePortRef> boundary;
        for (const engine::Node& n : spec.internal_graph.nodes) {
            for (const engine::Port& p : n.outputs) {
                for (const engine::Edge& e : state.graph.edges) {
                    if (e.from_node == n.id && e.from_port == p.id && !is_selected(e.to_node)) {
                        engine::CompositePortRef ref;
                        ref.internal_node = n.id;
                        ref.internal_port = p.id;
                        ref.display_name  = p.display_name;
                        ref.type          = p.type;
                        boundary.push_back(std::move(ref));
                        break;
                    }
                }
            }
        }
        if (boundary.empty()) {
            // 无边界输出：退而取选区第一个输出端口（若选区整体是端到端内部流程）
            for (const engine::Node& n : spec.internal_graph.nodes) {
                if (n.outputs.empty()) {
                    continue;
                }
                const engine::Port& p = n.outputs.front();
                engine::CompositePortRef ref;
                ref.internal_node = n.id;
                ref.internal_port = p.id;
                ref.display_name  = p.display_name;
                ref.type          = p.type;
                boundary.push_back(std::move(ref));
                break;
            }
        }
        if (boundary.empty()) {
            return fail("选区内没有任何可对外输出的端口（至少需要一个输出端口）");
        }
        spec.outputs.push_back(std::move(boundary.front()));
    }

    // ---- 4) 暴露参数：选中节点全部非密钥参数（外部参数 id = 内部参数 id）----
    for (const engine::Node& n : spec.internal_graph.nodes) {
        for (const engine::Param& p : n.params) {
            if (p.is_secret) {
                continue; // 密钥（如 API Key）不暴露
            }
            const bool dup = std::any_of(spec.exposed_params.begin(), spec.exposed_params.end(),
                                         [&p](const engine::CompositeParamRef& r) {
                                             return r.param_id == p.id;
                                         });
            if (dup) {
                continue; // 外部参数 id 唯一：重复的只暴露首个
            }
            engine::CompositeParamRef ref;
            ref.internal_node = n.id;
            ref.param_id      = p.id;
            ref.display_name  = p.display_name;
            spec.exposed_params.push_back(std::move(ref));
        }
    }

    // ---- 5) 校验 + 落盘 + 打开编辑器 ----
    spec.id = engine::CompositeRegistry::instance().make_id();
    std::string save_error;
    if (!engine::CompositeRegistry::instance().save(spec, &save_error)) {
        return fail("打包失败：" + save_error);
    }
    composite_editor_open(spec.id);
    if (error != nullptr) {
        error->clear();
    }
    return spec.id;
}


// ==================================================== 编辑器窗口 ============
namespace {

// 规格标记面板里的显示名编辑缓冲（key = 节点id + "|" + 端口id）
std::map<std::string, std::array<char, 128>>& ce_name_buffers()
{
    static std::map<std::string, std::array<char, 128>> buffers;
    return buffers;
}

std::array<char, 128>& ce_name_buffer(const std::string& key, const std::string& initial)
{
    auto& buffers = ce_name_buffers();
    auto  it      = buffers.find(key);
    if (it == buffers.end()) {
        std::array<char, 128> buf{};
        const std::size_t     n = std::min<std::size_t>(initial.size(), buf.size() - 1);
        initial.copy(buf.data(), n);
        buf[n] = '\0';
        it     = buffers.emplace(key, std::move(buf)).first;
    }
    return it->second;
}

} // namespace

void draw_composite_editor(const char* title, bool* open)
{
    CompositeEditorState& state = composite_editor_state();
    if (!state.active) {
        return;
    }
    // 点窗口 X 关闭：有未保存更改则拦截并弹确认
    if (open != nullptr && !*open) {
        if (state.dirty) {
            *open            = true;
            state.ask_close  = true;
        }
        else {
            composite_editor_force_close();
            return;
        }
    }

    // 名称/描述输入缓冲同步到工作副本（结构类修改才进撤销栈）
    state.working.name        = state.name_buf.data();
    state.working.description = state.desc_buf.data();

    ImGui::SetNextWindowSize(ImVec2(1000.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, open)) {
        ImGui::End();
        return;
    }

    // ---- 顶栏 ----
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("名称", state.name_buf.data(), state.name_buf.size());
    ImGui::SameLine();
    ImGui::TextDisabled("id: %s", state.working.id.c_str());
    ImGui::SameLine();
    if (ImGui::Button("撤销")) {
        composite_editor_undo_once();
    }
    ImGui::SameLine();
    if (ImGui::Button("重做")) {
        composite_editor_redo_once();
    }
    ImGui::SameLine();
    if (ImGui::Button("适配视图")) { // G8：ed::Begin 内执行 NavigateToContent
        g_ce_request_fit = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("画布辅助")) { // G4/G5/G7：小地图 / 对齐吸附 / 网格
        ImGui::OpenPopup("##ce_canvas_assist");
    }
    if (ImGui::BeginPopup("##ce_canvas_assist")) {
        ImGui::TextDisabled("画布辅助（会话级设置）");
        ImGui::Separator();
        ImGui::Checkbox("小地图（左下角）", &g_ce_show_minimap);
        ImGui::Checkbox("对齐吸附 + 辅助线", &g_ce_align_snap);
        ImGui::Checkbox("网格 16px 吸附", &g_ce_grid_snap);
        ImGui::Checkbox("网格显示", &g_ce_show_grid);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("保存")) {
        std::string error;
        if (composite_editor_save(&error)) {
            log::info("[上级节点] 已保存 " + state.working.id + "（" + state.working.name + "）");
        }
        else {
            log::warn("[上级节点] 保存失败: " + error);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("关闭")) {
        composite_editor_request_close();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(state.dirty ? "● 有未保存更改" : "已保存");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("描述", state.desc_buf.data(), state.desc_buf.size());
    if (!state.error.empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", state.error.c_str());
    }
    ImGui::Separator();

    // ---- 主体布局：左侧画布 + 右侧规格面板 ----
    const float spec_panel_w = 320.0f;
    std::string ce_selected_node; // PS-09-G1：本帧选中的内部节点（单选），供右侧参数编辑区使用
    ImGui::BeginChild("##ce_canvas_host",
                      ImVec2(ImGui::GetContentRegionAvail().x - spec_panel_w, 0.0f));
    {
        ensure_ce_context();
        ed::SetCurrentEditor(g_ce_context);

        // 位置同步：新建节点落到 working 记录坐标
        for (const engine::Node& n : state.working.internal_graph.nodes) {
            ed::SetNodePosition(ce_node_handle(n.id), ImVec2(n.x, n.y));
        }

        ce_block_minimap_input(); // G4：小地图区域输入与画布隔离（ed::Begin 之前）

        ed::Begin("##CeCanvas", ImVec2(0.0f, 0.0f));
        if (g_ce_request_fit) { // G8：适配视图
            ed::NavigateToContent(0.0f);
            g_ce_request_fit = false;
        }
        if (!g_ce_show_grid) { // G7：隐藏网格（透明网格色）
            ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        }
        for (const engine::Node& n : state.working.internal_graph.nodes) {
            ce_draw_node(n);
        }
        for (const engine::Edge& e : state.working.internal_graph.edges) {
            const engine::Node* from = state.working.internal_graph.findNode(e.from_node);
            const engine::Node* to   = state.working.internal_graph.findNode(e.to_node);
            if (from == nullptr || to == nullptr) {
                continue;
            }
            const int fi = from->portIndex(e.from_port, engine::PortDirection::Output);
            const int ti = to->portIndex(e.to_port, engine::PortDirection::Input);
            if (fi < 0 || ti < 0) {
                continue;
            }
            ed::Link(ce_link_handle(e.id),
                     ce_pin_handle(*from, engine::PortDirection::Output, fi),
                     ce_pin_handle(*to, engine::PortDirection::Input, ti));
        }

        // 拖出连线（类型不符变红被拒）
        const bool ce_creating = ed::BeginCreate(ImVec4(0.29f, 0.56f, 0.89f, 1.0f), 2.0f);
        if (ce_creating) {
            ed::PinId start_pin;
            ed::PinId end_pin;
            if (ed::QueryNewLink(&start_pin, &end_pin) && start_pin && end_pin) {
                CePinRef from = ce_resolve_pin(start_pin, state.working.internal_graph);
                CePinRef to   = ce_resolve_pin(end_pin, state.working.internal_graph);
                if (from.valid && to.valid && from.direction == engine::PortDirection::Input &&
                    to.direction == engine::PortDirection::Output) {
                    std::swap(from, to);
                }
                if (!from.valid || !to.valid) {
                    ed::RejectNewItem(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), 2.0f);
                }
                else {
                    std::string reason;
                    const bool  ok = state.working.internal_graph.canConnect(
                        from.node_id, from.port_id, to.node_id, to.port_id, &reason);
                    if (!ok) {
                        ed::RejectNewItem(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), 2.0f);
                        if (ImGui::IsMouseReleased(0)) {
                            state.error = reason;
                        }
                    }
                    else if (ed::AcceptNewItem(ImVec4(0.29f, 0.56f, 0.89f, 1.0f), 2.0f)) {
                        composite_editor_snapshot("添加内部连线");
                        std::string add_error;
                        composite_editor_add_edge(from.node_id, from.port_id, to.node_id,
                                                  to.port_id, &add_error);
                        if (!add_error.empty()) {
                            state.error = add_error;
                        }
                    }
                }
            }
        }
        // 【重要契约】与主画布 handle_create_link 一致：BeginCreate 返回 false 时
        // CreateItemAction 的 m_InActive 仍被置 true，EndCreate() 必须无条件配对，
        // 否则下一帧 BeginCreate 触发 IM_ASSERT(false == m_InActive) 崩溃。
        ed::EndCreate();

        // 删除（节点连带连线；连线直接删模型）
        const bool ce_deleting = ed::BeginDelete();
        if (ce_deleting) {
            ed::LinkId deleted_link;
            while (ed::QueryDeletedLink(&deleted_link)) {
                if (ed::AcceptDeletedItem()) {
                    composite_editor_snapshot("删除内部连线");
                    state.working.internal_graph.removeEdge("e" + std::to_string(deleted_link.Get()));
                }
            }
            ed::NodeId deleted_node;
            while (ed::QueryDeletedNode(&deleted_node)) {
                if (ed::AcceptDeletedItem()) {
                    composite_editor_delete_node("n" + std::to_string(deleted_node.Get()));
                }
            }
        }
        ed::EndDelete(); // 与 BeginDelete 无条件配对（契约同 BeginCreate）

        // 右键菜单（节点 > 背景；G2 节点菜单：删除内部节点 / 改名）
        ed::Suspend();
        static ed::NodeId ce_context_node_handle; // static：避免每帧重置
        static ImVec2     ce_spawn_position;      // 背景右键时的落点（画布坐标）
        std::string       ce_context_node;
        if (ed::ShowNodeContextMenu(&ce_context_node_handle)) {
            ce_context_node = "n" + std::to_string(ce_context_node_handle.Get());
            ImGui::OpenPopup("##ce_node_menu");
        }
        if (ed::ShowBackgroundContextMenu()) {
            ce_spawn_position  = ed::ScreenToCanvas(ImGui::GetMousePos());
            g_ce_add_filter[0] = '\0'; // G3：重新打开时清空搜索
            ImGui::OpenPopup("##ce_add_node_menu");
        }
        if (ImGui::BeginPopup("##ce_node_menu")) {
            ImGui::TextDisabled("内部节点 %s", ce_context_node.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("改名")) {
                ce_open_rename(ce_context_node);
            }
            if (ImGui::MenuItem("删除内部节点")) {
                composite_editor_delete_node(ce_context_node);
                ed::ClearSelection(); // 删除后清空选中，参数编辑区回落「未选中」
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##ce_add_node_menu")) {
            ImGui::TextDisabled("添加内部节点");
            ImGui::Separator();
            // G3：搜索过滤（打开时已清空；输入即时过滤）
            if (ImGui::IsWindowAppearing()) {
                ImGui::SetKeyboardFocusHere(); // 打开即可直接键入
            }
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##ce_add_filter", "搜索节点…", g_ce_add_filter,
                                     sizeof(g_ce_add_filter));
            ImGui::Separator();
            engine::registerAllNodes();
            engine::NodeRegistry& registry  = engine::NodeRegistry::instance();
            const std::string     query     = g_ce_add_filter;
            const bool            searching = !query.empty();

            // 搜索模式：跨分类平铺命中项；否则按分类分组（与节点库一致）
            ImGui::BeginChild("##ce_add_scroll", ImVec2(340.0f, 320.0f), false,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar);
            bool any = false;
            if (searching) {
                for (const engine::Definition* def : registry.listTypes()) {
                    if (ce_find_ci(def->display_name, query) == std::string::npos &&
                        ce_find_ci(def->type, query) == std::string::npos) {
                        continue;
                    }
                    any = true;
                    if (ImGui::MenuItem(def->display_name.c_str())) {
                        composite_editor_add_node(def->type, ce_spawn_position.x,
                                                  ce_spawn_position.y);
                    }
                }
                if (!any) {
                    ImGui::TextDisabled("（无匹配节点）");
                }
            }
            else {
                static const engine::NodeCategory kCeCategories[] = {
                    engine::NodeCategory::Input,     engine::NodeCategory::Process,
                    engine::NodeCategory::Config,    engine::NodeCategory::Inference,
                    engine::NodeCategory::Output,    engine::NodeCategory::Auxiliary};
                for (engine::NodeCategory category : kCeCategories) {
                    const std::vector<const engine::Definition*> items =
                        registry.listByCategory(category);
                    if (items.empty()) {
                        continue;
                    }
                    any = true;
                    if (ImGui::CollapsingHeader(engine::categoryName(category),
                                                ImGuiTreeNodeFlags_DefaultOpen)) {
                        for (const engine::Definition* def : items) {
                            if (ImGui::MenuItem(def->display_name.c_str())) {
                                composite_editor_add_node(def->type, ce_spawn_position.x,
                                                          ce_spawn_position.y);
                            }
                        }
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##ce_rename_menu")) {
            ImGui::TextDisabled("重命名内部节点 %s", g_ce_rename_id.c_str());
            ImGui::SetNextItemWidth(220.0f);
            const bool enter_pressed = ImGui::InputText(
                "##ce_rename_input", g_ce_rename_buf.data(), g_ce_rename_buf.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (enter_pressed) {
                ce_apply_rename(); // 回车确认
            }
            if (ImGui::Button("确定")) {
                ce_apply_rename();
            }
            ImGui::SameLine();
            if (ImGui::Button("取消")) {
                g_ce_rename_id.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        // G6：双击内部节点标题 → 改名弹窗
        const ed::NodeId ce_double_clicked = ed::GetDoubleClickedNode();
        if (ce_double_clicked.Get() != 0) {
            ce_open_rename("n" + std::to_string(ce_double_clicked.Get()));
        }
        ed::Resume();

        // G5/G7：拖动中的对齐吸附（辅助线）+ 网格 16px 吸附
        const CeSnapResult ce_snap = ce_apply_snap(state.working.internal_graph);

        // 位置写回（有变化才标记 dirty）
        for (engine::Node& n : state.working.internal_graph.nodes) {
            const ImVec2 p = ed::GetNodePosition(ce_node_handle(n.id));
            if (p.x != n.x || p.y != n.y) {
                n.x          = p.x;
                n.y          = p.y;
                state.dirty  = true;
            }
        }

        // PS-09-G1：同步内部节点选中（单选 → 右侧「参数编辑」区显示该节点参数）
        ce_selected_node.clear();
        {
            const int sel_count = ed::GetSelectedObjectCount();
            if (sel_count > 0) {
                std::vector<ed::NodeId> sel_nodes(static_cast<std::size_t>(sel_count));
                const int node_count = ed::GetSelectedNodes(sel_nodes.data(), sel_count);
                if (node_count == 1) {
                    ce_selected_node = "n" + std::to_string(sel_nodes[0].Get());
                }
            }
        }

        // G4 小地图 + G5 辅助线（ed::Begin 内、ed::End 前绘制；主画布 L3 同款时序）
        ce_draw_minimap(state.working.internal_graph);
        ce_draw_snap_lines(ce_snap, ImGui::GetWindowDrawList());
        if (!g_ce_show_grid) {
            ed::PopStyleColor(); // 与 ed::Begin 后的 PushStyleColor 配对
        }

        ed::End();
        ed::SetCurrentEditor(nullptr);
    }
    ImGui::EndChild();


    // ---- 右侧规格面板：对外输入 / 对外输出 / 暴露参数 ----
    ImGui::SameLine();
    ImGui::BeginChild("##ce_spec_panel", ImVec2(spec_panel_w, 0.0f));
    {
        ImGui::TextColored(ImVec4(0.8f, 0.7f, 1.0f, 1.0f), "规格标记");
        ImGui::TextDisabled("勾选端口/参数，即暴露到上级节点外部");
        ImGui::Separator();

        if (state.working.internal_graph.nodes.empty()) {
            ImGui::TextWrapped("内部图为空：右键画布添加内部节点。");
        }

        for (engine::Node& node : state.working.internal_graph.nodes) {
            ImGui::PushID(node.id.c_str());
            const std::string header = node.title + "（" + node.id + "）";
            if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                // 输入 → 对外输入（有内部入边的端口不允许标记）
                for (engine::Port& port : node.inputs) {
                    const bool has_internal_in =
                        state.working.internal_graph.findEdgeIntoInput(node.id, port.id) != nullptr;
                    bool exposed = false;
                    for (const engine::CompositePortRef& ref : state.working.inputs) {
                        if (ref.internal_node == node.id && ref.internal_port == port.id) {
                            exposed = true;
                        }
                    }
                    ImGui::PushID(("in" + port.id).c_str());
                    if (ImGui::Checkbox("对外输入", &exposed)) {
                        composite_editor_snapshot("标记对外输入 " + port.id);
                        state.working.inputs.erase(
                            std::remove_if(state.working.inputs.begin(), state.working.inputs.end(),
                                           [&node, &port](const engine::CompositePortRef& r) {
                                               return r.internal_node == node.id &&
                                                      r.internal_port == port.id;
                                           }),
                            state.working.inputs.end());
                        if (exposed) {
                            engine::CompositePortRef ref;
                            ref.internal_node = node.id;
                            ref.internal_port = port.id;
                            ref.display_name  = port.display_name;
                            ref.type          = port.type;
                            state.working.inputs.push_back(std::move(ref));
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s%s", port.display_name.c_str(),
                                        has_internal_in ? "（有内部连线）" : "");
                    ImGui::PopID();
                }

                // 输出 → 对外输出（单选）
                for (engine::Port& port : node.outputs) {
                    bool selected = false;
                    for (const engine::CompositePortRef& ref : state.working.outputs) {
                        if (ref.internal_node == node.id && ref.internal_port == port.id) {
                            selected = true;
                        }
                    }
                    ImGui::PushID(("out" + port.id).c_str());
                    if (ImGui::RadioButton("对外输出", selected)) {
                        if (!selected) {
                            composite_editor_snapshot("标记对外输出 " + port.id);
                            state.working.outputs.clear();
                            engine::CompositePortRef ref;
                            ref.internal_node = node.id;
                            ref.internal_port = port.id;
                            ref.display_name  = port.display_name;
                            ref.type          = port.type;
                            state.working.outputs.push_back(std::move(ref));
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", port.display_name.c_str());
                    ImGui::PopID();
                }
                // 参数 → 暴露（外部参数 id 唯一：重复的只暴露首个）
                for (engine::Param& param : node.params) {
                    bool exposed = false;
                    for (const engine::CompositeParamRef& ref : state.working.exposed_params) {
                        if (ref.internal_node == node.id && ref.param_id == param.id) {
                            exposed = true;
                        }
                    }
                    ImGui::PushID(("p" + param.id).c_str());
                    if (ImGui::Checkbox("暴露参数", &exposed)) {
                        composite_editor_snapshot("切换暴露参数 " + param.id);
                        state.working.exposed_params.erase(
                            std::remove_if(state.working.exposed_params.begin(),
                                           state.working.exposed_params.end(),
                                           [&node, &param](const engine::CompositeParamRef& r) {
                                               return r.internal_node == node.id &&
                                                      r.param_id == param.id;
                                           }),
                            state.working.exposed_params.end());
                        if (exposed) {
                            const bool dup = std::any_of(
                                state.working.exposed_params.begin(),
                                state.working.exposed_params.end(),
                                [&param](const engine::CompositeParamRef& r) {
                                    return r.param_id == param.id;
                                });
                            if (!dup) {
                                engine::CompositeParamRef ref;
                                ref.internal_node = node.id;
                                ref.param_id      = param.id;
                                ref.display_name  = param.display_name;
                                state.working.exposed_params.push_back(std::move(ref));
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", param.display_name.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::PopID();
        }

        // 外部显示名编辑（输入/输出端口）
        ImGui::Separator();
        ImGui::TextDisabled("外部显示名（可改）");
        for (engine::CompositePortRef& ref : state.working.inputs) {
            const std::string key = ref.internal_node + "|" + ref.internal_port + "|in";
            std::array<char, 128>& buf = ce_name_buffer(key, ref.display_name);
            ImGui::PushID(key.c_str());
            if (ImGui::InputText("##dn", buf.data(), buf.size())) {
                ref.display_name = buf.data();
                state.dirty      = true;
            }
            ImGui::PopID();
        }
        for (engine::CompositePortRef& ref : state.working.outputs) {
            const std::string key = ref.internal_node + "|" + ref.internal_port + "|out";
            std::array<char, 128>& buf = ce_name_buffer(key, ref.display_name);
            ImGui::PushID(key.c_str());
            if (ImGui::InputText("##dn", buf.data(), buf.size())) {
                ref.display_name = buf.data();
                state.dirty      = true;
            }
            ImGui::PopID();
        }
        if (state.working.inputs.empty() && state.working.outputs.empty()) {
            ImGui::TextDisabled("（尚无对外端口）");
        }

        // ---- PS-09-G1：参数编辑区（选中内部节点 → 复用公共 draw_param_widget）----
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.75f, 0.9f, 0.7f, 1.0f), "参数编辑");
        ImGui::TextDisabled("在画布中单击一个内部节点，在此编辑其参数值。");
        if (ce_selected_node.empty()) {
            ImGui::TextDisabled("（未选中节点）");
        }
        else {
            engine::Node* sel = state.working.internal_graph.findNode(ce_selected_node);
            if (sel == nullptr) {
                ImGui::TextDisabled("（选中节点不存在）");
            }
            else {
                ImGui::TextDisabled("%s（%s）", sel->title.c_str(), sel->id.c_str());
                if (sel->params.empty()) {
                    ImGui::TextDisabled("该节点没有参数");
                }
                for (engine::Param& param : sel->params) {
                    ImGui::PushID(param.id.c_str());
                    ImGui::TextUnformatted(param.display_name.c_str());
                    if (param.is_secret) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("（密钥：仅存内存、不写盘）");
                    }
                    ImGui::PopID();

                    bool param_begin  = false;
                    bool param_change = false;
                    draw_param_widget(*sel, param, param_begin, param_change);
                    if (param_begin) {
                        composite_editor_snapshot("修改内部节点参数 " + sel->id + "." + param.id);
                    }
                    if (param_change) {
                        state.dirty = true; // 参数值已写回工作副本（ce_push_snapshot 亦置 dirty）
                    }
                    ImGui::Spacing();
                }
            }
        }
    }
    ImGui::EndChild();


    // ---- 关闭确认弹窗 ----
    if (state.ask_close) {
        ImGui::OpenPopup("##ce_close_confirm");
        state.ask_close = false;
    }
    if (ImGui::BeginPopupModal("未保存的更改", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("上级节点「%s」有未保存的更改。", state.working.name.c_str());
        ImGui::Spacing();
        bool closed = false;
        bool discard = false;
        if (ImGui::Button("保存并关闭", ImVec2(120.0f, 0.0f))) {
            std::string error;
            if (composite_editor_save(&error)) {
                closed = true;
            }
            else {
                state.error = error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("放弃更改", ImVec2(120.0f, 0.0f))) {
            closed  = true;
            discard = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(80.0f, 0.0f))) {
            closed = true;
        }
        if (closed) {
            ImGui::CloseCurrentPopup();
        }
        if (closed && (discard || !state.dirty)) {
            // 放弃：直接关闭会话；保存成功：会话已不脏
            composite_editor_force_close();
            if (open != nullptr) {
                *open = false;
            }
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

// ==================================================== 新建 / 打包对话框 ====
namespace {

// 新建上级节点：名称输入对话框
bool g_new_dialog_open = false;
char g_new_name[128]   = "";
std::string g_new_error; // 对话框内错误展示（打开时清空）

// 打包选区：名称输入对话框
bool g_pack_dialog_open = false;
char g_pack_name[128]   = "";
std::string g_pack_error; // 打包失败原因（对话框内展示，打开时清空）

// [调试] --auto-open-composite：对话框可见后自动执行一次「创建并打开编辑器」点击
bool g_new_dialog_auto_accept = false;

// 点「创建并打开编辑器」的动作：组装空白规格 → 落盘 JSON → 走标准「打开」路径读取。
// （不创建无文件支撑的内存会话，避免依赖 imgui-node-editor 上下文的时序。）
// 返回 true = 已创建并打开（调用方应关闭对话框）；false = 失败（g_new_error 已填）。
bool create_and_open_new_composite()
{
    g_new_error.clear();
    std::string name = g_new_name;
    if (name.empty()) {
        name = "未命名上级节点";
    }
    engine::CompositeSpec spec;
    spec.id   = engine::CompositeRegistry::instance().make_id();
    spec.name = name;
    std::string save_error;
    // 新建 = 落盘「空白草稿」：跳过「内部图为空 / 未标记输出」校验（用户随后在编辑器里
    // 添加内部节点、标记输出；点「保存」时再走严格校验）。此前严格校验导致空白规格
    // 永远无法落盘 → 「创建并打开编辑器」只报错、编辑器永不打开（见 CN 校验与 2026-10 修复）。
    if (!engine::CompositeRegistry::instance().save(spec, &save_error,
                                                    /*allow_incomplete_draft=*/true)) {
        g_new_error = "创建失败：" + save_error;
        return false; // 保留弹窗展示错误，不关闭
    }
    composite_editor_open(spec.id);
    return true;
}

void run_new_dialog()
{
    if (!g_new_dialog_open) {
        return;
    }
    ImGui::OpenPopup("新建上级节点##ce_new");
    if (!ImGui::BeginPopupModal("新建上级节点##ce_new", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }
    if (g_new_dialog_auto_accept) {
        g_new_dialog_auto_accept = false;
        if (create_and_open_new_composite()) {
            ImGui::CloseCurrentPopup();
            g_new_dialog_open = false;
        }
    }
    ImGui::TextWrapped("新建一个空的上级节点：先在本地落盘一份空白规格 JSON，随后打开编辑器"
                       "添加内部节点、标记端口与参数。");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("名称", g_new_name, sizeof(g_new_name));
    bool closed = false;
    if (ImGui::Button("创建并打开编辑器", ImVec2(180.0f, 0.0f))) {
        closed = create_and_open_new_composite();
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(80.0f, 0.0f))) {
        closed = true;
    }
    if (!g_new_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", g_new_error.c_str());
    }
    if (closed) {
        ImGui::CloseCurrentPopup();
        g_new_dialog_open = false;
    }
    ImGui::EndPopup();
}

void run_pack_dialog()
{
    if (!g_pack_dialog_open) {
        return;
    }
    ImGui::OpenPopup("打包为上级节点##ce_pack");
    if (!ImGui::BeginPopupModal("打包为上级节点##ce_pack", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }
    const int count = static_cast<int>(editor().selected_nodes.size());
    ImGui::TextWrapped("把选中的 %d 个节点打包为上级节点。\n边界输入/输出、暴露参数将自动识别（可在编辑器调整）。",
                       count);
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("名称", g_pack_name, sizeof(g_pack_name));

    if (ImGui::Button("打包", ImVec2(100.0f, 0.0f))) {
        g_pack_error.clear();
        const std::string id =
            composite_pack_selection(std::string(g_pack_name), &g_pack_error);
        if (!id.empty()) {
            ImGui::CloseCurrentPopup();
            g_pack_dialog_open = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(80.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
        g_pack_dialog_open = false;
    }
    if (!g_pack_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", g_pack_error.c_str());
    }
    ImGui::EndPopup();
}

} // namespace

void composite_new_dialog()
{
    if (!g_new_dialog_open) {
        g_new_dialog_open = true;
        g_new_name[0]     = '\0';
        g_new_error.clear(); // 重新打开时清空上次失败原因
    }
}

// [调试] --auto-open-composite：打开「新建上级节点」对话框，并在其首次可见帧
// 自动执行「创建并打开编辑器」点击（与手工操作同路径，便于复现窗口栈断言）。
void composite_editor_test_drive_new_dialog()
{
    if (!g_new_dialog_open) {
        g_new_dialog_open        = true;
        g_new_name[0]            = '\0';
        g_new_error.clear();
        g_new_dialog_auto_accept = true;
    }
}

void composite_pack_dialog()
{
    if (!g_pack_dialog_open) {
        g_pack_dialog_open = true;
        g_pack_name[0]     = '\0';
        g_pack_error.clear(); // 重新打开时清空上次失败原因
        if (editor().selected_nodes.size() >= 2) {
            std::snprintf(g_pack_name, sizeof(g_pack_name), "组合节点");
        }
        else {
            composite_editor_state().error = "请先在画布上选中至少 2 个节点再打包";
            g_pack_dialog_open             = false;
        }
    }
}

void draw_composite_dialogs()
{
    run_new_dialog();
    run_pack_dialog();
}

} // namespace aiwrite::ui

