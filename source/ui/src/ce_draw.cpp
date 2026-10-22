#include "composite_editor.h"
#include "ce_internal.h"
#include "canvas_view.h"
#include "editor_state.h"
#include "log.h"
#include "node_registry.h"
#include "nodes.h"
#include "property_panel.h"
#include "string_util.h"
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

// ---------------------------------------------------- Delete 键（打磨）----
// 与主画布一致：画布聚焦/悬停 + 无文本输入 + 无激活控件/弹出层 + 左键未按住时，
// Delete 键删除选中的内部节点与连线（一次批次只压一次撤销快照；ce_push_snapshot 内含 dirty 标记）。
void ce_handle_delete_key(CompositeEditorState& state)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        return;
    }
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        !ImGui::IsWindowHovered()) {
        return;
    }
    if (!ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        return;
    }

    const int count = ed::GetSelectedObjectCount();
    if (count <= 0) {
        return;
    }
    std::vector<ed::NodeId> node_handles(static_cast<std::size_t>(count));
    const int node_count = ed::GetSelectedNodes(node_handles.data(), count);
    std::vector<ed::LinkId> link_handles(static_cast<std::size_t>(count));
    const int link_count = ed::GetSelectedLinks(link_handles.data(), count);
    if (node_count == 0 && link_count == 0) {
        return;
    }

    cePushSnapshot("删除选中（Delete 键）"); // 一次批次只压一次快照
    for (int i = 0; i < node_count; ++i) {
        ceRemoveNode(state,
                       "n" + std::to_string(node_handles[static_cast<std::size_t>(i)].Get()));
    }
    for (int i = 0; i < link_count; ++i) {
        state.working.internal_graph.removeEdge(
            "e" + std::to_string(link_handles[static_cast<std::size_t>(i)].Get()));
    }
    ed::ClearSelection(); // 清掉内部选中 → 右侧「参数编辑」回落「未选中」（下一帧再同步）
}

// ------------------------------------------------ Ctrl+S 保存（DEBT-B1）----
// 用户验收记录（ToVerify #305 子项）：Ctrl+S 未保存成功、按键保存可成功 ——
// 根因 = 上级节点编辑器从未实现 Ctrl+S 快捷键（仅有「保存」按钮）。
// 修复（2026-10-21 复测静默失败 → 二修）：
//   ① 原隔离条件「IsWindowFocused(ChildWindows) || IsWindowHovered()」只认画布容器
//      （##ce_canvas_host child），而 Ctrl+S 的使用场景是「编辑完按保存」——
//      此时鼠标/焦点常在右侧规格面板或顶栏，不在画布上 → 条件拦截 → 静默失败。
//      放宽为「根窗口（上级节点编辑器主窗口）聚焦或悬停」：
//        * IsWindowFocused(RootAndChildWindows)：键盘焦点在主窗口或任一子窗口内；
//        * IsWindowHovered(RootAndChildWindows) ：鼠标在主窗口任意区域。
//   ② io.KeyCtrl（1.90 起 deprecated 的兼容字段）改走新 API ImGui::IsKeyDown(ImGuiMod_Ctrl)，
//      不依赖废弃字段的同步路径，与 backend 的 AddKeyEvent(ImGuiMod_Ctrl) 直接对齐。
//   ③ 保留与 Delete 键一致的其余隔离条件（无文本输入 / 无激活控件 / 无弹层 / 左键未按住）。
// 二修验证（2026-10-21 真机实测）：SendKeys 注入真实 Ctrl+S，诊断日志确认
//   Focused=1 Hovered=1 Ctrl=1 → 保存函数真实执行（compositeEditorSave 失败路径）
//   —— 触发链路完整；诊断日志已清理（本次提交不留残留）。
// 保存成功/失败路径与「保存」按钮完全一致（compositeEditorSave → 顶栏 dirty 指示器 /
// state.error 红色提示反馈）。
void ce_handle_save_key(CompositeEditorState& state)
{
    const ImGuiIO& io = ImGui::GetIO();

    if (io.WantTextInput || ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        return;
    }
    // 窗口条件：放宽为根窗口（主窗口 CeCanvas 聚焦/悬停），见函数头注释 ①。
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
        return;
    }
    if (!(ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_S, false))) {
        return;
    }

    std::string error;
    if (compositeEditorSave(&error)) {
        log::info("[上级节点] 已保存 " + state.working.id + "（" + state.working.name + "）");
    } else {
        log::warn("[上级节点] 保存失败: " + error);
    }
}

void drawCompositeEditor(const char* title, bool* open)
{
    CompositeEditorState& state = compositeEditorState();
    if (!state.active) {
        return;
    }
    compositeEditorTickRun(); // 试运行：泵事件 → 回写内部节点状态/结果（窗口隐藏也继续推进）
    // 点窗口 X 关闭：有未保存更改则拦截并弹确认
    if (open != nullptr && !*open) {
        if (state.dirty) {
            *open            = true;
            state.ask_close  = true;
        }
        else {
            compositeEditorForceClose();
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
        compositeEditorUndoOnce();
    }
    ImGui::SameLine();
    if (ImGui::Button("重做")) {
        compositeEditorRedoOnce();
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
    // 试运行「执行连线」：与主画布同机制执行内部图（暴露输入自动接测试源）
    if (!state.run_active) {
        if (ImGui::Button("执行连线")) {
            std::string run_error;
            if (compositeEditorStartRun(&run_error)) {
                state.error.clear();
            }
            else {
                state.error = run_error;
                log::warn("[上级节点] 试运行启动失败: " + run_error);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("清空结果")) {
            compositeEditorClearRun();
        }
    }
    else {
        if (ImGui::Button("停止")) {
            compositeEditorStopRun();
            state.run_status = "试运行已停止";
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.29f, 0.56f, 0.89f, 1.0f), "试运行中…");
    }
    ImGui::SameLine();
    if (ImGui::Button("保存")) {
        std::string error;
        if (compositeEditorSave(&error)) {
            log::info("[上级节点] 已保存 " + state.working.id + "（" + state.working.name + "）");
        }
        else {
            log::warn("[上级节点] 保存失败: " + error);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("关闭")) {
        compositeEditorRequestClose();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(state.dirty ? "● 有未保存更改" : "已保存");
    if (!state.run_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", state.run_status.c_str());
    }
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
        ensureCeContext();
        ed::SetCurrentEditor(g_ce_context);

        // 位置同步：新建节点落到 working 记录坐标
        for (const engine::Node& n : state.working.internal_graph.nodes) {
            ed::SetNodePosition(nodeHandle(n.id), ImVec2(n.x, n.y));
        }

        blockMinimapInput(g_ce_show_minimap, g_ce_minimap); // G4：小地图区域输入与画布隔离（ed::Begin 之前）

        ed::Begin("##CeCanvas", ImVec2(0.0f, 0.0f));
        if (g_ce_request_fit) { // G8：适配视图
            ed::NavigateToContent(0.0f);
            g_ce_request_fit = false;
        }
        if (!g_ce_show_grid) { // G7：隐藏网格（透明网格色）
            ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        }
        // CN_editor_refactor 阶段 1（CEA-02）：内部图画布改走共享绘制层 canvas_view。
        //   —— 主题 = 差异主题（CEA-D5 偏蓝配色，与主画布一眼区分）
        //   —— 运行读模型 = composite_editor_run_snapshot()（试运行结果摘要）
        //   —— 内容宽度 = 共享 update_content_width 动态宽度（原固定 160px → 与主画布同款自适应）
        CanvasContext ce_ctx;
        ce_ctx.graph        = &state.working.internal_graph;
        ce_ctx.theme        = &canvasThemeComposite();
        ce_ctx.run_snapshot = []() -> const engine::RunSnapshot& { return compositeEditorRunSnapshot(); };
        // 内联文本编辑（CEA）：TextInput 节点的「文本内容」直接在卡片上编辑（主画布保持只读预览）。
        // 写回工作副本 + 标脏；编辑开始压「修改前」快照 → 可撤销（与右侧参数编辑区同语义）。
        ce_ctx.inline_edit_text = true;
        ce_ctx.on_param_edit_begin = []() {
            compositeEditorSnapshot("修改内部节点文本参数");
        };
        ce_ctx.on_param_edit_commit = [](const std::string& node_id, const std::string& param_id,
                                         const std::string& value) {
            CompositeEditorState& st = compositeEditorState();
            if (engine::Node* node = st.working.internal_graph.findNode(node_id)) {
                if (engine::Param* param = node->findParam(param_id)) {
                    param->value = value; // ParamType::Text → 字符串直接写回
                    st.dirty    = true;
                }
            }
        };
        updateContentWidth(state.working.internal_graph, ce_ctx);
        for (const engine::Node& n : state.working.internal_graph.nodes) {
            drawNodeBody(ce_ctx, n);
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
            ed::Link(linkHandle(e.id),
                     pinHandle(*from, engine::PortDirection::Output, fi),
                     pinHandle(*to, engine::PortDirection::Input, ti));
        }

        // 拖出连线（类型不符变红被拒）
        const bool ce_creating = ed::BeginCreate(ImVec4(0.29f, 0.56f, 0.89f, 1.0f), 2.0f);
        if (ce_creating) {
            ed::PinId start_pin;
            ed::PinId end_pin;
            if (ed::QueryNewLink(&start_pin, &end_pin) && start_pin && end_pin) {
                PinRef from = resolvePin(start_pin, state.working.internal_graph);
                PinRef to   = resolvePin(end_pin, state.working.internal_graph);
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
                        compositeEditorSnapshot("添加内部连线");
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
                    compositeEditorSnapshot("删除内部连线");
                    state.working.internal_graph.removeEdge("e" + std::to_string(deleted_link.Get()));
                }
            }
            ed::NodeId deleted_node;
            while (ed::QueryDeletedNode(&deleted_node)) {
                if (ed::AcceptDeletedItem()) {
                    compositeEditorDeleteNode("n" + std::to_string(deleted_node.Get()));
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
                ceOpenRename(ce_context_node);
            }
            if (ImGui::MenuItem("删除内部节点")) {
                compositeEditorDeleteNode(ce_context_node);
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
            // 创建内部节点落点：背景右键那一刻的画布坐标（右键点在画布哪里，节点就落哪里）。
            const ImVec2 ce_spawn = ce_spawn_position;
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
                    if (utils::findCi(def->display_name, query) == std::string::npos &&
                        utils::findCi(def->type, query) == std::string::npos) {
                        continue;
                    }
                    any = true;
                    if (ImGui::MenuItem(def->display_name.c_str())) {
                        compositeEditorAddNode(def->type, ce_spawn.x, ce_spawn.y);
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
                                compositeEditorAddNode(def->type, ce_spawn.x, ce_spawn.y);
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
                ceApplyRename(); // 回车确认
            }
            if (ImGui::Button("确定")) {
                ceApplyRename();
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
            ceOpenRename("n" + std::to_string(ce_double_clicked.Get()));
        }
        ed::Resume();

        // G5/G7：拖动中的对齐吸附（辅助线）+ 网格 16px 吸附
        const SnapResult ce_snap = ceApplySnap(state.working.internal_graph);

        // 位置写回（有变化才标记 dirty）
        for (engine::Node& n : state.working.internal_graph.nodes) {
            const ImVec2 p = ed::GetNodePosition(nodeHandle(n.id));
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

        // Delete 键：删除画布中选中的内部节点/连线（与主画布一致的快捷键）
        ce_handle_delete_key(state);
        // Ctrl+S：保存上级节点（DEBT-B1 修复 · 与「保存」按钮同一路径）
        ce_handle_save_key(state);

        ed::End();

        // G4 小地图 + G5 辅助线：必须在 ed::End() **之后**绘制（CEA-D2 修复）——
        // 画布内容在 ed::Begin..ed::End 内的绘制坐标会叠加画布平移/缩放变换（观察表现为
        // 「小地图随画布跑、嵌在画布内」）；移到 ed::End 之后即回到宿主窗口的屏幕空间，
        // 小地图固定锚定视图左下角、不随平移缩放移动。时序与主画布一致
        // （node_canvas.cpp：ed::End() → apply_alignment_snap + draw_minimap →
        //  SetCurrentEditor(nullptr)）。
        // 注意：此时 ed::SetCurrentEditor 仍指向 g_ce_context，共享 draw_minimap 内的
        //       GetNodeSize / NavigateToCenter 才能正常取到节点尺寸与发起视口导航。
        // CLN-10/09：辅助线与小地图上移共享（draw_snap_lines / draw_minimap），
        // 主题用 canvas_theme_composite()、节点块色固定蓝紫（保留两画布差异）。
        drawSnapLines(ce_snap, ImGui::GetWindowDrawList(), IM_COL32(200, 80, 220, 255), 1.0f);
        draw_minimap(state.working.internal_graph, canvasThemeComposite(), g_ce_show_minimap,
                     g_ce_minimap, [](const engine::Node&) { return IM_COL32(190, 195, 220, 200); });
        if (!g_ce_show_grid) {
            ed::PopStyleColor(); // 与 ed::Begin 后的 PushStyleColor 配对（主画布同款：ed::End 后 Pop）
        }

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
                        compositeEditorSnapshot("标记对外输入 " + port.id);
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
                            compositeEditorSnapshot("标记对外输出 " + port.id);
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
                // 参数 → 暴露（外部参数 id 唯一：同一 param_id 只能暴露一次；冲突项禁勾选并提示，
                // 避免「勾选后又被静默取消」——与 validate_composite_spec 的全局唯一校验同口径）
                for (engine::Param& param : node.params) {
                    bool exposed = false;
                    for (const engine::CompositeParamRef& ref : state.working.exposed_params) {
                        if (ref.internal_node == node.id && ref.param_id == param.id) {
                            exposed = true;
                        }
                    }
                    // 唯一性冲突：该 param_id 已被**其它**内部节点暴露（外部实例参数 id 会撞车）
                    const bool id_conflict =
                        !exposed &&
                        std::any_of(state.working.exposed_params.begin(),
                                    state.working.exposed_params.end(),
                                    [&node, &param](const engine::CompositeParamRef& r) {
                                        return r.param_id == param.id && r.internal_node != node.id;
                                    });
                    ImGui::PushID(("p" + param.id).c_str());
                    if (id_conflict) {
                        ImGui::BeginDisabled();
                    }
                    if (ImGui::Checkbox("暴露参数", &exposed)) {
                        compositeEditorSnapshot("切换暴露参数 " + param.id);
                        state.working.exposed_params.erase(
                            std::remove_if(state.working.exposed_params.begin(),
                                           state.working.exposed_params.end(),
                                           [&node, &param](const engine::CompositeParamRef& r) {
                                               return r.internal_node == node.id &&
                                                      r.param_id == param.id;
                                           }),
                            state.working.exposed_params.end());
                        if (exposed) {
                            // 冲突项已禁用，到不了这里；其余场景无同 id 冲突，直接入列
                            engine::CompositeParamRef ref;
                            ref.internal_node = node.id;
                            ref.param_id      = param.id;
                            ref.display_name  = param.display_name;
                            state.working.exposed_params.push_back(std::move(ref));
                        }
                    }
                    if (id_conflict) {
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s（id 冲突）", param.display_name.c_str());
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("参数 id「%s」已被其它内部节点暴露：外部参数 id 必须唯一，"
                                              "请先取消对方的暴露",
                                              param.id.c_str());
                        }
                        ImGui::PopID();
                        continue;
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
                        compositeEditorSnapshot("修改内部节点参数 " + sel->id + "." + param.id);
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
            if (compositeEditorSave(&error)) {
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
            compositeEditorForceClose();
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
    spec.id   = engine::CompositeRegistry::instance().makeId();
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
    compositeEditorOpen(spec.id);
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
            compositePackSelection(std::string(g_pack_name), &g_pack_error);
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

void compositeNewDialog()
{
    if (!g_new_dialog_open) {
        g_new_dialog_open = true;
        g_new_name[0]     = '\0';
        g_new_error.clear(); // 重新打开时清空上次失败原因
    }
}

// [调试] --auto-open-composite：打开「新建上级节点」对话框，并在其首次可见帧
// 自动执行「创建并打开编辑器」点击（与手工操作同路径，便于复现窗口栈断言）。
void compositeEditorTestDriveNewDialog()
{
    if (!g_new_dialog_open) {
        g_new_dialog_open        = true;
        g_new_name[0]            = '\0';
        g_new_error.clear();
        g_new_dialog_auto_accept = true;
    }
}

void compositePackDialog()
{
    if (!g_pack_dialog_open) {
        g_pack_dialog_open = true;
        g_pack_name[0]     = '\0';
        g_pack_error.clear(); // 重新打开时清空上次失败原因
        if (editor().selected_nodes.size() >= 2) {
            std::snprintf(g_pack_name, sizeof(g_pack_name), "组合节点");
        }
        else {
            compositeEditorState().error = "请先在画布上选中至少 2 个节点再打包";
            g_pack_dialog_open             = false;
        }
    }
}

void drawCompositeDialogs()
{
    run_new_dialog();
    run_pack_dialog();
}


} // namespace aiwrite::ui
