#include "node_canvas.h"

#include "composite.h"        // CN：上级节点实例识别 / 创建
#include "canvas_view.h"      // CN_editor_refactor：共享只读绘制层（CEA-01 / CEA-02）
#include "commands.h"         // ARCH-R4：命令路由层（快捷键表 / handleShortcut）
#include "composite_editor.h" // CN：双击/右键打开上级节点编辑器
#include "node_registry.h"
#include "provider_resolve.h"
#include "editor_state.h"
#include "output_panel.h"
#include "theme.h"
#include "config.h"
#include "asset_store.h" // P7a-16：图片卡片解析资源令牌
#include "log.h"
#include "paths.h"
#include "texture_cache.h"  // P7a-16：节点卡片缩略图 + 格式徽标

#include <imgui.h>
#include <imgui_node_editor.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace aiwrite::ui {
namespace {

namespace ed = ax::NodeEditor;

using engine::Edge;
using engine::Graph;
using engine::Node;
using engine::NodeCategory;
using engine::NodeState;
using engine::Port;
using engine::PortDirection;
using engine::PortType;

ed::EditorContext* g_context             = nullptr;
bool               g_first_frame         = true;
bool               g_drag_in_progress    = false; // 一次拖拽只压一次快照
bool               g_delete_in_progress  = false; // 一次删除批次只压一次快照

// --------------------------------------------- 画布按键绑定（imgui-node-editor）---
// imgui-node-editor v0.9.3 的 Config 默认值（third_party/.../imgui_node_editor.h:120-123）：
//   DragButtonIndex = 0（左键拖节点）· SelectButtonIndex = 0（左键框选）
//   NavigateButtonIndex = 1（右键平移）· ContextMenuButtonIndex = 1（右键菜单）
// 本项目**不改 third_party 源码**，只在「Alt 按住」期间用公开 API `ed::GetConfig()`
// 临时改写绑定：平移键 → 左键；拖节点 / 框选 → 中键位（2，本项目用不到中键）。
// 于是 Alt+左键拖拽**只平移**，不会顺手移动节点或拉出框选矩形；Alt 一松开立即还原。
constexpr int kNodeEditorDragButton   = ImGuiMouseButton_Left;   // 0：左键拖拽节点
constexpr int kNodeEditorSelectButton = ImGuiMouseButton_Left;   // 0：左键框选
constexpr int kNodeEditorNavButton    = ImGuiMouseButton_Right;  // 1：右键平移画布
constexpr int kAltPanParkedButton     = ImGuiMouseButton_Middle; // 2：Alt 期间「拖节点/框选」的停机位
bool          g_alt_pan_active        = false;                   // 当前是否处于 Alt 平移模式（日志/提示用）

// 颜色（分类 / 端口类型 / 节点状态）统一由 ui/theme.h 提供（设计 §14.3 / §4.5 / §4.6）

// 手柄映射（node_handle / link_handle / pin_handle / resolve_pin / PinRef）
// 已上移 canvas_view.{h,cpp}（CN_editor_refactor CEA-01：主画布与上级编辑器共用
// 同一套整数手柄，避免映射逻辑漂移）。

// ------------------------------------------------------------- 初始化 -------
// 设计 §14.4：画布/节点/边框/选中 配色 —— 由共享 canvas_view 主题统一管理（CEA-D5）。
// 主画布 = canvas_theme_main()（现状配色，零行为变更）；样式按编辑器上下文隔离。

// ---------------------------------------------------------- 设置文件净化 -----
// node-editor 会把节点位置与视图（scroll / visible_rect / zoom）持久化到
// ~/.brain-ai/node_editor.json 并在启动时恢复。若其中残留损坏数值
// （例如 ±2^31 的坐标、2e-7 的缩放 —— 历史上"复制粘贴"脏值事故写过一次），
// 编辑器每帧的网格与剔除计算会爆量，表现为 **CPU 单核打满 + 界面无响应**。
// 这里在创建编辑器之前校验一次：异常则备份改名，让编辑器以默认视图重建。
constexpr double kSettingsNumberLimit = 1000000.0;

bool settings_numbers_sane(const nlohmann::json& value)
{
    if (value.is_number()) {
        const double number = value.is_number_float() ? value.get<double>()
                                                      : static_cast<double>(value.get<long long>());
        return std::isfinite(number) && std::fabs(number) <= kSettingsNumberLimit;
    }
    if (value.is_object() || value.is_array()) {
        for (const auto& item : value) {
            if (!settings_numbers_sane(item)) {
                return false;
            }
        }
    }
    return true;
}

bool editor_settings_sane(const nlohmann::json& settings)
{
    if (!settings.is_object() || !settings_numbers_sane(settings)) {
        return false;
    }
    // 缩放必须落在编辑器允许的包络内（本项目配置 0.1x ~ 4.0x，留出余量）
    const auto view = settings.find("view");
    if (view != settings.end() && view->is_object()) {
        const auto zoom = view->find("zoom");
        if (zoom != view->end()) {
            if (!zoom->is_number()) {
                return false;
            }
            const double value = zoom->get<double>();
            if (!std::isfinite(value) || value < 0.05 || value > 10.0) {
                return false;
            }
        }
    }
    return true;
}

void sanitize_editor_settings(const std::filesystem::path& file)
{
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        return;
    }

    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return;
    }
    const std::string raw((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();

    bool sane = false;
    try {
        sane = editor_settings_sane(nlohmann::json::parse(raw));
    }
    catch (const std::exception& ex) {
        log::warn(std::string("[画布] 编辑器设置文件解析失败: ") + ex.what());
        sane = false;
    }

    if (sane) {
        return;
    }

    char stamp[32] = {};
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_s(&local, &now);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);

    const std::filesystem::path backup =
        file.parent_path() / ("node_editor_bad_" + std::string(stamp) + ".json");
    std::filesystem::rename(file, backup, error);
    if (error) {
        std::filesystem::remove(file, error);
        log::warn("[画布] 检测到损坏的编辑器设置（坐标/缩放异常），已删除并重建默认视图");
    }
    else {
        log::warn("[画布] 检测到损坏的编辑器设置（坐标/缩放异常），已备份为 " +
                  backup.filename().string() + " 并重建默认视图");
    }
}

void ensure_context()
{
    if (g_context != nullptr) {
        return;
    }

    static std::string settings_file = (paths::dataRoot() / "node_editor.json").string();

    // 启动前净化：损坏的坐标/视图会让 node-editor 的网格计算爆量（CPU 打满、界面无响应）
    sanitize_editor_settings(settings_file);

    ed::Config config;
    config.SettingsFile = settings_file.c_str();
    // 设计 §6.4：缩放范围 0.1x ~ 4.0x
    const float levels[] = {0.1f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
    config.CustomZoomLevels.resize(IM_ARRAYSIZE(levels));
    for (int i = 0; i < IM_ARRAYSIZE(levels); ++i) {
        config.CustomZoomLevels[i] = levels[i];
    }

    g_context = ed::CreateEditor(&config);
    log::info("[画布] 编辑器上下文已创建");

    // 注意：GetStyle/PushStyleColor 作用于"当前编辑器"，必须先 SetCurrentEditor
    ed::SetCurrentEditor(g_context);
    canvasApplyEditorStyle(canvasThemeMain());
    log::info("[画布] 编辑器样式已应用（缩放范围 0.1x ~ 4.0x）");

    // 设计 §6.7：MVP 不保留任何快捷键，全部改为鼠标操作（右键菜单 / 工具栏 / 菜单栏）
    ed::EnableShortcuts(false);
    log::info("[画布] 已关闭画布键盘快捷键（仅保留鼠标操作）");
}

// 节点宽度 / 文本收口 / 端口绘制 / 节点卡片等只读绘制层已上移 canvas_view.{h,cpp}
// （CN_editor_refactor CEA-01）。主画布每帧用 g_canvas（CanvasContext）驱动共享绘制。
CanvasContext g_canvas;

// 单行省略 elide_to_width 已上移 canvas_view.{h,cpp}（CEA-01）

// 端口绘制 draw_pin 已上移 canvas_view.{h,cpp}（CEA-01，含 PinPivotAlignment 锚点契约）

// 折行 wrap_to_width 已上移 canvas_view.{h,cpp}（CEA-01）

// 节点卡片只读绘制层（NodeResultView / result_view_of / node_card_image /
// draw_node_separator / draw_node_body）已整体上移 canvas_view.{h,cpp}
// （CN_editor_refactor CEA-01：主画布与上级编辑器共用同一实现，杜绝「简化副本」漂移）。

// ------------------------------------------------------------- 连线创建 -----
// 设计 §6.2：拖拽创建连接；类型不兼容变红并拒绝；输入端口已连接则替换
//
// 【重要契约】imgui-node-editor v0.9.3：ed::BeginCreate() 内部会调用
//   CreateItemAction::Begin()（其中 IM_ASSERT(false == m_InActive); m_InActive = true;），
//   只有 ed::EndCreate() 会把它复位为 false。因此 Begin/End **必须成对调用**，
//   不能用返回值提前 return（否则下一帧 Begin 时断言 IM_ASSERT(false == m_InActive) 失败）。
//   官方示例（widgets-example / blueprints-example）同样是无条件调用 EndCreate()。
void handle_create_link()
{
    EditorState& state = editor();

    const bool creating = ed::BeginCreate(ImVec4(0.29f, 0.56f, 0.89f, 1.0f), 2.0f);

    ed::PinId start_pin;
    ed::PinId end_pin;
    if (creating && ed::QueryNewLink(&start_pin, &end_pin) && start_pin && end_pin) {
        PinRef from = resolvePin(start_pin, state.model().graph);
        PinRef to   = resolvePin(end_pin, state.model().graph);

        // 统一成"输出 → 输入"
        if (from.valid && to.valid && from.direction == PortDirection::Input &&
            to.direction == PortDirection::Output) {
            std::swap(from, to);
        }

        if (!from.valid || !to.valid) {
            ed::RejectNewItem(ImVec4(1.0f, 0.30f, 0.30f, 1.0f), 2.0f);
        }
        else {
            bool        would_replace = false;
            std::string reason;
            const bool  ok = state.model().graph.canConnect(from.node_id, from.port_id, to.node_id,
                                                    to.port_id, &reason, &would_replace);
            if (!ok) {
                ed::RejectNewItem(ImVec4(1.0f, 0.30f, 0.30f, 1.0f), 2.0f);
                if (ImGui::IsMouseReleased(0)) {
                    log::warn("[连线] 被拒绝：" + reason);
                    state.setStatus("连线被拒绝：" + reason);
                }
            }
            else {
                if (ed::AcceptNewItem(ImVec4(0.29f, 0.56f, 0.89f, 1.0f), 2.0f)) {
                    state.snapshot(would_replace ? "替换连线" : "新建连线");
                    std::string error;
                    const std::string edge_id = state.model().graph.addEdge(
                        from.node_id, from.port_id, to.node_id, to.port_id, &error);
                    if (edge_id.empty()) {
                        log::error("[连线] 创建失败：" + error);
                        state.setStatus("连线失败：" + error);
                    }
                    else {
                        // ARCH-E1：连线创建通知（EdgeAdded；负载含两端端口与 id）
                        state.model().event_bus().publish(EditEvent::edge(
                            EditEventKind::EdgeAdded, from.node_id, from.port_id, to.node_id,
                            to.port_id, edge_id));
                        state.logSummary("[连线] " + from.node_id + "." + from.port_id + " → " +
                                          to.node_id + "." + to.port_id);
                        state.setStatus("已连线 " + from.node_id + " → " + to.node_id +
                                         (would_replace ? "（替换了原连线）" : ""));
                    }
                }
            }
        }
    }

    // ---- M_node_expansion L3-09：连线交互提示（鼠标旁显示端口类型与兼容性）----
    //  * QueryNewLink 已在上面调用过（end_pin 可能为 0 = 还没拖到目标端口）
    //  * 「类型不兼容变红」由 RejectNewItem 已提供（既有），这里补充文字提示
    if (creating && start_pin.Get() != 0) {
        const PinRef from = resolvePin(start_pin, state.model().graph);
        const PinRef to   = resolvePin(end_pin, state.model().graph);
        const Node*  fnode = from.valid ? state.model().graph.findNode(from.node_id) : nullptr;
        const Node*  tnode = to.valid ? state.model().graph.findNode(to.node_id) : nullptr;
        const Port*  fport = (fnode != nullptr) ? fnode->findPort(from.port_id, from.direction) : nullptr;
        const Port*  tport = (tnode != nullptr) ? tnode->findPort(to.port_id, to.direction) : nullptr;

        std::string tip;
        ImU32       color = IM_COL32(215, 215, 225, 255);
        if (fport != nullptr && tport != nullptr) {
            std::string reason;
            bool        would_replace = false;
            const bool  ok = state.model().graph.canConnect(from.node_id, from.port_id, to.node_id,
                                                    to.port_id, &reason, &would_replace);
            tip = std::string(engine::portTypeName(fport->type)) + " → " +
                  engine::portTypeName(tport->type);
            if (ok) {
                tip += "  ✓ 可连接";
                color = IM_COL32(130, 220, 140, 255);
            }
            else {
                tip += "  ✗ " + (reason.empty() ? std::string("类型不兼容") : reason);
                color = IM_COL32(235, 110, 110, 255);
            }
        }
        else if (fport != nullptr) {
            tip = std::string("从 ") + engine::portTypeName(fport->type) + " 端口连线（拖向目标端口）";
        }
        if (!tip.empty()) {
            ImDrawList* draw_list = ImGui::GetForegroundDrawList();
            const ImVec2 mouse = ImGui::GetMousePos();
            const ImVec2 size  = ImGui::CalcTextSize(tip.c_str());
            draw_list->AddRectFilled(mouse + ImVec2(12.0f, 10.0f),
                                     mouse + ImVec2(14.0f + size.x, 12.0f + size.y),
                                     IM_COL32(16, 18, 24, 232), 3.0f);
            draw_list->AddText(mouse + ImVec2(14.0f, 12.0f), color, tip.c_str());
        }
    }

    ed::EndCreate();
}

// ------------------------------------------------------------- 删除处理 -----
// 设计 §6.1：删除节点（连带连线）；§6.2：删除连线。每个对象一次撤销快照
// 【重要契约】同 handle_create_link：BeginDelete()/EndDelete() 必须成对调用
void handle_delete_items()
{
    EditorState& state = editor();

    const bool deleting = ed::BeginDelete();

    if (deleting) {
        ed::LinkId deleted_link;
        while (ed::QueryDeletedLink(&deleted_link)) {
            if (ed::AcceptDeletedItem()) {
                const std::string edge_id = "e" + std::to_string(deleted_link.Get());
                state.snapshot("删除连线");
                const engine::Edge* edge = state.model().graph.findEdge(edge_id);
                if (state.model().graph.removeEdge(edge_id)) {
                    // ARCH-E1：连线删除通知（删除前取边数据作负载）
                    if (edge != nullptr) {
                        state.model().event_bus().publish(EditEvent::edge(EditEventKind::EdgeRemoved, *edge));
                    }
                    state.logSummary("[连线] 删除 " + edge_id);
                    state.setStatus("已删除连线 " + edge_id);
                }
            }
        }

        ed::NodeId deleted_node;
        while (ed::QueryDeletedNode(&deleted_node)) {
            if (ed::AcceptDeletedItem()) {
                const std::string node_id = "n" + std::to_string(deleted_node.Get());
                state.snapshot("删除节点");
                if (state.deleteNode(node_id)) {
                    state.logSummary("[节点] 删除 " + node_id);
                    state.setStatus("已删除节点 " + node_id + "（含其连线）");
                }
            }
        }
    }

    ed::EndDelete();
}

// 位置安全上限：任何异常数值（FLT_MAX / NaN / 溢出）都不得进入绘图与视图计算
constexpr float kPositionLimit = 100000.0f;

float clamp_position(float value)
{
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, -kPositionLimit, kPositionLimit);
}

// ------------------------------------------------------------- 状态同步 -----
void sync_selection(EditorState& state)
{
    // ARCH-E4：选中集合归 `SelectionState`（§1.8.4）—— 从画布读回后一次
    // `set_selection` 提交；变化检测 + SelectionChanged 发布由对象内部完成
    //（内置 prev 比较，仅真正变化才回调，避免每帧高频通知 —— 与 E1 语义一致）。
    const int count = ed::GetSelectedObjectCount();
    const std::size_t capacity = static_cast<std::size_t>(std::max(count, 0));

    // 选中的节点
    std::vector<ed::NodeId> node_handles(capacity);
    const int node_count = (count > 0) ? ed::GetSelectedNodes(node_handles.data(), count) : 0;
    std::vector<std::string> node_ids;
    node_ids.reserve(static_cast<std::size_t>(node_count));
    for (int i = 0; i < node_count; ++i) {
        node_ids.push_back("n" +
                           std::to_string(node_handles[static_cast<std::size_t>(i)].Get()));
    }

    // 选中的连线（左键点连线 / 框选都会选中连线 → 可与工具栏「删除选中」联动）
    std::vector<ed::LinkId> link_handles(capacity);
    const int link_count = (count > 0) ? ed::GetSelectedLinks(link_handles.data(), count) : 0;
    std::vector<std::string> link_ids;
    link_ids.reserve(static_cast<std::size_t>(link_count));
    for (int i = 0; i < link_count; ++i) {
        link_ids.push_back("e" +
                           std::to_string(link_handles[static_cast<std::size_t>(i)].Get()));
    }

    // 单选节点（参数面板用）：单选 = 唯一选中节点；多选且原单选仍在选中内 → 保留
    std::string single;
    if (node_ids.size() == 1) {
        single = node_ids.front();
    }
    else {
        const std::string& prev_single = state.selection().single();
        if (prev_single.empty() ||
            std::find(node_ids.begin(), node_ids.end(), prev_single) == node_ids.end()) {
            single.clear();
        }
        else {
            single = prev_single;
        }
    }

    // ARCH-E4：一次提交（内部变化检测 → 变化才回调 → SelectionChanged）
    state.selection().set_selection(node_ids, link_ids, single);
}

// ------------------------------------------------------- Delete 键（ARCH-R4）----
// 设计 §6.7 原本「全部鼠标可达」；这里只补回 Delete 键这一个快捷键（不调
// ed::EnableShortcuts(true)，避免连带恢复 Ctrl+A / Ctrl+C/V/X / Ctrl+Z/Y 等整套
// 内置快捷键 —— 复制/粘贴曾因坐标同步问题暂停过，见 CHANGELOG「粘贴坐标修复」）。
// ARCH-R4：快捷键统一走命令路由层 —— 窗口条件（画布窗口聚焦/悬停）+ 通用隔离
// （文本输入 / 激活控件 / 弹层 / 左键按住）+ Delete 键检测统一在 handleShortcut
// （commands.cpp）内完成；删除执行经 dispatchCommand(AppCommand::DeleteSelection)
// —— 与工具栏 / 菜单走**同一实现**（三入口收敛；无选中时静默，与 Delete 键原行为一致）。
void handle_delete_key()
{
    handleShortcut(CommandScope::Main,
                   /*window_active=*/ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) ||
                       ImGui::IsWindowHovered(),
                   CommandContext{},
                   /*only=*/AppCommand::DeleteSelection);
}

// 位置同步（双向）：
//   · 编辑器已认识该节点  → 读回位置写进 Graph（对应拖拽移动）
//   · 编辑器还不认识（ed::GetNodePosition 对未知手柄返回 FLT_MAX，见 imgui_node_editor.cpp:1676）
//     → 用 ed::SetNodePosition 把 Graph 的位置推给编辑器，**绝不把脏值回写模型**
//   · 非有限值 / 超上限一律夹紧，避免溢出进入绘图与视图导航（历史"复制粘贴卡死"根因，见 CHANGELOG）
// 一次拖拽只压一次撤销快照
void sync_positions(EditorState& state)
{
    bool moved = false;
    for (Node& node : state.model().graph.nodes) {
        const ImVec2 position = ed::GetNodePosition(nodeHandle(node.id));

        const bool editor_knows =
            std::isfinite(position.x) && std::isfinite(position.y) &&
            std::fabs(position.x) < kPositionLimit && std::fabs(position.y) < kPositionLimit;

        if (!editor_knows) {
            ed::SetNodePosition(nodeHandle(node.id), ImVec2(node.x, node.y)); // 模型 → 编辑器
            continue;
        }

        if (std::fabs(position.x - node.x) > 0.5f || std::fabs(position.y - node.y) > 0.5f) {
            if (!g_drag_in_progress) {
                g_drag_in_progress = true;
                state.snapshot("移动节点");
            }
            node.x = clamp_position(position.x);
            node.y = clamp_position(position.y);
            moved  = true;
            // ARCH-E1：节点移动通知（拖拽期间每帧位置变化都会发；通知形态，重复无害）
            state.model().event_bus().publish(EditEvent::node(EditEventKind::NodeMoved, node.id));
        }
    }
    if (!moved) {
        g_drag_in_progress = false; // 拖拽结束
    }
}

// --------------------------------------------------- 右键菜单：新建节点 ------
void show_add_node_menu(EditorState& state, const ImVec2& spawn)
{
    engine::registerAllNodes();
    engine::NodeRegistry& registry = engine::NodeRegistry::instance();

    const NodeCategory categories[] = {NodeCategory::Input,  NodeCategory::Process,
                                       NodeCategory::Config, NodeCategory::Inference,
                                       NodeCategory::Output, NodeCategory::Auxiliary};
    for (NodeCategory category : categories) {
        if (!ImGui::BeginMenu(engine::categoryName(category))) {
            continue;
        }
        for (const engine::Definition* definition : registry.listByCategory(category)) {
            if (ImGui::MenuItem(definition->display_name.c_str())) {
                state.snapshot(std::string("创建节点 ") + definition->display_name);
                const std::string id = state.createNode(definition->type, spawn.x, spawn.y);
                if (!id.empty()) {
                    state.selection().select_node(id); // ARCH-E4：选中独立对象
                }
            }
            if (ImGui::IsItemHovered() && !definition->description.empty()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(definition->description.c_str());
                ImGui::TextDisabled("输入 %d 个 / 输出 %d 个 / 参数 %d 项",
                                    static_cast<int>(definition->inputs.size()),
                                    static_cast<int>(definition->outputs.size()),
                                    static_cast<int>(definition->params.size()));
                ImGui::EndTooltip();
            }
        }
        ImGui::EndMenu();
    }

    // ---- CN：上级节点子菜单（组合节点）----
    const std::vector<const engine::CompositeSpec*> composites =
        engine::CompositeRegistry::instance().list();
    if (!composites.empty()) {
        ImGui::Separator();
        if (ImGui::BeginMenu("上级节点")) {
            for (const engine::CompositeSpec* spec : composites) {
                if (ImGui::MenuItem(spec->name.c_str())) {
                    state.snapshot("创建上级节点");
                    std::string error;
                    const std::string id =
                        state.model().graph.addCompositeNode(spec->id, spawn.x, spawn.y, &error);
                    if (!id.empty()) {
                        state.selection().select_node(id); // ARCH-E4：选中独立对象
                        state.setStatus("已创建上级节点：" + spec->name);
                    }
                    else {
                        state.setStatus("创建上级节点失败：" + error);
                    }
                }
            }
            ImGui::EndMenu();
        }
    }
}

// ------------------------------------------------- 宽度自检（不变量）----
// 画布上**所有节点必须等宽**：宽度 = g_canvas.content_width + 节点内边距（内容宽度由「输入/输出
// 标签实测最宽」算出、全画布共用）。任何文本项漏了收口、或新加了无界控件，都会在这里被抓到。
// 尺寸 / 节点数一变就写一条 INFO（可直接拿去核对），破坏不变量时写 WARN（限流 2 秒一条）。
std::string px_text(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f", static_cast<double>(value));
    return std::string(buffer);
}

void check_uniform_node_width(const Graph& graph)
{
    static int         frame          = 0;
    static std::string last_signature; // 上一次采样到的「尺寸指纹」
    static double      last_warn_time = 0.0;
    if (++frame < 3) {
        return; // 前两帧节点尺寸尚未结算（新建 / 刚加载图）
    }

    float       min_w = 0.0f;
    float       max_w = 0.0f;
    std::string min_id;
    std::string max_id;
    int         counted = 0;
    for (const Node& node : graph.nodes) {
        const ImVec2 size = ed::GetNodeSize(nodeHandle(node.id));
        if (size.x <= 0.0f) {
            continue; // 编辑器本帧才认识该节点 → 尺寸还没结算
        }
        ++counted;
        if (min_w == 0.0f || size.x < min_w) {
            min_w  = size.x;
            min_id = node.id;
        }
        if (size.x > max_w) {
            max_w  = size.x;
            max_id = node.id;
        }
    }
    if (counted < 2) {
        return;
    }

    const bool        uniform = (max_w - min_w) < 0.5f;
    const std::string signature =
        std::to_string(counted) + "/" + px_text(min_w) + "/" + px_text(max_w);
    if (signature != last_signature) {
        last_signature = signature; // 尺寸/节点数一变就重新采样并留证（换工作流也能被记录）
        log::info("[画布] 节点宽度自检：" + std::to_string(counted) + " 个节点，宽 " + px_text(min_w) +
                  " ~ " + px_text(max_w) + " px（内容区 " + px_text(g_canvas.content_width) +
                  " px · 按输入/输出标签自适应；最窄 " + min_id + " / 最宽 " + max_id + "）→ " +
                  (uniform ? "全部等宽 ✅" : "宽度不一致 ❌"));
    }
    if (!uniform) { // 破坏不变量：限流告警（最多 2 秒一条）
        const double now = ImGui::GetTime();
        if (now - last_warn_time > 2.0) {
            last_warn_time = now;
            log::warn("[画布] 节点宽度不一致（「固定宽度」不变量被破坏）：最窄 " + min_id + " = " +
                      px_text(min_w) + " px，最宽 " + max_id + " = " + px_text(max_w) +
                      " px —— 检查是否有文本未走 elide_to_width()/wrap_to_width() 收口");
        }
    }
}

// 折行自检（一次性 · 首次绘制时跑一遍）：用「超长无空格串 / 纯中文 / 中英混排 / 自带换行」四类样例
// 验证 wrap_to_width 的每条输出行都放得下 —— 这是「节点宽度固定」的构造性证据（不依赖人工造数据）。
void log_wrap_selftest()
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    const std::vector<std::string> samples = {
        std::string(400, 'x'), // 超长无空格拉丁串（英文单词不会自动断行的经典反例）
        "这是一段没有任何空格的超长中文文本，用来验证中文也能在任意字符处断行，而不会把节点撑宽。",
        "中文English混排12345，测试mixed-width文本的折行边界是否正确。",
        "第一行\n第二行很长很长很长很长很长很长很长很长很长很长很长很长很长很长\n第三行",
    };

    float worst = 0.0f;
    for (const std::string& sample : samples) {
        const std::string wrapped = wrapToWidth(sample, g_canvas.content_width, kNodeSummaryLines);
        std::size_t       begin   = 0;
        while (true) {
            const std::size_t end  = wrapped.find('\n', begin);
            const std::string line = (end == std::string::npos)
                                         ? wrapped.substr(begin)
                                         : wrapped.substr(begin, end - begin);
            worst                  = (std::max)(worst, ImGui::CalcTextSize(line.c_str()).x);
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
    }
    log::info("[画布] 折行自检：" + std::to_string(samples.size()) + " 个样例，最长行 " + px_text(worst) +
              " px（上限 " + px_text(g_canvas.content_width) + " px）→ " +
              (worst <= g_canvas.content_width + 0.5f ? "全部收口 ✅" : "有行超宽 ❌"));
}

// --------------------------------- Alt + 左键拖拽平移（新增：无中键 / 笔记本可用）----
// 原理：imgui-node-editor 的平移动作每帧读 `Config::NavigateButtonIndex`
//       （imgui_node_editor.cpp:3326 `ImGui::IsMouseDragging(...NavigateButtonIndex...)`），
//       所以**在 ed::Begin() 之前**改写它，本帧的平移键就换了。
// 实现：`ed::GetConfig()`（imgui_node_editor.h:285）返回的是编辑器内部**真实**的 Config
//       对象（api.cpp:77 → EditorContext::GetConfig()），不是临时副本；其底层
//       `Detail::Config` 并非 const，因此这里只做「Alt 按下临时改写 → 松开还原」，
//       既不碰 third_party 源码，也不改变 Alt 未按下时的任何既有行为。
// 返回：本次是否处于 Alt 平移模式（供调用方做界面提示）。
bool update_alt_left_pan()
{
    const bool alt = ImGui::GetIO().KeyAlt;

    ed::Config& config = const_cast<ed::Config&>(ed::GetConfig(g_context));

    const int want_nav    = alt ? ImGuiMouseButton_Left : kNodeEditorNavButton;
    const int want_drag   = alt ? kAltPanParkedButton   : kNodeEditorDragButton;
    const int want_select = alt ? kAltPanParkedButton   : kNodeEditorSelectButton;

    // 已经是目标状态 → 直接返回（幂等：反复进入本函数不会重复写日志）
    if (config.NavigateButtonIndex == want_nav && config.DragButtonIndex == want_drag &&
        config.SelectButtonIndex == want_select) {
        g_alt_pan_active = alt;
        return alt;
    }

    config.NavigateButtonIndex = want_nav;
    config.DragButtonIndex     = want_drag;
    config.SelectButtonIndex   = want_select;
    g_alt_pan_active           = alt;

    log::info(alt ? "[画布] 进入 Alt 平移模式：Alt+左键拖拽平移（右键平移 / 左键拖节点·框选暂挂起）"
                  : "[画布] 退出 Alt 平移模式：恢复右键平移、左键拖节点 / 框选");
    return alt;
}

// --------------------------------------------------- CS-01：画布逐帧诊断（2 秒一行）----
// 为什么要有这一行：2026-10-09 的「个位数 FPS」事故（纹理缓存容量 8 < 同屏卡片图 9 →
// 每帧 100% 未命中、每帧重解码 9 张 ≈ 166 ms）在日志里**完全看不见**——
// 旧心跳 3600 帧才一条（短会话采不到），而纹理失败路径才写日志（成功路径静默）。
// 这里把「帧预算归因」直接打出来：节点循环 / 宽度重算 / 连线 + 纹理请求·命中·解码·上传。
struct CanvasDiagnostic {
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    double      nodes_ms = 0.0;
    double      width_ms = 0.0;
    double      links_ms = 0.0;
    std::size_t frames   = 0;
    ui::TextureCacheStats prev;
    bool        has_prev = false;
};

CanvasDiagnostic& canvas_diagnostic()
{
    static CanvasDiagnostic diag;
    return diag;
}

double ms_between(const std::chrono::steady_clock::time_point& from,
                  const std::chrono::steady_clock::time_point& to)
{
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// 纯格式化（不参与绘制；避免 std::to_string 输出 6 位小数刷屏）
std::string fmt_num(double value, int decimals)
{
    char buffer[48] = {};
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

void log_canvas_diagnostic(const EditorState& state, float zoom, double width_ms, double nodes_ms,
                           double links_ms)
{
    CanvasDiagnostic& diag = canvas_diagnostic();
    diag.width_ms += width_ms;
    diag.nodes_ms += nodes_ms;
    diag.links_ms += links_ms;
    ++diag.frames;

    const auto now = std::chrono::steady_clock::now();
    if (now - diag.last < std::chrono::seconds(2)) {
        return; // 2 秒一行（够密，能覆盖「只有 17 秒的短会话」；够疏，不刷屏）
    }

    const double elapsed = std::chrono::duration<double>(now - diag.last).count();
    const double frames  = (diag.frames > 0) ? static_cast<double>(diag.frames) : 1.0;

    const ui::TextureCacheStats stats = ui::textureCacheStats();
    const auto delta = [&diag, &stats](std::size_t TextureCacheStats::*member) {
        return stats.*member - (diag.has_prev ? diag.prev.*member : 0);
    };
    const std::size_t requests  = delta(&ui::TextureCacheStats::requests);
    const std::size_t hits      = delta(&ui::TextureCacheStats::hits);
    const std::size_t misses    = delta(&ui::TextureCacheStats::misses);
    const std::size_t decodes   = delta(&ui::TextureCacheStats::decodes);
    const std::size_t uploads   = delta(&ui::TextureCacheStats::uploads);
    const std::size_t evictions = delta(&ui::TextureCacheStats::evictions);

    log::info("[诊断] 画布 节点=" + std::to_string(state.model().graph.nodes.size()) +
              " 连线=" + std::to_string(state.model().graph.edges.size()) +
              " 缩放=" + fmt_num(static_cast<double>(zoom), 2) +
              " 帧=" + std::to_string(diag.frames) + " FPS=" +
              std::to_string(static_cast<int>(ImGui::GetIO().Framerate)) +
              " 宽度重算=" + fmt_num(diag.width_ms / frames, 1) + "ms/帧" +
              " 节点循环=" + fmt_num(diag.nodes_ms / frames, 1) + "ms/帧" +
              " 连线自检=" + fmt_num(diag.links_ms / frames, 1) + "ms/帧" +
              " | 纹理 近" + fmt_num(elapsed, 1) + "s: 请求=" + std::to_string(requests) +
              " 命中=" + std::to_string(hits) + " 未命中=" + std::to_string(misses) +
              " 解码=" + std::to_string(decodes) + " 上传=" + std::to_string(uploads) +
              " 淘汰=" + std::to_string(evictions) + "（每帧请求 " +
              fmt_num(static_cast<double>(requests) / frames, 1) + " 次 / 解码 " +
              fmt_num(static_cast<double>(decodes) / frames, 1) + " 次）缓存 " +
              std::to_string(stats.entries) + "/" + std::to_string(stats.capacity) + " 条 " +
              fmt_num(static_cast<double>(stats.bytes) / (1024.0 * 1024.0), 1) + "MB");

    // 不变量告警：本周期有请求却一次没命中 ⇒ 容量/预算相对「同屏卡片图数」过小
    // （或图片文件每帧都在变）→ 每帧重解码，FPS 必然掉到个位数。
    if (requests > 0 && hits == 0) {
        log::warn("[画布] 纹理缓存**整周期 0 命中**（请求 " + std::to_string(requests) +
                  " 次 / 解码 " + std::to_string(decodes) + " 次；容量 " +
                  std::to_string(stats.capacity) + " 条 / 预算 " +
                  fmt_num(static_cast<double>(stats.byte_budget) / (1024.0 * 1024.0), 0) +
                  "MB）：每帧重解码是 FPS 掉到个位数的直接原因，见 CHANGELOG 2026-10-09");
    }

    // 换周期：清空累加器，记下本周期末的累计值（下周期用它算差值）
    diag.nodes_ms = 0.0;
    diag.width_ms = 0.0;
    diag.links_ms = 0.0;
    diag.frames   = 0;
    diag.prev     = stats;
    diag.has_prev = true;
    diag.last     = now;
}

} // namespace

// ============================================================================
//  M_node_expansion L3（v5 定稿 §4.4.3）：小地图 ④ / 对齐辅助系统 ⑤+⑥ / 辅助开关
//  会话级开关（不进 config：避免动 settings_validation 断言面）
// ============================================================================
bool  g_show_minimap  = true;
bool  g_snap_enabled  = true;
float g_snap_distance = 8.0f; // 吸附距离（**屏幕像素**，换算画布坐标用当前缩放）

// ----------------------------------------------------------------------------
//  小地图 = 纯导航工具（M_node_expansion L3 · 2026-10-15 修正）：
//  · 只做「查看缩略 + 点击/拖动 → 视口中心平移到对应画布位置」（快速挪动 / 查询）；
//  · 严禁在小地图区域触发任何画布编辑（选中 / 拖节点 / 框选 / 右键 / 平移）。
//  实现：ed::Begin 之前把该区域的鼠标事件从 io 中摘除（ed 完全感知不到），
//       小地图交互由共享 draw_minimap 用「屏蔽前的快照」自行驱动。
//  CLN-09：几何 / 输入屏蔽 / 绘制已上移 canvas_view 单一来源，本 TU 只留开关与状态。
// ----------------------------------------------------------------------------
MinimapState g_minimap; // CLN-09：输入屏蔽快照（blocking/clicked/down/mouse）


// 上一帧节点位置（对齐辅助判断「谁在拖动」：imgui-node-editor 无公开拖动查询 API）
std::unordered_map<std::string, ImVec2> g_prev_positions;

// 拖拽中的节点 id：已上移 canvas_view 共享（CLN-10），主画布传 g_alt_pan_active 互斥。
// 对齐吸附封装（CLN-10）：开关 + 拖拽判定 + 调共享核心 + 状态更新。
// 对齐核心逻辑（三边对 / 辅助线端点）已上移 canvas_view::apply_alignment_snap_core 单一来源。
SnapResult apply_alignment_snap(const Graph& graph)
{
    SnapResult result;
    if (!g_snap_enabled || g_alt_pan_active) {
        g_prev_positions.clear();
        return result;
    }
    const std::string dragged = find_dragged_node(graph, g_alt_pan_active, g_prev_positions);
    if (dragged.empty()) {
        return result;
    }
    if (graph.findNode(dragged) == nullptr) {
        return result;
    }
    const float zoom        = ed::GetCurrentZoom();
    const float snap_canvas = (zoom > 1e-6f) ? g_snap_distance / zoom : g_snap_distance;
    const ImVec2 dpos  = ed::GetNodePosition(nodeHandle(dragged));
    const ImVec2 dsize = ed::GetNodeSize(nodeHandle(dragged));
    const float  dw    = dsize.x > 1.0f ? dsize.x : 80.0f;
    const float  dh    = dsize.y > 1.0f ? dsize.y : 40.0f;
    result = apply_alignment_snap_core(graph, dragged, dpos, dw, dh, snap_canvas);
    for (const Node& node : graph.nodes) {
        g_prev_positions[node.id] = ed::GetNodePosition(nodeHandle(node.id));
    }
    return result;
}
// 小地图（左下角缩略图）已上移 canvas_view::draw_minimap 共享（CLN-09），
// 主画布在 ed::End 之后以 canvas_theme_main() + 状态着色回调调用（见 draw 循环尾）。

// 画布辅助开关弹窗（小地图 / 对齐吸附 / 吸附距离）
void draw_canvas_assist_popup()
{
    if (ImGui::Button("画布辅助")) {
        ImGui::OpenPopup("##canvas_assist");
    }
    if (ImGui::BeginPopup("##canvas_assist")) {
        ImGui::TextDisabled("画布辅助（会话级设置）");
        ImGui::Separator();
        ImGui::Checkbox("小地图（左下角）", &g_show_minimap);
        ImGui::Checkbox("对齐吸附（拖动时）", &g_snap_enabled);
        ImGui::SliderFloat("吸附距离", &g_snap_distance, 2.0f, 30.0f, "%.0f px");
        ImGui::EndPopup();
    }
}



void drawNodeCanvas(const char* title, CanvasOptions& options, const ReadOnlyView& view)
{
    ensure_context();
    EditorState& state = editor();

    static ImVec2      spawn_position = ImVec2(0.0f, 0.0f);
    static std::string context_node;

    ImGui::SetNextWindowSize(ImVec2(1000.0f, 600.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title)) {
        ImGui::End();
        return;
    }

    // 新增：Alt+左键拖拽平移（Alt 未按下时完全维持原有绑定，见 update_alt_left_pan）。
    // 放在提示行之前 → 提示能立刻反映本帧的平移模式。
    g_alt_pan_active = update_alt_left_pan();

    ImGui::TextDisabled("输入端口在节点左侧、输出端口在右侧 ｜ 拖左侧圆点连线，类型不符会变红被拒");
    ImGui::SameLine();
    ImGui::TextDisabled("｜ 右键：画布新建 / 节点复制·删除 / 连线删除 ｜ 左键点连线可选中 → Delete 或工具栏「删除选中」");
    if (g_alt_pan_active) {
        ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f),
                           "Alt 平移模式：按住左键拖拽即可平移画布（松开 Alt 即恢复拖动节点/框选）");
    } else {
        ImGui::TextDisabled(
            "Alt + 左键拖拽：平移画布（无中键鼠标 / 笔记本也可用）｜ 滚轮：以鼠标为中心缩放 0.1x~4.0x");
    }
    ImGui::SameLine();
    draw_canvas_assist_popup(); // M_node_expansion L3：小地图 / 对齐吸附开关
    ImGui::Separator();

    ed::SetCurrentEditor(g_context);
    // 小地图 = 纯导航：该区域鼠标事件只属于小地图（查看 / 跳转），画布不得选中或编辑节点。
    // 必须放在 ed::Begin 之前（ed 在 Begin 内处理鼠标）。
    blockMinimapInput(g_show_minimap, g_minimap);
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
        ed::SetNodePosition(nodeHandle(node.id), ImVec2(node.x, node.y));
    }
    // 宽度自适应 + 节点卡片绘制：走共享 canvas_view 只读绘制层（CEA-01 / CEA-02）
    //   —— 主题 = 主画布默认主题；运行读模型 = view.snapshot（ARCH-R3 渲染只读 ReadOnlyView）
    g_canvas.graph        = &view.graph();
    g_canvas.theme        = &canvasThemeMain();
    g_canvas.run_snapshot = [&view]() -> const engine::RunSnapshot& { return view.snapshot; };
    const auto diag_t0 = std::chrono::steady_clock::now();
    updateContentWidth(view.graph(), g_canvas);
    const auto diag_t1 = std::chrono::steady_clock::now();
    for (const Node& node : view.nodes_for_render()) {
        drawNodeBody(g_canvas, node);
    }
    const auto diag_t2 = std::chrono::steady_clock::now();
    log_wrap_selftest();                     // 折行自检（一次性：每条输出行都必须放得下）
    check_uniform_node_width(view.graph());  // 固定宽度自检（不变量：全部节点等宽）
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

    handle_create_link();
    handle_delete_items();

    // ------------------------------------------------- 右键菜单（需挂起画布）----
    static ed::NodeId context_node_handle; // static：避免每帧重置（历史 bug 根因，见 CHANGELOG）

    ed::Suspend();

    // 右键菜单按「最具体优先」串联（官方 blueprints-example 写法）：节点 > 背景。
    // 说明：连线右键菜单（「删除连线」「选中该连线」）已按需求下线，见 CHANGELOG「连线右键菜单移除」；
    //       连线删除请用「左键单击连线选中 → 工具栏【删除选中】」。
    if (ed::ShowNodeContextMenu(&context_node_handle)) {
        context_node         = "n" + std::to_string(context_node_handle.Get());
        state.selection().select_node(context_node); // ARCH-E4：选中独立对象
        ImGui::OpenPopup("##node_menu");
    }
    else if (ed::ShowBackgroundContextMenu()) {
        spawn_position = ed::ScreenToCanvas(ImGui::GetMousePos());
        ImGui::OpenPopup("##canvas_menu");
    }

    if (ImGui::BeginPopup("##canvas_menu")) {
        ImGui::TextDisabled("新建节点");
        ImGui::Separator();
        show_add_node_menu(state, spawn_position);
        ImGui::Separator();
        // 设计 §6.1：右键画布 → 粘贴
        if (ImGui::MenuItem("粘贴", nullptr, false, state.has_clipboard())) {
            state.pasteClipboard();
        }
        if (ImGui::IsItemHovered() && !state.has_clipboard()) {
            ImGui::SetTooltip("剪贴板为空：先在节点右键菜单里「复制」");
        }
        if (ImGui::MenuItem("删除选中（节点 / 连线）", nullptr, false, state.has_selection())) {
            state.deleteSelected();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##node_menu")) {
        ImGui::TextDisabled("节点 %s", context_node.c_str());
        ImGui::Separator();
        // 设计 §6.1：右键节点 → 复制（粘贴在画布右键菜单）
        if (ImGui::MenuItem("复制", nullptr, false, state.has_selection())) {
            state.copySelection();
        }
        if (ImGui::MenuItem("改名（右侧参数面板）")) {
            state.request_focus_title = true;
        }
        // CN：上级节点 → 打开独立编辑器
        {
            const engine::Node* ctx_node = state.model().graph.findNode(context_node);
            if (ctx_node != nullptr && !ctx_node->composite_id.empty()) {
                ImGui::Separator();
                if (ImGui::MenuItem("编辑内部图…")) {
                    compositeEditorOpen(ctx_node->composite_id);
                }
            }
        }
        ImGui::Separator();
        // PD-06①（FEA-M4-11 · 通用重跑）：复用上游结果，受影响节点先清结果与状态
        {
            const engine::Node* ctx_node = state.model().graph.findNode(context_node);
            const bool ctx_rerun_ok =
                ctx_node != nullptr && ctx_node->state != engine::NodeState::Waiting &&
                !state.session_active();
            if (ImGui::MenuItem("重跑该节点", nullptr, false, ctx_rerun_ok)) {
                state.startRerunAsync(context_node, engine::RerunScope::NodeOnly);
            }
            if (ImGui::MenuItem("重跑该节点及下游", nullptr, false, ctx_rerun_ok)) {
                state.startRerunAsync(context_node, engine::RerunScope::Downstream);
            }
            if (ImGui::IsItemHovered() && ctx_node != nullptr &&
                ctx_node->state == engine::NodeState::Waiting) {
                ImGui::SetTooltip("该节点尚未运行过（状态为等待），暂不可重跑");
            }
        }
        if (ImGui::MenuItem("删除该节点")) {
            state.deleteSelected();
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
    handle_delete_key();

    // 视图跟随前先确认坐标正常（历史卡死根因：异常坐标 + NavigateToContent 会让视图落到天文数字区间）
    const bool positions_sane =
        view.nodes_for_render().empty() ||
        std::all_of(view.nodes_for_render().begin(), view.nodes_for_render().end(),
                    [](const Node& node) {
                        return std::isfinite(node.x) && std::isfinite(node.y) &&
                               std::fabs(node.x) < kPositionLimit &&
                               std::fabs(node.y) < kPositionLimit;
                    });

    if (g_first_frame || state.request_navigate_to_content) {
        if (positions_sane) {
            ed::NavigateToContent(0.0f);
        }
        else {
            log::warn("[画布] 跳过视图跟随：存在异常节点坐标（已按上限夹紧）");
            state.setStatus("已跳过视图跟随（检测到异常节点坐标）");
        }
        g_first_frame                     = false;
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
        const SnapResult snap = apply_alignment_snap(state.model().graph);
        if (snap.applied) {
            drawSnapLines(snap, ImGui::GetWindowDrawList(), canvasThemeMain().snap_line, 1.6f);
        }
        draw_minimap(view.graph(), canvasThemeMain(), g_show_minimap, g_minimap,
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

    sync_positions(state);
    ed::SetCurrentEditor(nullptr);

    options.node_count = static_cast<int>(view.nodes_for_render().size());
    options.link_count = static_cast<int>(view.edges_for_render().size());

    ImGui::End();
}

std::string canvasAddNodeAtCenter(const std::string& type)
{
    ensure_context();
    EditorState& state = editor();

    ed::SetCurrentEditor(g_context);
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
std::string canvasAddCompositeAtCenter(const std::string& composite_id)
{
    ensure_context();
    EditorState& state = editor();

    ed::SetCurrentEditor(g_context);
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

} // namespace aiwrite::ui
