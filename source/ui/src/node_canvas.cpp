#include "node_canvas.h"

#include "composite.h"         // CN：上级节点实例识别 / 创建
#include "canvas_view.h"       // CN_editor_refactor：共享只读绘制层（CEA-01 / CEA-02）
#include "interactor.h"        // ARCH-E5：画布交互共享层（kCanvasPositionLimit / interactorPushNodePosition）
#include "composite_editor.h"  // CN：双击/右键打开上级节点编辑器
#include "node_registry.h"
#include "provider_resolve.h"
#include "editor_controller.h"
#include "commands.h"          // POL-E-01a：右键菜单收敛进命令层（三入口一致）
#include "theme.h"             // canvas_theme_main（主画布主题）
#include "config.h"
#include "texture_cache.h"     // P7a-16：节点卡片缩略图 + 格式徽标

#include <imgui.h>
#include <imgui_node_editor.h>

#include <chrono>
#include <cstdint>
#include <string>

#include "log.h"
#include "node_canvas_detail.h"  // ARCH-Z1：impl TU 的 12 个入口（ensure_context / 菜单 / 同步 / 诊断 …）

namespace aiwrite::ui {

namespace ed = ax::NodeEditor;  // 原匿名命名空间内的别名（渲染 TU 需自备一份）
using namespace canvas_detail;  // 原匿名命名空间函数（同 TU 直接调用，名字查找不变）
using engine::Edge;
using engine::Graph;
using engine::Node;
using engine::NodeCategory;
using engine::NodeState;
using engine::Port;
using engine::PortDirection;
using engine::PortType;
// ============================================================================
//  M_node_expansion L3（v5 定稿 §4.4.3）：小地图 ④ / 对齐辅助系统 ⑤+⑥ / 辅助开关
//  会话级开关（不进 config：避免动 settings_validation 断言面）
// ============================================================================

// ----------------------------------------------------------------------------
//  小地图 = 纯导航工具（M_node_expansion L3 · 2026-10-15 修正）：
//  · 只做「查看缩略 + 点击/拖动 → 视口中心平移到对应画布位置」（快速挪动 / 查询）；
//  · 严禁在小地图区域触发任何画布编辑（选中 / 拖节点 / 框选 / 右键 / 平移）。
//  实现：ed::Begin 之前把该区域的鼠标事件从 io 中摘除（ed 完全感知不到），
//       小地图交互由共享 draw_minimap 用「屏蔽前的快照」自行驱动。
//  CLN-09：几何 / 输入屏蔽 / 绘制已上移 canvas_view 单一来源，本 TU 只留开关与状态。
// ----------------------------------------------------------------------------


// 上一帧节点位置（对齐辅助判断「谁在拖动」：imgui-node-editor 无公开拖动查询 API）

// 拖拽中的节点 id：已上移 canvas_view 共享（CLN-10），主画布传 nc.alt_pan_active 互斥。
// 对齐吸附封装（CLN-10）：开关 + 拖拽判定 + 调共享核心 + 状态更新。
// 对齐核心逻辑（三边对 / 辅助线端点）已上移 canvas_view::apply_alignment_snap_core 单一来源。
SnapResult apply_alignment_snap(const Graph& graph, NodeCanvasState& nc)
{
    SnapResult result;
    if (!nc.snap_enabled || nc.alt_pan_active) {
        nc.prev_positions.clear();
        return result;
    }
    const std::string dragged = find_dragged_node(graph, nc.alt_pan_active, nc.prev_positions);
    if (dragged.empty()) {
        return result;
    }
    if (graph.findNode(dragged) == nullptr) {
        return result;
    }
    const float zoom        = ed::GetCurrentZoom();
    const float snap_canvas = (zoom > 1e-6f) ? nc.snap_distance / zoom : nc.snap_distance;
    const ImVec2 dpos  = ed::GetNodePosition(nodeHandle(dragged));
    const ImVec2 dsize = ed::GetNodeSize(nodeHandle(dragged));
    const float  dw    = dsize.x > 1.0f ? dsize.x : 80.0f;
    const float  dh    = dsize.y > 1.0f ? dsize.y : 40.0f;
    result = apply_alignment_snap_core(graph, dragged, dpos, dw, dh, snap_canvas);
    for (const Node& node : graph.nodes) {
        nc.prev_positions[node.id] = ed::GetNodePosition(nodeHandle(node.id));
    }
    return result;
}
// 小地图（左下角缩略图）已上移 canvas_view::draw_minimap 共享（CLN-09），
// 主画布在 ed::End 之后以 canvas_theme_main() + 状态着色回调调用（见 draw 循环尾）。

// 画布辅助开关弹窗（小地图 / 对齐吸附 / 吸附距离）
void draw_canvas_assist_popup(NodeCanvasState& nc)
{
    if (ImGui::Button("画布辅助")) {
        ImGui::OpenPopup("##canvas_assist");
    }
    if (ImGui::BeginPopup("##canvas_assist")) {
        ImGui::TextDisabled("画布辅助（会话级设置）");
        ImGui::Separator();
        ImGui::Checkbox("小地图（左下角）", &nc.show_minimap);
        ImGui::Checkbox("对齐吸附（拖动时）", &nc.snap_enabled);
        ImGui::SliderFloat("吸附距离", &nc.snap_distance, 2.0f, 30.0f, "%.0f px");
        ImGui::EndPopup();
    }
}



void drawNodeCanvas(const char* title, CanvasOptions& options, const ReadOnlyView& view,
                    EditorController& editor)
{
    EditorController& state = editor;
    NodeCanvasState&  nc    = state.node_canvas(); // ARCH-A6：画布跨帧状态（单实例）
    ensure_context(nc);


    ImGui::SetNextWindowSize(ImVec2(1000.0f, 600.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title)) {
        ImGui::End();
        return;
    }

    // 新增：Alt+左键拖拽平移（Alt 未按下时完全维持原有绑定，见 update_alt_left_pan）。
    // 放在提示行之前 → 提示能立刻反映本帧的平移模式。
    nc.alt_pan_active = update_alt_left_pan(nc);

    ImGui::TextDisabled("输入端口在节点左侧、输出端口在右侧 ｜ 拖左侧圆点连线，类型不符会变红被拒");
    ImGui::SameLine();
    ImGui::TextDisabled("｜ 右键：撤销 / 重做 · 画布新建 · 复制 / 粘贴 · 打包为上级节点 ｜ 左键点连线可选中 → Delete 或工具栏「删除选中」");
    if (nc.alt_pan_active) {
        ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f),
                           "Alt 平移模式：按住左键拖拽即可平移画布（松开 Alt 即恢复拖动节点/框选）");
    } else {
        ImGui::TextDisabled(
            "Alt + 左键拖拽：平移画布（无中键鼠标 / 笔记本也可用）｜ 滚轮：以鼠标为中心缩放 0.1x~4.0x");
    }
    ImGui::SameLine();
    draw_canvas_assist_popup(nc); // M_node_expansion L3：小地图 / 对齐吸附开关
    ImGui::Separator();

    ed::SetCurrentEditor(nc.context);
    // 小地图 = 纯导航：该区域鼠标事件只属于小地图（查看 / 跳转），画布不得选中或编辑节点。
    // 必须放在 ed::Begin 之前（ed 在 Begin 内处理鼠标）。
    blockMinimapInput(nc.show_minimap, nc.minimap);
    // CS-01：诊断用的缩放值（必须在 ed::Begin 内取；ed::End() 之后打印整行）
    float diag_zoom = 1.0f;
    ed::Begin("##NodeCanvas", ImVec2(0.0f, 0.0f));

    // 视图自愈：损坏的设置文件可能残留 ±3e9 视图 / 2e-7 缩放，会让网格与剔除计算爆量
    // （表现为 CPU 单核打满、界面无响应）。检测到异常就重置为"跟随内容"。
    {
        const float  zoom   = ed::GetCurrentZoom();
        const ImVec2 origin = ed::ScreenToCanvas(ImVec2(0.0f, 0.0f));
        const bool zoom_bad = !std::isfinite(zoom) || zoom < 0.05f || zoom > 10.0f;
        const bool origin_bad = !std::isfinite(origin.x) || !std::isfinite(origin.y) ||
                                std::fabs(origin.x) > 1.0e6f || std::fabs(origin.y) > 1.0e6f;
        if (zoom_bad || origin_bad) {
            ed::NavigateToContent(0.0f);
            log::warn("[画布] 检测到异常视图（zoom=" + std::to_string(zoom) +
                      "，原点 x=" + std::to_string(origin.x) + "），已重置为跟随内容");
            state.setStatus("已重置画布视图（检测到异常缩放/原点）");
        }
    }

    if (!options.show_grid) {
        ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    }

    diag_zoom = ed::GetCurrentZoom(); // CS-01：视图自愈之后再取，避免拿到被重置前的旧值

    // 位置同步：新建 / 粘贴的节点在这里落到 Graph 记录的位置
    // ARCH-R3：渲染读取走 view（nodes_for_render 零拷贝返回 graph 内部数组 const 引用）
    for (const Node& node : view.nodes_for_render()) {
        interactorPushNodePosition(node.id, node.x, node.y);
    }
    // 宽度自适应 + 节点卡片绘制：走共享 canvas_view 只读绘制层（CEA-01 / CEA-02）
    //   —— 主题 = 主画布默认主题；运行读模型 = view.snapshot（ARCH-R3 渲染只读 ReadOnlyView）
    nc.canvas.graph        = &view.graph();
    nc.canvas.theme        = &canvasThemeMain();
    nc.canvas.run_snapshot = [&view]() -> const engine::RunSnapshot& { return view.snapshot; };
    nc.canvas.textures     = &state.textures(); // ARCH-A6：卡片缩略图（单实例缓存）
    const auto diag_t0 = std::chrono::steady_clock::now();
    updateContentWidth(view.graph(), nc.canvas);
    const auto diag_t1 = std::chrono::steady_clock::now();
    for (const Node& node : view.nodes_for_render()) {
        drawNodeBody(nc.canvas, node);
    }
    const auto diag_t2 = std::chrono::steady_clock::now();
    log_wrap_selftest(nc.canvas);                     // 折行自检（一次性：每条输出行都必须放得下）
    check_uniform_node_width(view.graph(), nc.canvas);  // 固定宽度自检（不变量：全部节点等宽）
    for (const Edge& edge : view.edges_for_render()) {
        const Node* from = view.find_node(edge.from_node);
        const Node* to   = view.find_node(edge.to_node);
        if (from == nullptr || to == nullptr) {
            continue;
        }
        const int from_index = from->portIndex(edge.from_port, PortDirection::Output);
        const int to_index   = to->portIndex(edge.to_port, PortDirection::Input);
        if (from_index < 0 || to_index < 0) {
            continue;
        }
        ed::Link(linkHandle(edge.id), pinHandle(*from, PortDirection::Output, from_index),
                 pinHandle(*to, PortDirection::Input, to_index));
    }
    const auto diag_t3 = std::chrono::steady_clock::now();

    handle_create_link(state);
    handle_delete_items(state);

    // ------------------------------------------------- 右键菜单（需挂起画布）----

    ed::Suspend();

    // POL-E-01a：右键菜单动作统一经 dispatchCommand（与「编辑」菜单 / 工具栏同一实现，
    // 消除「右键直调 EditorController → 行为与另两入口不一致」的漂移）。
    const CommandContext menu_ctx{nullptr, nullptr, &state};

    // 右键菜单按「最具体优先」串联（官方 blueprints-example 写法）：节点 > 背景。
    // 说明：连线右键菜单（「删除连线」「选中该连线」）已按需求下线，见 CHANGELOG「连线右键菜单移除」；
    //       连线删除请用「左键单击连线选中 → 工具栏【删除选中】」。
    if (ed::ShowNodeContextMenu(&nc.context_node_handle)) {
        nc.context_node         = "n" + std::to_string(nc.context_node_handle.Get());
        // POL-E-01a：右键**已在多选内**的节点 → 保留原多选（否则「打包为上级节点」
        // 永远只剩 1 个节点可选，右键入口形同虚设）；右键未选中的节点 → 收敛为单选。
        if (!state.selection().is_selected(nc.context_node)) {
            state.selection().select_node(nc.context_node); // ARCH-E4：选中独立对象
        }
        ImGui::OpenPopup("##node_menu");
    }
    else if (ed::ShowBackgroundContextMenu()) {
        nc.spawn_position = ed::ScreenToCanvas(ImGui::GetMousePos());
        ImGui::OpenPopup("##canvas_menu");
    }

    if (ImGui::BeginPopup("##canvas_menu")) {
        // POL-E-01a：撤销 / 重做 —— 谓词与「编辑」菜单同源（state.model().undo），
        // 动作走命令层（三入口同一实现）
        if (ImGui::MenuItem("撤销", nullptr, false, state.model().undo.canUndo())) {
            dispatchCommand(AppCommand::Undo, menu_ctx);
        }
        if (ImGui::MenuItem("重做", nullptr, false, state.model().undo.canRedo())) {
            dispatchCommand(AppCommand::Redo, menu_ctx);
        }
        ImGui::Separator();
        ImGui::TextDisabled("新建节点");
        ImGui::Separator();
        show_add_node_menu(state, nc.spawn_position);
        ImGui::Separator();
        // 设计 §6.1：右键画布 → 粘贴
        if (ImGui::MenuItem("粘贴", nullptr, false, state.has_clipboard())) {
            dispatchCommand(AppCommand::Paste, menu_ctx);
        }
        if (ImGui::IsItemHovered() && !state.has_clipboard()) {
            ImGui::SetTooltip("剪贴板为空：先在节点右键菜单里「复制」");
        }
        if (ImGui::MenuItem("删除选中（节点 / 连线）", nullptr, false, state.has_selection())) {
            dispatchCommand(AppCommand::DeleteSelection, menu_ctx);
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##node_menu")) {
        ImGui::TextDisabled("节点 %s", nc.context_node.c_str());
        ImGui::Separator();
        // POL-E-01a：撤销 / 重做（谓词同「编辑」菜单；动作走命令层）
        if (ImGui::MenuItem("撤销", nullptr, false, state.model().undo.canUndo())) {
            dispatchCommand(AppCommand::Undo, menu_ctx);
        }
        if (ImGui::MenuItem("重做", nullptr, false, state.model().undo.canRedo())) {
            dispatchCommand(AppCommand::Redo, menu_ctx);
        }
        ImGui::Separator();
        // 设计 §6.1：右键节点 → 复制（粘贴在画布右键菜单）
        if (ImGui::MenuItem("复制", nullptr, false, state.has_selection())) {
            dispatchCommand(AppCommand::Copy, menu_ctx);
        }
        if (ImGui::MenuItem("改名（右侧参数面板）")) {
            state.request_focus_title = true;
        }
        // CN：上级节点 → 打开独立编辑器
        {
            const engine::Node* ctx_node = state.model().graph.findNode(nc.context_node);
            if (ctx_node != nullptr && !ctx_node->composite_id.empty()) {
                ImGui::Separator();
                if (ImGui::MenuItem("编辑内部图…")) {
                    compositeEditorOpen(ctx_node->composite_id);
                }
            }
        }
        // CN-D10：右键多选 → 打包为上级节点（与「上级节点」菜单同一命令路径；
        // 谓词挡在 <2 —— 不让用户走到 ceSession().error 的「请选中至少 2 个」分支，
        // 那是上级编辑器会话的错误位，主画布调用会串台）
        {
            const bool pack_ok = state.selection().nodes().size() >= 2;
            if (ImGui::MenuItem("打包为上级节点…", nullptr, false, pack_ok)) {
                dispatchCommand(AppCommand::CompositePack, menu_ctx);
            }
            if (ImGui::IsItemHovered() && !pack_ok) {
                ImGui::SetTooltip("请先选中至少 2 个节点再打包（Ctrl 点选 / 框选）");
            }
        }
        ImGui::Separator();
        // PD-06①（FEA-M4-11 · 通用重跑）：复用上游结果，受影响节点先清结果与状态
        {
            const engine::Node* ctx_node = state.model().graph.findNode(nc.context_node);
            const bool ctx_rerun_ok =
                ctx_node != nullptr && ctx_node->state != engine::NodeState::Waiting &&
                !state.session_active();
            if (ImGui::MenuItem("重跑该节点", nullptr, false, ctx_rerun_ok)) {
                state.startRerunAsync(nc.context_node, engine::RerunScope::NodeOnly);
            }
            if (ImGui::MenuItem("重跑该节点及下游", nullptr, false, ctx_rerun_ok)) {
                state.startRerunAsync(nc.context_node, engine::RerunScope::Downstream);
            }
            if (ImGui::IsItemHovered() && ctx_node != nullptr &&
                ctx_node->state == engine::NodeState::Waiting) {
                ImGui::SetTooltip("该节点尚未运行过（状态为等待），暂不可重跑");
            }
        }
        // 删除：多选时明确「删 N 个」（标签如实，避免「删除该节点」实际删掉整批选中）
        {
            const std::size_t sel_count = state.selection().nodes().size();
            const std::string delete_label =
                sel_count > 1 ? "删除选中的 " + std::to_string(sel_count) + " 个节点"
                              : "删除该节点";
            if (ImGui::MenuItem(delete_label.c_str(), nullptr, false, state.has_selection())) {
                dispatchCommand(AppCommand::DeleteSelection, menu_ctx);
            }
        }
        ImGui::EndPopup();
    }

    ed::Resume();
    // 双击节点标题 → 参数面板聚焦标题（设计 §6.1 重命名）；CN：上级节点改为打开编辑器
    const ed::NodeId double_clicked = ed::GetDoubleClickedNode();
    if (double_clicked.Get() != 0) {
        const std::string node_id = "n" + std::to_string(double_clicked.Get());
        state.selection().select_node(node_id); // ARCH-E4：选中独立对象
        if (const engine::Node* node = state.model().graph.findNode(node_id);
            node != nullptr && !node->composite_id.empty()) {
            compositeEditorOpen(node->composite_id);
            state.setStatus("已打开上级节点编辑器：" + node->title + "（" +
                             node->composite_id + "）");
        }
        else {
            state.request_focus_title = true;
            state.setStatus("可在右侧参数面板修改标题与参数：" + node_id);
        }
    }

    sync_selection(state);
    handle_delete_key(state);

    // 视图跟随前先确认坐标正常（历史卡死根因：异常坐标 + NavigateToContent 会让视图落到天文数字区间）
    const bool positions_sane =
        view.nodes_for_render().empty() ||
        std::all_of(view.nodes_for_render().begin(), view.nodes_for_render().end(),
                    [](const Node& node) {
                        return std::isfinite(node.x) && std::isfinite(node.y) &&
                               std::fabs(node.x) < kCanvasPositionLimit &&
                               std::fabs(node.y) < kCanvasPositionLimit;
                    });

    if (nc.first_frame || state.request_navigate_to_content) {
        if (positions_sane) {
            ed::NavigateToContent(0.0f);
        }
        else {
            log::warn("[画布] 跳过视图跟随：存在异常节点坐标（已按上限夹紧）");
            state.setStatus("已跳过视图跟随（检测到异常节点坐标）");
        }
        nc.first_frame                     = false;
        state.request_navigate_to_content = false;
    }

    // P7a-17：错误条点击「定位」→ 视图**居中并放大**到选中节点（不是仅跟随全部内容）
    if (state.request_focus_selection) {
        if (positions_sane) {
            ed::NavigateToSelection(true);
        }
        state.request_focus_selection = false;
    }

    ed::End();

    // ---- M_node_expansion L3（§4.4.3）：对齐辅助 ⑤+⑥ + 小地图 ④（ed::End 之后、画布上下文失效前）----
    // CLN-10：辅助线绘制上移共享 draw_snap_lines（颜色/线宽传 canvas_theme_main() 的 snap_line）
    // CLN-09：小地图上移共享 draw_minimap（主题 + 节点块色按状态着色回调）
    {
        const SnapResult snap = apply_alignment_snap(state.model().graph, nc);
        if (snap.applied) {
            drawSnapLines(snap, ImGui::GetWindowDrawList(), canvasThemeMain().snap_line, 1.6f);
        }
        draw_minimap(view.graph(), canvasThemeMain(), nc.show_minimap, nc.minimap,
                     [](const Node& n) { return state_color(n.state); });
    }

    // CS-01：画布诊断（每 2 秒一行；把「帧预算归因 + 纹理请求/解码」写进 app.log）
    // 参数顺序必须与签名一致：(宽度重算 t0→t1, 节点循环 t1→t2, 连线+自检 t2→t3)
    log_canvas_diagnostic(state, diag_zoom, ms_between(diag_t0, diag_t1), ms_between(diag_t1, diag_t2),
                          ms_between(diag_t2, diag_t3));

    // P7a-14：空画布引导（新用户第一眼就知道下一步做什么）
    if (view.nodes_for_render().empty()) {
        ImDrawList*  draw_list   = ImGui::GetWindowDrawList();
        const ImVec2 window_pos  = ImGui::GetWindowPos();
        const ImVec2 window_size = ImGui::GetWindowSize();
        const char*  line1 = "画布是空的：从左侧「节点库」单击一个节点，或直接拖到画布";
        const char*  line2 = "也可以右键画布 → 新建节点；菜单「文件 → 示例工作流」可载入示例";
        const ImVec2 size1 = ImGui::CalcTextSize(line1);
        const ImVec2 size2 = ImGui::CalcTextSize(line2);
        const float  center_x = window_pos.x + window_size.x * 0.5f;
        const float  base_y   = window_pos.y + window_size.y * 0.42f;
        draw_list->AddText(ImVec2(center_x - size1.x * 0.5f, base_y),
                           IM_COL32(205, 205, 205, 220), line1);
        draw_list->AddText(ImVec2(center_x - size2.x * 0.5f, base_y + size1.y + 6.0f),
                           IM_COL32(150, 150, 150, 200), line2);
    }

    if (!options.show_grid) {
        ed::PopStyleColor();
    }

    sync_positions(state, nc);
    ed::SetCurrentEditor(nullptr);

    options.node_count = static_cast<int>(view.nodes_for_render().size());
    options.link_count = static_cast<int>(view.edges_for_render().size());

    ImGui::End();
}

std::string canvasAddNodeAtCenter(const std::string& type, EditorController& editor)
{
    EditorController& state = editor;
    NodeCanvasState&  nc    = state.node_canvas(); // ARCH-A6：画布跨帧状态（单实例）
    ensure_context(nc);

    ed::SetCurrentEditor(nc.context);
    const ImVec2 center_canvas = ed::ScreenToCanvas(ImGui::GetMainViewport()->GetCenter());
    ed::SetCurrentEditor(nullptr);

    state.snapshot("创建节点");
    const std::string id = state.createNode(type, center_canvas.x, center_canvas.y);
    if (!id.empty()) {
        state.selection().select_node(id); // ARCH-E4：选中独立对象
    }
    return id;
}

// CN（Composition_node）：依据规格在画布可视中心创建上级节点
std::string canvasAddCompositeAtCenter(const std::string& composite_id, EditorController& editor)
{
    EditorController& state = editor;
    NodeCanvasState&  nc    = state.node_canvas(); // ARCH-A6：画布跨帧状态（单实例）
    ensure_context(nc);

    ed::SetCurrentEditor(nc.context);
    const ImVec2 center_canvas = ed::ScreenToCanvas(ImGui::GetMainViewport()->GetCenter());
    ed::SetCurrentEditor(nullptr);

    state.snapshot("创建上级节点");
    std::string error;
    const std::string id =
        state.model().graph.addCompositeNode(composite_id, center_canvas.x, center_canvas.y, &error);
    if (!id.empty()) {
        state.selection().select_node(id); // ARCH-E4：选中独立对象
        if (const engine::Node* node = state.model().graph.findNode(id)) {
            state.setStatus("已创建上级节点：" + node->title + "（双击进入内部图编辑）");
        }
    }
    else {
        state.setStatus("创建上级节点失败：" + error);
    }
    return id;
}

}  // namespace aiwrite::ui
