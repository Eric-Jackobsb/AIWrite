#include "composite_editor.h"
#include "ce_internal.h"
#include "canvas_view.h"
#include "commands.h"      // ARCH-R4：命令路由层（快捷键表 / dispatchCommand / handleShortcut）
#include "interactor.h"     // ARCH-E5：画布交互共享层（双画布单套交互）
#include "editor_controller.h"
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
#include <string>
#include <unordered_map>
#include <vector>

namespace aiwrite::ui {
namespace ed = ax::NodeEditor;

// ==================================================== 编辑器窗口 ============
namespace {

// ------------------------------------------------------- ce 交互回调组 ------
// ARCH-E5：ce 画布与主画布共用 interactor（双画布单套交互）；模型差异
//（图源 = model().graph() / 无替换连线语义 / 局部单选）经回调注入。
// ARCH-Z2：规格标记显示名缓冲已收进 `CeSpec`（原 `ce_name_buffers()` 函数内 static 移除）。
CanvasInteract ceInteractCore()
{
    CanvasInteract interact;

    interact.resolve_pin = [](ed::PinId start_pin, ed::PinId end_pin, PinRef& from, PinRef& to) {
        const engine::Graph& graph = ceSession().model().graph();
        from = resolvePin(start_pin, graph);
        to   = resolvePin(end_pin, graph);
        return from.valid && to.valid;
    };
    interact.can_connect = [](const PinRef& from, const PinRef& to, std::string& reason,
                              bool& would_replace) {
        would_replace = false; // ce 内部图无替换语义（与主画布 canConnect 差异点）
        return ceSession().model().graph().canConnect(from.node_id, from.port_id, to.node_id,
                                                      to.port_id, &reason);
    };
    interact.on_link_accepted = [](const PinRef& from, const PinRef& to, bool) {
        ceSession().snapshot("添加内部连线");
        std::string add_error;
        composite_editor_add_edge(from.node_id, from.port_id, to.node_id, to.port_id, &add_error);
        if (!add_error.empty()) {
            ceSession().error = add_error;
        }
    };
    interact.on_link_rejected = [](const std::string& reason) {
        ceSession().error = reason;
    };
    // on_create_tip：ce 无拖拽连线提示（保持两画布差异；主画布有 L3-09）
    interact.on_delete_batch = [](const std::string& label) { ceSession().snapshot(label); };
    interact.delete_link = [](const std::string& link_id) {
        ceSession().model().graph().removeEdge(link_id);
    };
    interact.delete_node = [](const std::string& node_id) {
        ceRemoveNode(node_id);
    };
    return interact;
}

} // namespace

// ---------------------------------------------------- 命令路由层（ARCH-R4）----
// ce 的 Delete / Ctrl+S 快捷键统一走命令路由层：快捷键表（ShortcutBinding）
// + handleShortcut(CommandScope::CompositeEditor, ...) + dispatchCommand ——
// 与主画布 Delete / 菜单 / 工具栏同一实现，见 draw_composite_editor 末尾调用点。
// 本文件保留两件事：
//   * compositeEditorDeleteSelection —— ce Delete 的**执行**（无窗口/按键检测；
//     由 handleShortcut 命中后经 dispatchCommand 调用；需在 ed::Begin/End 之间）
//   * compositeEditorWindowFocused —— 主循环据此判断 Ctrl+S 作用域归属
//     （ce 窗口聚焦/悬停 → Ctrl+S 保存上级节点；否则归主窗口保存工作流）
// 窗口/按键检测统一在 handleShortcut（commands.cpp）内，本函数只做删除本身。
void compositeEditorDeleteSelection()
{
    // ARCH-E5：ed 选中读取 + 删除收敛进 interactor（GetSelected* / ClearSelection
    // 唯一封装在 interactor.cpp；一次批次只压一次撤销快照 —— 与 Delete 键原行为一致）
    const SelectionRead sel = interactorReadSelection();
    if (sel.node_ids.empty() && sel.link_ids.empty()) {
        return;
    }
    interactorDeleteSelection(ceInteractCore(), sel,
                              []() { cePushSnapshot("删除选中（Delete 键）"); });
}

// ce 窗口当前聚焦/悬停（ARCH-A5：状态挂 ceUiState().window_focused；上一帧
// draw_composite_editor 在窗口上下文内更新 —— ARCH-R4 Ctrl+S 作用域归属）
bool compositeEditorWindowFocused()
{
    return ceUiState().window_focused;
}

// ------------------------------------------------ Ctrl+S 保存（DEBT-B1 历史）----
// 用户验收记录（ToVerify #305 子项）：Ctrl+S 未保存成功、按键保存可成功 ——
// 根因 = 上级节点编辑器从未实现 Ctrl+S 快捷键（仅有「保存」按钮）。
// 原修复（2026-10-21 二修）要点（本批 ARCH-R4 已收敛进命令路由层，语义不变）：
//   ① 窗口条件放宽为「根窗口（上级节点编辑器主窗口）聚焦或悬停」——
//      IsWindowFocused/Hovered(RootAndChildWindows)；现由 draw_composite_editor
//      末尾的 handleShortcut(CompositeEditor) 调用点传入同一判定。
//   ② io.KeyCtrl（1.90 起 deprecated）改走 IsKeyChordPressed（handleShortcut 内部）。
//   ③ 其余隔离条件（无文本输入 / 无激活控件 / 无弹层 / 左键未按住）统一在
//      handleShortcut（commands.cpp）内判定。
// 保存成功/失败路径与「保存」按钮完全一致 —— 现在两者都经
// dispatchCommand(AppCommand::CompositeSave) 同一实现。

void drawCompositeEditor(const char* title, bool* open, const CeReadOnlyView& view,
                         EditorController& editor)
{
    // ARCH-R4：ce 窗口聚焦/悬停（主循环判断 Ctrl+S 作用域归属）。每帧在 ce 窗口
    // 上下文内更新；未进入窗口绘制（ce 未打开/折叠）时保持 false → Ctrl+S 归主窗口。
    ceUiState().window_focused = false;

    CeSession& session = ceSession();
    if (!session.active) {
        return;
    }
    compositeEditorTickRun(); // 试运行：泵事件 → 回写内部节点状态/结果（窗口隐藏也继续推进）
    // 点窗口 X 关闭：有未保存更改则拦截并弹确认
    if (open != nullptr && !*open) {
        if (view.dirty()) {
            *open           = true;
            session.ask_close = true;
        }
        else {
            compositeEditorForceClose();
            return;
        }
    }

    // 名称/描述输入缓冲同步到工作副本（结构类修改才进撤销栈）
    session.syncNameBuffers();

    ImGui::SetNextWindowSize(ImVec2(1000.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, open)) {
        ImGui::End();
        return;
    }
    // 窗口 Begin 成功后计算聚焦/悬停（RootAndChildWindows：顶栏 / 规格面板 / 画布都算）
    ceUiState().window_focused = interactorWindowActiveComposite(); // ARCH-E5：窗口判定收敛进 interactor

    // ---- 顶栏 ----
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("名称", session.name_buf.data(), session.name_buf.size());
    ImGui::SameLine();
    ImGui::TextDisabled("id: %s", view.id().c_str());
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
        ceUiState().request_fit = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("画布辅助")) { // G4/G5/G7：小地图 / 对齐吸附 / 网格
        ImGui::OpenPopup("##ce_canvas_assist");
    }
    if (ImGui::BeginPopup("##ce_canvas_assist")) {
        ImGui::TextDisabled("画布辅助（会话级设置）");
        ImGui::Separator();
        ImGui::Checkbox("小地图（左下角）", &ceUiState().show_minimap);
        ImGui::Checkbox("对齐吸附 + 辅助线", &ceUiState().align_snap);
        ImGui::Checkbox("网格 16px 吸附", &ceUiState().grid_snap);
        ImGui::Checkbox("网格显示", &ceUiState().show_grid);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    // 试运行「执行连线」：与主画布同机制执行内部图（暴露输入自动接测试源）
    if (!session.run_active) {
        if (ImGui::Button("执行连线")) {
            std::string run_error;
            if (compositeEditorStartRun(&run_error, editor)) {
                session.error.clear();
            }
            else {
                session.error = run_error;
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
            session.run_status = "试运行已停止";
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.29f, 0.56f, 0.89f, 1.0f), "试运行中…");
    }
    ImGui::SameLine();
    if (ImGui::Button("保存")) {
        // ARCH-R4：与 Ctrl+S 同一路径（dispatchCommand 单一实现）
        dispatchCommand(AppCommand::CompositeSave, CommandContext{nullptr, nullptr, &editor});
    }
    ImGui::SameLine();
    if (ImGui::Button("关闭")) {
        compositeEditorRequestClose();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(view.dirty() ? "● 有未保存更改" : "已保存");
    if (!session.run_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", session.run_status.c_str());
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("描述", session.desc_buf.data(), session.desc_buf.size());
    if (!session.error.empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", session.error.c_str());
    }
    ImGui::Separator();

    // ---- 主体布局：左侧画布 + 右侧规格面板 ----
    const float spec_panel_w = 320.0f;
    // ARCH-E5：ce 画布交互回调组（核心 + 选中同步）
    // ARCH-Z2：选中落点 = CeSelection（对称 SelectionState；原为本帧局部 ce_selected_node）
    CanvasInteract ce_interact = ceInteractCore();
    ce_interact.on_selection   = [](const std::vector<std::string>& node_ids,
                                  const std::vector<std::string>&) {
        // PS-09-G1：单选 → 右侧「参数编辑」区显示该节点参数（ce 独立选中，不互通主画布）
        ceSession().selection().sync(node_ids);
    };
    ImGui::BeginChild("##ce_canvas_host",
                      ImVec2(ImGui::GetContentRegionAvail().x - spec_panel_w, 0.0f));
    {
        ensureCeContext();
        ed::SetCurrentEditor(ceUiState().context);

        // 位置同步：新建节点落到工作副本记录坐标
        for (const engine::Node& n : view.nodes_for_render()) {
            interactorPushNodePosition(n.id, n.x, n.y);
        }

        blockMinimapInput(ceUiState().show_minimap, ceUiState().minimap); // G4：小地图区域输入与画布隔离（ed::Begin 之前）

        ed::Begin("##CeCanvas", ImVec2(0.0f, 0.0f));
        if (ceUiState().request_fit) { // G8：适配视图
            ed::NavigateToContent(0.0f);
            ceUiState().request_fit = false;
        }
        if (!ceUiState().show_grid) { // G7：隐藏网格（透明网格色）
            ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        }
        // CN_editor_refactor 阶段 1（CEA-02）：内部图画布改走共享绘制层 canvas_view。
        //   —— 主题 = 差异主题（CEA-D5 偏蓝配色，与主画布一眼区分）
        //   —— 运行读模型 = composite_editor_run_snapshot()（试运行结果摘要）
        //   —— 内容宽度 = 共享 update_content_width 动态宽度（原固定 160px → 与主画布同款自适应）
        CanvasContext ce_ctx;
        ce_ctx.graph        = &view.graph(); // ARCH-Z2：渲染只读视图（零拷贝）
        ce_ctx.theme        = &canvasThemeComposite();
        ce_ctx.run_snapshot = [&view]() -> const engine::RunSnapshot& { return view.snapshot; };
        ce_ctx.textures     = &editor.textures(); // ARCH-A6：卡片缩略图（与主画布共用同一缓存）
        // 内联文本编辑（CEA）：TextInput 节点的「文本内容」直接在卡片上编辑（主画布保持只读预览）。
        // 写回工作副本 + 标脏；编辑开始压「修改前」快照 → 可撤销（与右侧参数编辑区同语义）。
        ce_ctx.inline_edit_text = true;
        ce_ctx.on_param_edit_begin = []() {
            compositeEditorSnapshot("修改内部节点文本参数");
        };
        ce_ctx.on_param_edit_commit = [](const std::string& node_id, const std::string& param_id,
                                         const std::string& value) {
            CeSession& session_w = ceSession();
            if (engine::Node* node = session_w.model().graph().findNode(node_id)) {
                if (engine::Param* param = node->findParam(param_id)) {
                    param->value = value; // ParamType::Text → 字符串直接写回
                    session_w.model().markDirty();
                }
            }
        };
        updateContentWidth(view.graph(), ce_ctx);
        for (const engine::Node& n : view.nodes_for_render()) {
            drawNodeBody(ce_ctx, n);
        }
        for (const engine::Edge& e : view.edges_for_render()) {
            const engine::Node* from = view.find_node(e.from_node);
            const engine::Node* to   = view.find_node(e.to_node);
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
        // ARCH-E5：BeginCreate..EndCreate 成对契约 + 拒绝/接受语义收敛进 interactor.cpp
        interactorCreateLink(ce_interact);

        // 删除（节点连带连线；连线直接删模型）
        // ARCH-E5：BeginDelete..EndDelete 成对契约收敛进 interactor.cpp
        interactorDeleteOuter(ce_interact, "删除内部连线", "删除内部节点");

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
            ceUiState().add_filter[0] = '\0'; // G3：重新打开时清空搜索
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
                interactorClearSelection(); // 删除后清空选中，参数编辑区回落「未选中」
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
            ImGui::InputTextWithHint("##ce_add_filter", "搜索节点…", ceUiState().add_filter,
                                     sizeof(ceUiState().add_filter));
            ImGui::Separator();
            // 创建内部节点落点：背景右键那一刻的画布坐标（右键点在画布哪里，节点就落哪里）。
            const ImVec2 ce_spawn = ce_spawn_position;
            engine::registerAllNodes();
            engine::NodeRegistry& registry  = engine::NodeRegistry::instance();
            const std::string     query     = ceUiState().add_filter;
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
            ImGui::TextDisabled("重命名内部节点 %s", ceUiState().rename_id.c_str());
            ImGui::SetNextItemWidth(220.0f);
            const bool enter_pressed = ImGui::InputText(
                "##ce_rename_input", ceUiState().rename_buf.data(),
                ceUiState().rename_buf.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            if (enter_pressed) {
                ceApplyRename(); // 回车确认
            }
            if (ImGui::Button("确定")) {
                ceApplyRename();
            }
            ImGui::SameLine();
            if (ImGui::Button("取消")) {
                ceUiState().rename_id.clear();
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
        const SnapResult ce_snap = ceApplySnap(view.graph());

        // 位置写回（有变化才标记 dirty；ARCH-E5：读回经 interactorReadNodePosition，
        // 0.0f clamp = 不夹紧（保持 ce 原语义）、0.0f epsilon = 任何变化都写回）
        // 写路径：经 model().graph()（渲染读仍走 view）
        for (engine::Node& n : session.model().graph().nodes) {
            const PositionRead p = interactorReadNodePosition(n.id, n.x, n.y, 0.0f, 0.0f);
            if (p.changed && p.editor_knows) {
                n.x = p.x;
                n.y = p.y;
                session.model().markDirty();
            }
        }

        // PS-09-G1：同步内部节点选中（单选 → 右侧「参数编辑」区显示该节点参数；
        // ARCH-E5：ed 选中读取经 interactorSyncSelection，落点 = CeSelection）
        session.selection().clear();
        interactorSyncSelection(ce_interact);

        // ARCH-R4：Delete / Ctrl+S 统一走命令路由层（快捷键表 + dispatchCommand）。
        //  * 作用域 = CompositeEditor；窗口条件 = 上级节点编辑器根窗口聚焦/悬停
        //    （RootAndChildWindows：画布 / 顶栏 / 规格面板都算 —— DEBT-B1 修复语义）
        //  * 通用隔离（文本输入 / 激活控件 / 弹层 / 左键）在 handleShortcut 内判定
        //  * 与主画布 Delete / 菜单 / 工具栏同一实现（三入口收敛）
        //  * 窗口判定收敛进 interactor（ARCH-E5：interactorWindowActiveComposite）
        handleShortcut(CommandScope::CompositeEditor,
                       /*window_active=*/interactorWindowActiveComposite(),
                       CommandContext{nullptr, nullptr, &editor});

        ed::End();

        // G4 小地图 + G5 辅助线：必须在 ed::End() **之后**绘制（CEA-D2 修复）——
        // 画布内容在 ed::Begin..ed::End 内的绘制坐标会叠加画布平移/缩放变换（观察表现为
        // 「小地图随画布跑、嵌在画布内」）；移到 ed::End 之后即回到宿主窗口的屏幕空间，
        // 小地图固定锚定视图左下角、不随平移缩放移动。时序与主画布一致
        // （node_canvas.cpp：ed::End() → apply_alignment_snap + draw_minimap →
        //  SetCurrentEditor(nullptr)）。
        // 注意：此时 ed::SetCurrentEditor 仍指向 ceUiState().context，共享 draw_minimap 内的
        //       GetNodeSize / NavigateToCenter 才能正常取到节点尺寸与发起视口导航。
        // CLN-10/09：辅助线与小地图上移共享（draw_snap_lines / draw_minimap），
        // 主题用 canvas_theme_composite()、节点块色固定蓝紫（保留两画布差异）。
        drawSnapLines(ce_snap, ImGui::GetWindowDrawList(), IM_COL32(200, 80, 220, 255), 1.0f);
        draw_minimap(view.graph(), canvasThemeComposite(), ceUiState().show_minimap,
                     ceUiState().minimap, [](const engine::Node&) { return IM_COL32(190, 195, 220, 200); });
        if (!ceUiState().show_grid) {
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

        if (view.nodes_for_render().empty()) {
            ImGui::TextWrapped("内部图为空：右键画布添加内部节点。");
        }

        // 渲染读走 view（ARCH-Z2 判据 7）；标记写走 ceSession().spec_marks()（单一实现）
        for (const engine::Node& node : view.nodes_for_render()) {
            ImGui::PushID(node.id.c_str());
            const std::string header = node.title + "（" + node.id + "）";
            if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                // 输入 → 对外输入（有内部入边的端口不允许标记）
                for (const engine::Port& port : node.inputs) {
                    const bool has_internal_in =
                        view.graph().findEdgeIntoInput(node.id, port.id) != nullptr;
                    bool exposed = ceSession().spec_marks().has_input(node.id, port.id);
                    ImGui::PushID(("in" + port.id).c_str());
                    if (ImGui::Checkbox("对外输入", &exposed)) {
                        compositeEditorSnapshot("标记对外输入 " + port.id);
                        ceSession().spec_marks().toggle_input(node.id, port.id, exposed);
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s%s", port.display_name.c_str(),
                                        has_internal_in ? "（有内部连线）" : "");
                    ImGui::PopID();
                }

                // 输出 → 对外输出（单选）
                for (const engine::Port& port : node.outputs) {
                    const bool selected = ceSession().spec_marks().has_output(node.id, port.id);
                    ImGui::PushID(("out" + port.id).c_str());
                    if (ImGui::RadioButton("对外输出", selected)) {
                        if (!selected) {
                            compositeEditorSnapshot("标记对外输出 " + port.id);
                            ceSession().spec_marks().set_output(node.id, port.id);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", port.display_name.c_str());
                    ImGui::PopID();
                }
                // 参数 → 暴露（外部参数 id 唯一：同一 param_id 只能暴露一次；冲突项禁勾选并提示，
                // 避免「勾选后又被静默取消」——与 validate_composite_spec 的全局唯一校验同口径）
                for (const engine::Param& param : node.params) {
                    bool exposed = ceSession().spec_marks().has_exposed(node.id, param.id);
                    // 唯一性冲突：该 param_id 已被**其它**内部节点暴露（外部实例参数 id 会撞车）
                    const bool id_conflict =
                        !exposed && ceSession().spec_marks().param_id_conflict(node.id, param.id);
                    ImGui::PushID(("p" + param.id).c_str());
                    if (id_conflict) {
                        ImGui::BeginDisabled();
                    }
                    if (ImGui::Checkbox("暴露参数", &exposed)) {
                        compositeEditorSnapshot("切换暴露参数 " + param.id);
                        // 冲突项已禁用，到不了这里；其余场景无同 id 冲突
                        ceSession().spec_marks().toggle_exposed(node.id, param.id, exposed);
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

        // 外部显示名编辑（输入/输出端口）—— 写路径经 CeSpec（缓冲 + 写回单一实现）
        ImGui::Separator();
        ImGui::TextDisabled("外部显示名（可改）");
        for (engine::CompositePortRef& ref : ceSession().model().spec().inputs) {
            const std::string      key = ref.internal_node + "|" + ref.internal_port + "|in";
            std::array<char, 128>& buf = ceSession().spec_marks().name_buffer(key, ref.display_name);
            ImGui::PushID(key.c_str());
            if (ImGui::InputText("##dn", buf.data(), buf.size())) {
                ceSession().spec_marks().set_input_display_name(ref.internal_node, ref.internal_port,
                                                                buf.data());
                ceSession().model().markDirty();
            }
            ImGui::PopID();
        }
        for (engine::CompositePortRef& ref : ceSession().model().spec().outputs) {
            const std::string      key = ref.internal_node + "|" + ref.internal_port + "|out";
            std::array<char, 128>& buf = ceSession().spec_marks().name_buffer(key, ref.display_name);
            ImGui::PushID(key.c_str());
            if (ImGui::InputText("##dn", buf.data(), buf.size())) {
                ceSession().spec_marks().set_output_display_name(ref.internal_node,
                                                                 ref.internal_port, buf.data());
                ceSession().model().markDirty();
            }
            ImGui::PopID();
        }
        if (ceSession().model().spec().inputs.empty() && ceSession().model().spec().outputs.empty()) {
            ImGui::TextDisabled("（尚无对外端口）");
        }

        // ---- PS-09-G1：参数编辑区（选中内部节点 → 复用公共 draw_param_widget）----
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.75f, 0.9f, 0.7f, 1.0f), "参数编辑");
        ImGui::TextDisabled("在画布中单击一个内部节点，在此编辑其参数值。");
        const std::string& ce_selected_id = view.selection.single(); // ARCH-Z2：选中来自 CeSelection
        if (ce_selected_id.empty()) {
            ImGui::TextDisabled("（未选中节点）");
        }
        else {
            // 写路径：参数控件直接写回工作副本（渲染读仍走 view）
            engine::Node* sel = session.model().graph().findNode(ce_selected_id);
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
                        session.model().markDirty(); // 参数值已写回工作副本（cePushSnapshot 亦置 dirty）
                    }
                    ImGui::Spacing();
                }
            }
        }
    }
    ImGui::EndChild();


    // ---- 关闭确认弹窗 ----
    if (session.ask_close) {
        ImGui::OpenPopup("##ce_close_confirm");
        session.ask_close = false;
    }
    if (ImGui::BeginPopupModal("未保存的更改", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("上级节点「%s」有未保存的更改。", view.name().c_str());
        ImGui::Spacing();
        bool closed  = false;
        bool discard = false;
        if (ImGui::Button("保存并关闭", ImVec2(120.0f, 0.0f))) {
            std::string error;
            if (compositeEditorSave(&error, editor)) {
                closed = true;
            }
            else {
                session.error = error;
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
        if (closed && (discard || !view.dirty())) {
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

// 对话框状态（开关 / 名称输入缓冲 / 展示用错误文案 / 调试自动确认）见
// `CompositeDialogsState`（composite_editor.h · ARCH-A6）：原先是 7 条文件级 static，
// 现在由 `EditorController::composite_dialogs()` 持有并逐帧传入。

// 点「创建并打开编辑器」的动作：组装空白规格 → 落盘 JSON → 走标准「打开」路径读取。
// （不创建无文件支撑的内存会话，避免依赖 imgui-node-editor 上下文的时序。）
// 返回 true = 已创建并打开（调用方应关闭对话框）；false = 失败（dlg.new_error 已填）。
bool create_and_open_new_composite(CompositeDialogsState& dlg)
{
    dlg.new_error.clear();
    std::string name = dlg.new_name;
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
        dlg.new_error = "创建失败：" + save_error;
        return false; // 保留弹窗展示错误，不关闭
    }
    compositeEditorOpen(spec.id);
    return true;
}

void run_new_dialog(CompositeDialogsState& dlg)
{
    if (!dlg.new_open) {
        return;
    }
    ImGui::OpenPopup("新建上级节点##ce_new");
    if (!ImGui::BeginPopupModal("新建上级节点##ce_new", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }
    if (dlg.new_auto_accept) {
        dlg.new_auto_accept = false;
        if (create_and_open_new_composite(dlg)) {
            ImGui::CloseCurrentPopup();
            dlg.new_open = false;
        }
    }
    ImGui::TextWrapped("新建一个空的上级节点：先在本地落盘一份空白规格 JSON，随后打开编辑器"
                       "添加内部节点、标记端口与参数。");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("名称", dlg.new_name, sizeof(dlg.new_name));
    bool closed = false;
    if (ImGui::Button("创建并打开编辑器", ImVec2(180.0f, 0.0f))) {
        closed = create_and_open_new_composite(dlg);
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(80.0f, 0.0f))) {
        closed = true;
    }
    if (!dlg.new_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", dlg.new_error.c_str());
    }
    if (closed) {
        ImGui::CloseCurrentPopup();
        dlg.new_open = false;
    }
    ImGui::EndPopup();
}

void run_pack_dialog(EditorController& editor, CompositeDialogsState& dlg)
{
    if (!dlg.pack_open) {
        return;
    }
    ImGui::OpenPopup("打包为上级节点##ce_pack");
    if (!ImGui::BeginPopupModal("打包为上级节点##ce_pack", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }
    const int count = static_cast<int>(editor.selection().nodes().size()); // ARCH-E4
    ImGui::TextWrapped("把选中的 %d 个节点打包为上级节点。\n边界输入/输出、暴露参数将自动识别（可在编辑器调整）。",
                       count);
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("名称", dlg.pack_name, sizeof(dlg.pack_name));

    if (ImGui::Button("打包", ImVec2(100.0f, 0.0f))) {
        dlg.pack_error.clear();
        const std::string id =
            compositePackSelection(std::string(dlg.pack_name), &dlg.pack_error, editor);
        if (!id.empty()) {
            ImGui::CloseCurrentPopup();
            dlg.pack_open = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(80.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
        dlg.pack_open = false;
    }
    if (!dlg.pack_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", dlg.pack_error.c_str());
    }
    ImGui::EndPopup();
}

} // namespace

void compositeNewDialog(EditorController& editor)
{
    CompositeDialogsState& dlg = editor.composite_dialogs(); // ARCH-A6：对话框状态
    if (!dlg.new_open) {
        dlg.new_open = true;
        dlg.new_name[0]     = '\0';
        dlg.new_error.clear(); // 重新打开时清空上次失败原因
    }
}

// [调试] --auto-open-composite：打开「新建上级节点」对话框，并在其首次可见帧
// 自动执行「创建并打开编辑器」点击（与手工操作同路径，便于复现窗口栈断言）。
void compositeEditorTestDriveNewDialog(EditorController& editor)
{
    CompositeDialogsState& dlg = editor.composite_dialogs(); // ARCH-A6：对话框状态
    if (!dlg.new_open) {
        dlg.new_open        = true;
        dlg.new_name[0]            = '\0';
        dlg.new_error.clear();
        dlg.new_auto_accept = true;
    }
}

void compositePackDialog(EditorController& editor)
{
    CompositeDialogsState& dlg = editor.composite_dialogs(); // ARCH-A6：对话框状态
    if (!dlg.pack_open) {
        dlg.pack_open = true;
        dlg.pack_name[0]     = '\0';
        dlg.pack_error.clear(); // 重新打开时清空上次失败原因
        if (editor.selection().nodes().size() >= 2) { // ARCH-E4：选中独立对象
            std::snprintf(dlg.pack_name, sizeof(dlg.pack_name), "组合节点");
        }
        else {
            ceSession().error = "请先在画布上选中至少 2 个节点再打包";
            dlg.pack_open             = false;
        }
    }
}

void drawCompositeDialogs(EditorController& editor)
{
    CompositeDialogsState& dlg = editor.composite_dialogs(); // ARCH-A6：对话框状态
    run_new_dialog(dlg);
    run_pack_dialog(editor, dlg);
}


} // namespace aiwrite::ui