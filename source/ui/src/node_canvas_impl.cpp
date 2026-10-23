#include "node_canvas.h"

#include "composite.h"        // CN：上级节点实例识别 / 创建
#include "canvas_view.h"      // CN_editor_refactor：共享只读绘制层（CEA-01 / CEA-02）
#include "commands.h"         // ARCH-R4：命令路由层（快捷键表 / handleShortcut）
#include "interactor.h"       // ARCH-E5：画布交互共享层（双画布单套交互）
#include "composite_editor.h" // CN：双击/右键打开上级节点编辑器
#include "node_registry.h"
#include "provider_resolve.h"
#include "editor_controller.h"
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

#include "node_canvas_detail.h"  // ARCH-Z1：模块内契约（跨 TU 的 12 个入口）

namespace aiwrite::ui {
namespace canvas_detail {

namespace ed = ax::NodeEditor;

using engine::Edge;
using engine::Graph;
using engine::Node;
using engine::NodeCategory;
using engine::NodeState;
using engine::Port;
using engine::PortDirection;
using engine::PortType;

// 主画布跨帧状态（编辑器上下文 / CanvasContext / 小地图 / 吸附 / 拖拽标记 / 右键菜单落点）
// 见 `NodeCanvasState`（node_canvas.h · ARCH-A6）：原先是 11 条文件级 static + 3 条函数内 static。

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

void ensure_context(NodeCanvasState& nc)
{
    if (nc.context != nullptr) {
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

    nc.context = ed::CreateEditor(&config);
    log::info("[画布] 编辑器上下文已创建");

    // 注意：GetStyle/PushStyleColor 作用于"当前编辑器"，必须先 SetCurrentEditor
    ed::SetCurrentEditor(nc.context);
    canvasApplyEditorStyle(canvasThemeMain());
    log::info("[画布] 编辑器样式已应用（缩放范围 0.1x ~ 4.0x）");

    // 设计 §6.7：MVP 不保留任何快捷键，全部改为鼠标操作（右键菜单 / 工具栏 / 菜单栏）
    ed::EnableShortcuts(false);
    log::info("[画布] 已关闭画布键盘快捷键（仅保留鼠标操作）");
}

// 节点宽度 / 文本收口 / 端口绘制 / 节点卡片等只读绘制层已上移 canvas_view.{h,cpp}
// （CN_editor_refactor CEA-01）。主画布每帧重填 `state.node_canvas().canvas` 驱动共享绘制。

// 单行省略 elide_to_width 已上移 canvas_view.{h,cpp}（CEA-01）

// 端口绘制 draw_pin 已上移 canvas_view.{h,cpp}（CEA-01，含 PinPivotAlignment 锚点契约）

// 折行 wrap_to_width 已上移 canvas_view.{h,cpp}（CEA-01）

// 节点卡片只读绘制层（NodeResultView / result_view_of / node_card_image /
// draw_node_separator / draw_node_body）已整体上移 canvas_view.{h,cpp}
// （CN_editor_refactor CEA-01：主画布与上级编辑器共用同一实现，杜绝「简化副本」漂移）。

// --------------------------------------------------------- 交互回调组 ------
// ARCH-E5：主画布与 ce 画布共用 interactor（双画布单套交互）；模型差异
//（图源 = 注入的 EditorController.model().graph + SelectionState / 快照归属 / 提示落点）经回调注入。
CanvasInteract makeMainCanvasInteract(EditorController& editor); // 前向声明（定义紧随其后）

// 每帧被连线/删除/选中/位置同步调用 4 次 —— 回调只捕获注入的控制器引用（构造廉价），
// 不做文件级静态缓存（ARCH-A5：可变静态状态归零）；跨帧语义不变。
CanvasInteract mainCanvasInteract(EditorController& editor)
{
    return makeMainCanvasInteract(editor);
}

CanvasInteract makeMainCanvasInteract(EditorController& editor)
{
    CanvasInteract interact;

    interact.resolve_pin = [&editor](ed::PinId start_pin, ed::PinId end_pin, PinRef& from, PinRef& to) {
        EditorController& state = editor;
        from = resolvePin(start_pin, state.model().graph);
        to   = resolvePin(end_pin, state.model().graph);
        return from.valid && to.valid;
    };
    interact.can_connect = [&editor](const PinRef& from, const PinRef& to, std::string& reason,
                              bool& would_replace) {
        EditorController& state = editor;
        return state.model().graph.canConnect(from.node_id, from.port_id, to.node_id, to.port_id,
                                              &reason, &would_replace);
    };
    interact.on_link_accepted = [&editor](const PinRef& from, const PinRef& to, bool would_replace) {
        EditorController& state = editor;
        state.snapshot(would_replace ? "替换连线" : "新建连线");
        std::string error;
        const std::string edge_id = state.model().graph.addEdge(from.node_id, from.port_id,
                                                                to.node_id, to.port_id, &error);
        if (edge_id.empty()) {
            log::error("[连线] 创建失败：" + error);
            state.setStatus("连线失败：" + error);
        }
        else {
            // ARCH-E1：连线创建通知（EdgeAdded；负载含两端端口与 id）
            state.model().event_bus().publish(EditEvent::edge(
                EditEventKind::EdgeAdded, from.node_id, from.port_id, to.node_id, to.port_id,
                edge_id));
            state.logSummary("[连线] " + from.node_id + "." + from.port_id + " → " +
                             to.node_id + "." + to.port_id);
            state.setStatus("已连线 " + from.node_id + " → " + to.node_id +
                            (would_replace ? "（替换了原连线）" : ""));
        }
    };
    interact.on_link_rejected = [&editor](const std::string& reason) {
        EditorController& state = editor;
        log::warn("[连线] 被拒绝：" + reason);
        state.setStatus("连线被拒绝：" + reason);
    };

    interact.on_create_tip = [&editor](ed::PinId start_pin, ed::PinId end_pin) {
        // M_node_expansion L3-09：连线交互提示（鼠标旁显示端口类型与兼容性）。
        //  * QueryNewLink 已由交互层调用过（end_pin 可能为 0 = 还没拖到目标端口）
        //  * 「类型不兼容变红」由 RejectNewItem 已提供（既有），这里补充文字提示
        EditorController& state = editor;
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
            tip = std::string("从 ") + engine::portTypeName(fport->type) +
                  " 端口连线（拖向目标端口）";
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
    };
    interact.on_delete_batch = [&editor](const std::string& label) { editor.snapshot(label); };
    interact.delete_link = [&editor](const std::string& link_id) {
        EditorController& state = editor;
        const engine::Edge* edge = state.model().graph.findEdge(link_id);
        if (state.model().graph.removeEdge(link_id)) {
            // ARCH-E1：连线删除通知（删除前取边数据作负载）
            if (edge != nullptr) {
                state.model().event_bus().publish(EditEvent::edge(EditEventKind::EdgeRemoved, *edge));
            }
            state.logSummary("[连线] 删除 " + link_id);
            state.setStatus("已删除连线 " + link_id);
        }
    };
    interact.delete_node = [&editor](const std::string& node_id) {
        EditorController& state = editor;
        if (state.deleteNode(node_id)) {
            state.logSummary("[节点] 删除 " + node_id);
            state.setStatus("已删除节点 " + node_id + "（含其连线）");
        }
    };
    interact.on_selection = [&editor](const std::vector<std::string>& node_ids,
                               const std::vector<std::string>& link_ids) {
        EditorController& state = editor;
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
    };
    return interact;
}

// ------------------------------------------------------------- 连线创建 -----
// 设计 §6.2：拖拽创建连接；类型不兼容变红并拒绝；输入端口已连接则替换
//（ARCH-E5：BeginCreate/EndCreate 成对契约 + 拒绝/接受/替换语义已收敛进 interactor.cpp；
//  拖拽提示见 mainCanvasInteract.on_create_tip）
void handle_create_link(EditorController& state)
{
    interactorCreateLink(mainCanvasInteract(state));
}


// ------------------------------------------------------------- 删除处理 -----
// 设计 §6.1：删除节点（连带连线）；§6.2：删除连线。每个对象一次撤销快照
//（ARCH-E5：BeginDelete/EndDelete 成对契约收敛进 interactor.cpp）
void handle_delete_items(EditorController& state)
{
    interactorDeleteOuter(mainCanvasInteract(state), "删除连线", "删除节点");
}


// ------------------------------------------------------------- 状态同步 -----
void sync_selection(EditorController& state)
{
    // ARCH-E4/E5：选中集合归 `SelectionState`（§1.8.4）—— ed 选中读取经
    // interactorSyncSelection（GetSelected* 唯一封装在 interactor.cpp）；
    // single 保留逻辑见 mainCanvasInteract.on_selection（一次 set_selection 提交）。
    interactorSyncSelection(mainCanvasInteract(state));
}

// ------------------------------------------------------- Delete 键（ARCH-R4）----
// 设计 §6.7 原本「全部鼠标可达」；这里只补回 Delete 键这一个快捷键（不调
// ed::EnableShortcuts(true)，避免连带恢复 Ctrl+A / Ctrl+C/V/X / Ctrl+Z/Y 等整套
// 内置快捷键 —— 复制/粘贴曾因坐标同步问题暂停过，见 CHANGELOG「粘贴坐标修复」）。
// ARCH-R4：快捷键统一走命令路由层 —— 窗口条件（画布窗口聚焦/悬停）+ 通用隔离
// （文本输入 / 激活控件 / 弹层 / 左键按住）+ Delete 键检测统一在 handleShortcut
// （commands.cpp）内完成；删除执行经 dispatchCommand(AppCommand::DeleteSelection)
// —— 与工具栏 / 菜单走**同一实现**（三入口收敛；无选中时静默，与 Delete 键原行为一致）。
void handle_delete_key(EditorController& editor)
{
    handleShortcut(CommandScope::Main,
                   /*window_active=*/interactorWindowActiveMain(),
                   CommandContext{nullptr, nullptr, &editor},
                   /*only=*/AppCommand::DeleteSelection);
}

// 位置同步（双向）：
//   · 编辑器已认识该节点  → 读回位置写进 Graph（对应拖拽移动）
//   · 编辑器还不认识（GetNodePosition 对未知手柄返回 FLT_MAX）→ interactor 把
//     Graph 位置推给编辑器，**绝不把脏值回写模型**
//   · 非有限值 / 超上限一律夹紧（interactor 内按 kCanvasPositionLimit）
// 一次拖拽只压一次撤销快照
void sync_positions(EditorController& state, NodeCanvasState& nc)
{
    bool moved = false;
    for (Node& node : state.model().graph.nodes) {
        const PositionRead p =
            interactorReadNodePosition(node.id, node.x, node.y, kCanvasPositionLimit, 0.5f);
        if (!p.editor_knows) {
            continue; // 编辑器不认识的节点：interactor 已把模型坐标推给编辑器
        }
        if (p.changed) {
            if (!nc.drag_in_progress) {
                nc.drag_in_progress = true;
                state.snapshot("移动节点");
            }
            node.x = p.x;
            node.y = p.y;
            moved  = true;
            // ARCH-E1：节点移动通知（拖拽期间每帧位置变化都会发；通知形态，重复无害）
            state.model().event_bus().publish(EditEvent::node(EditEventKind::NodeMoved, node.id));
        }
    }
    if (!moved) {
        nc.drag_in_progress = false; // 拖拽结束
    }
}

// --------------------------------------------------- 右键菜单：新建节点 ------
void show_add_node_menu(EditorController& state, const ImVec2& spawn)
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
// 画布上**所有节点必须等宽**：宽度 = canvas.content_width + 节点内边距（内容宽度由「输入/输出
// 标签实测最宽」算出、全画布共用）。任何文本项漏了收口、或新加了无界控件，都会在这里被抓到。
// 尺寸 / 节点数一变就写一条 INFO（可直接拿去核对），破坏不变量时写 WARN（限流 2 秒一条）。
std::string px_text(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f", static_cast<double>(value));
    return std::string(buffer);
}

void check_uniform_node_width(const Graph& graph, const CanvasContext& canvas)
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
                  " ~ " + px_text(max_w) + " px（内容区 " + px_text(canvas.content_width) +
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
void log_wrap_selftest(const CanvasContext& canvas)
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
        const std::string wrapped = wrapToWidth(sample, canvas.content_width, kNodeSummaryLines);
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
              " px（上限 " + px_text(canvas.content_width) + " px）→ " +
              (worst <= canvas.content_width + 0.5f ? "全部收口 ✅" : "有行超宽 ❌"));
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
bool update_alt_left_pan(NodeCanvasState& nc)
{
    const bool alt = ImGui::GetIO().KeyAlt;

    ed::Config& config = const_cast<ed::Config&>(ed::GetConfig(nc.context));

    const int want_nav    = alt ? ImGuiMouseButton_Left : kNodeEditorNavButton;
    const int want_drag   = alt ? kAltPanParkedButton   : kNodeEditorDragButton;
    const int want_select = alt ? kAltPanParkedButton   : kNodeEditorSelectButton;

    // 已经是目标状态 → 直接返回（幂等：反复进入本函数不会重复写日志）
    if (config.NavigateButtonIndex == want_nav && config.DragButtonIndex == want_drag &&
        config.SelectButtonIndex == want_select) {
        nc.alt_pan_active = alt;
        return alt;
    }

    config.NavigateButtonIndex = want_nav;
    config.DragButtonIndex     = want_drag;
    config.SelectButtonIndex   = want_select;
    nc.alt_pan_active           = alt;

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

void log_canvas_diagnostic(const EditorController& state, float zoom, double width_ms, double nodes_ms,
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

    const ui::TextureCacheStats stats = state.textures().stats(); // ARCH-A6：缓存是显式对象
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
}  // namespace canvas_detail
}  // namespace aiwrite::ui
