#include "ui/node_canvas.h"

#include "engine/node_registry.h"
#include "engine/provider_resolve.h"
#include "ui/editor_state.h"
#include "ui/output_panel.h"
#include "ui/theme.h"
#include "utils/config.h"
#include "utils/log.h"
#include "utils/paths.h"

#include <imgui.h>
#include <imgui_node_editor.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
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

// 颜色（分类 / 端口类型 / 节点状态）统一由 ui/theme.h 提供（设计 §14.3 / §4.5 / §4.6）

// ------------------------------------------------- 稳定手柄映射（id ↔ handle）---
// 节点 "n12" → 12；端口 = (节点序号 << 8) | (输出位 0x80) | 端口下标
ed::NodeId node_handle(const std::string& node_id)
{
    return ed::NodeId(static_cast<uintptr_t>(engine::numericIdOf(node_id)));
}

ed::LinkId link_handle(const std::string& edge_id)
{
    return ed::LinkId(static_cast<uintptr_t>(engine::numericIdOf(edge_id)));
}

ed::PinId pin_handle(const Node& node, PortDirection direction, int index)
{
    const uintptr_t base  = static_cast<uintptr_t>(engine::numericIdOf(node.id)) << 8;
    const uintptr_t flags = (direction == PortDirection::Output) ? 0x80u : 0x00u;
    return ed::PinId(base | flags | static_cast<uintptr_t>(index & 0x7F));
}

struct PinRef {
    std::string   node_id;
    std::string   port_id;
    PortDirection direction = PortDirection::Input;
    bool          valid     = false;
};

PinRef resolve_pin(ed::PinId id, const Graph& graph)
{
    PinRef ref;
    const uintptr_t value = id.Get();
    if (value == 0) {
        return ref;
    }
    const int node_seq = static_cast<int>((value >> 8) & 0x7FFFFF);
    const PortDirection direction =
        ((value & 0x80u) != 0) ? PortDirection::Output : PortDirection::Input;
    const int index = static_cast<int>(value & 0x7F);

    const Node* node = graph.findNode("n" + std::to_string(node_seq));
    if (node == nullptr) {
        return ref;
    }
    const std::vector<Port>& ports = (direction == PortDirection::Input) ? node->inputs : node->outputs;
    if (index < 0 || index >= static_cast<int>(ports.size())) {
        return ref;
    }

    ref.node_id   = node->id;
    ref.port_id   = ports[static_cast<std::size_t>(index)].id;
    ref.direction = direction;
    ref.valid     = true;
    return ref;
}

// ------------------------------------------------------------- 初始化 -------
// 设计 §14.4：画布/节点/边框/选中 配色
void apply_editor_style()
{
    ed::Style& style              = ed::GetStyle();
    style.NodeRounding            = 4.0f;
    style.NodeBorderWidth         = 1.5f;
    style.SelectedNodeBorderWidth = 2.5f;
    style.NodePadding             = ImVec4(10.0f, 8.0f, 10.0f, 8.0f);
    // 说明（PA-08 治理）：vendored imgui-node-editor 的 Style **没有网格间距字段**，
    // 因此 ui.grid_size 目前无法生效 —— 已在 unwired_config_fields() 中如实登记并在启动日志提示
    style.Colors[ed::StyleColor_Bg]            = ImColor(30, 30, 30, 255);
    style.Colors[ed::StyleColor_Grid]          = ImColor(42, 42, 42, 255);
    style.Colors[ed::StyleColor_NodeBg]        = ImColor(45, 45, 45, 255);
    style.Colors[ed::StyleColor_NodeBorder]    = ImColor(58, 58, 58, 255);
    style.Colors[ed::StyleColor_SelNodeBorder] = ImColor(74, 144, 226, 255);
    style.Colors[ed::StyleColor_HovNodeBorder] = ImColor(74, 144, 226, 200);
}

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

    static std::string settings_file = (paths::data_root() / "node_editor.json").string();

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
    apply_editor_style();
    log::info("[画布] 编辑器样式已应用（缩放范围 0.1x ~ 4.0x）");

    // 设计 §6.7：MVP 不保留任何快捷键，全部改为鼠标操作（右键菜单 / 工具栏 / 菜单栏）
    ed::EnableShortcuts(false);
    log::info("[画布] 已关闭画布键盘快捷键（仅保留鼠标操作）");
}

// 节点内容宽度：输入端口行左对齐、输出端口行右对齐，都以它为基准（保证节点宽度一致）
constexpr float kNodeContentWidth = 215.0f;
constexpr float kPinRadius        = 5.0f;
constexpr float kPinDotSize       = kPinRadius * 2.0f;
constexpr float kPinTextGap       = 6.0f;

// ------------------------------------------------------------- 端口绘制 -----
// 设计 §14.7：**输入端口在节点左侧、输出端口在节点右侧**
//   · 输入行：[圆点][名称]          —— 圆点贴左边缘，连线接在左侧
//   · 输出行：[        名称][圆点]  —— 名称与圆点右对齐，圆点贴右边缘，连线接在右侧
//   · 颜色按端口类型（§4.5）；可变长端口多一圈描边
void draw_pin(const Node& node, const Port& port, int index)
{
    const bool is_input = (port.direction == PortDirection::Input);

    ed::BeginPin(pin_handle(node, port.direction, index),
                 is_input ? ed::PinKind::Input : ed::PinKind::Output);
    // 连线锚点：输入取端口矩形左中，输出取右中
    ed::PinPivotAlignment(is_input ? ImVec2(0.0f, 0.5f) : ImVec2(1.0f, 0.5f));

    ImDrawList* draw_list   = ImGui::GetWindowDrawList();
    const float line_height = ImGui::GetTextLineHeight();
    const ImU32 dot_color   = port_color(port.type);

    // 在"圆点占位 Dummy"的位置上画圆（占位 Dummy 同时保证端口矩形覆盖圆点）
    const auto draw_dot = [&](const ImVec2& dummy_top_left) {
        const ImVec2 center(dummy_top_left.x + kPinRadius, dummy_top_left.y + line_height * 0.5f);
        draw_list->AddCircleFilled(center, kPinRadius, dot_color, 16);
        draw_list->AddCircle(center, kPinRadius, IM_COL32(20, 20, 20, 255), 16, 1.5f);
        if (port.is_variadic) {
            draw_list->AddCircle(center, kPinRadius + 2.5f, dot_color, 16, 1.0f);
        }
    };

    if (is_input) {
        // ---- 左列：圆点 + 名称 ----
        const ImVec2 dot_pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(kPinDotSize, line_height));
        draw_dot(dot_pos);

        ImGui::SameLine(0.0f, kPinTextGap);
        ImGui::TextUnformatted(port.display_name.c_str());
    }
    else {
        // ---- 右列：名称 + 圆点（先用占位 Dummy 把内容推到右边缘）----
        const float text_width = ImGui::CalcTextSize(port.display_name.c_str()).x;
        const float spacer     = kNodeContentWidth - kPinDotSize - kPinTextGap - text_width;
        if (spacer > 0.0f) {
            ImGui::Dummy(ImVec2(spacer, line_height));
            ImGui::SameLine(0.0f, 0.0f);
        }

        ImGui::TextUnformatted(port.display_name.c_str());

        ImGui::SameLine(0.0f, kPinTextGap);
        const ImVec2 dot_pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(kPinDotSize, line_height));
        draw_dot(dot_pos);
    }

    ed::EndPin();
}

// ------------------------------------------------------------- 节点绘制 -----
// 设计 §14.1：标题栏（分类色 + 状态圆点）+ 端口 + 参数预览 + 错误提示
// ------------------------------------------------- 运行结果摘要（PA-03）----
// 节点内只读展示运行结果（与输出面板 / 参数面板「运行结果」同源 ui::node_output_text）。
// 用「运行信息条数 + 状态 + 错误长度 + 输出字节数」做轻量签名缓存，避免每帧重算长文本。
struct NodeResultView {
    std::string text;
    bool        has_content = false;
    bool        is_error    = false;
    bool        is_skipped  = false;
};

std::size_t result_signature(const Node& node, const engine::RunSnapshot& snapshot)
{
    std::size_t signature = snapshot.nodes.size() * 1000003u;
    signature += static_cast<std::size_t>(node.state) * 10007u;
    signature += node.error_message.size() * 31u;
    if (const engine::RunNodeView* view = snapshot.find(node.id)) {
        signature += view->text.size() * 17u; // PB-01：读快照文本长度（含流式增长）
    }
    return signature;
}

NodeResultView result_view_of(const Node& node)
{
    struct Cached {
        std::size_t    signature = 0;
        NodeResultView view;
        bool           valid = false;
    };
    static std::unordered_map<std::string, Cached> cache;

    const engine::RunSnapshot& snapshot = editor().run_snapshot_view();
    const std::size_t       signature = result_signature(node, snapshot);
    Cached&                 entry     = cache[node.id];
    if (entry.valid && entry.signature == signature) {
        return entry.view;
    }

    entry.valid     = true;
    entry.signature = signature;
    entry.view      = NodeResultView{};

    if (node.state == NodeState::Error) {
        std::string first = node.error_message;
        if (const std::size_t pos = first.find('\n'); pos != std::string::npos) {
            first.resize(pos);
        }
        if (first.size() > 120) {
            first.resize(117);
            first += "…";
        }
        entry.view.has_content = true;
        entry.view.is_error    = true;
        entry.view.text        = first.empty() ? std::string("执行失败") : first;
        return entry.view;
    }
    if (node.state == NodeState::Skipped) {
        entry.view.has_content = true;
        entry.view.is_skipped  = true;
        entry.view.text        = "已跳过（上游失败）";
        return entry.view;
    }
    if (node.state == NodeState::Running) {
        // PB-08：流式进度（增量字节数来自读模型；无增量时退回"运行中…"）
        const engine::RunNodeView* live  = snapshot.find(node.id);
        const std::size_t          bytes = (live != nullptr) ? live->delta_bytes : 0;
        entry.view.has_content = true;
        entry.view.text        = (bytes > 0) ? ("生成中…（" + std::to_string(bytes) + " 字）")
                                            : std::string("运行中…");
        return entry.view;
    }

    const std::string body = node_output_text(editor().graph, editor().run_snapshot_view(), node.id);
    if (!body.empty()) {
        std::string preview;
        for (const char ch : body) {
            if (preview.size() >= 160) {
                break;
            }
            preview += (ch == '\n' || ch == '\r') ? ' ' : ch;
        }
        entry.view.has_content = true;
        entry.view.text        = "结果： " + preview +
                          (body.size() > preview.size() ? "…" : "") +
                          "（" + std::to_string(body.size()) + " 字符）";
    }
    return entry.view;
}

void draw_node_body(const Node& node)
{
    engine::registerAllNodes();
    const engine::Definition* definition = engine::NodeRegistry::instance().find(node.type);
    const NodeCategory category =
        (definition != nullptr) ? definition->category : NodeCategory::Process;

    ed::BeginNode(node_handle(node.id));

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const float  height = ImGui::GetTextLineHeight() + 8.0f;
        // 分类色条 + 状态圆点
        draw_list->AddRectFilled(ImVec2(cursor.x - 4.0f, cursor.y),
                                 ImVec2(cursor.x, cursor.y + height), category_color(category), 2.0f);
        draw_list->AddCircleFilled(ImVec2(cursor.x + 7.0f, cursor.y + height * 0.5f), 4.0f,
                                   state_color(node.state), 12);
        // PA-08：ui.running_animation 开 → 运行中节点状态点加脉冲光环（关闭则静止）
        if (node.state == NodeState::Running && aiwrite::app_config().ui.running_animation) {
            const ImVec2 dot_center(cursor.x + 7.0f, cursor.y + height * 0.5f);
            const float  pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
            ImVec4       glow  = ImGui::ColorConvertU32ToFloat4(state_color(node.state));
            glow.w             = 0.30f + 0.45f * pulse;
            draw_list->AddCircle(dot_center, 6.0f + 3.0f * pulse,
                                 ImGui::ColorConvertFloat4ToU32(glow), 20, 1.5f);
        }
        ImGui::Dummy(ImVec2(13.0f, height));
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::TextUnformatted(node.title.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", node.id.c_str());
    }

    ImGui::Dummy(ImVec2(kNodeContentWidth, 1.0f));
    ImGui::Separator();

    // ---- 输入端口（左列）----
    int index = 0;
    for (const Port& port : node.inputs) {
        draw_pin(node, port, index++);
    }
    // ---- 输出端口（右列）----
    index = 0;
    for (const Port& port : node.outputs) {
        draw_pin(node, port, index++);
    }

    if (!node.params.empty()) { // 参数预览
        ImGui::Separator();
        int shown = 0;
        for (const engine::Param& param : node.params) {
            if (!engine::param_visible(node, param)) {
                continue; // 条件隐藏的参数不预览（如 web 模式下的 API Key / API 地址）
            }
            if (shown >= 2) {
                break;
            }
            std::string value = param.text();
            if (param.is_secret && !value.empty()) {
                value = "******";
            }
            if (value.size() > 18) {
                value = value.substr(0, 18) + "…";
            }
            ImGui::TextDisabled("%s: %s", param.display_name.c_str(),
                                value.empty() ? "(空)" : value.c_str());
            ++shown;
        }
    }

    // ---- 生效提供商提示（P1-a：provider 输入优先；official 未接线 → 必定失败）----
    if (engine::uses_provider(node.type)) {
        const engine::EffectiveProvider effective =
            engine::resolve_effective_provider(editor().graph, node);
        const std::string reason = engine::unwired_reason(editor().graph, node);

        ImGui::Separator();
        if (node.type == "LLMGenerate") {
            if (effective.from_edge) {
                ImGui::TextDisabled("生效 %s ← %s", effective.mode.c_str(),
                                    effective.source_node.c_str());
            }
            else {
                ImGui::TextDisabled("生效 %s（节点自身）", effective.mode.c_str());
            }
        }
        if (!reason.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kNodeContentWidth);
            ImGui::TextUnformatted((reason + "：必定失败").c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    }

    std::vector<std::string> errors;
    editor().graph.validateParams(node, &errors);
    if (!errors.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "参数校验 %d 项错误",
                           static_cast<int>(errors.size()));
    }

    // ---- 运行结果摘要（PA-03：只读；与输出面板 / 参数面板同源）----
    const NodeResultView result_view = result_view_of(node);
    if (result_view.has_content) {
        ImGui::Separator();
        if (result_view.is_error) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        }
        else if (result_view.is_skipped) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.70f, 0.30f, 1.0f));
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.31f, 0.75f, 0.42f, 1.0f));
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kNodeContentWidth);
        ImGui::TextUnformatted(result_view.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    ed::EndNode();
}

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
        PinRef from = resolve_pin(start_pin, state.graph);
        PinRef to   = resolve_pin(end_pin, state.graph);

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
            const bool  ok = state.graph.canConnect(from.node_id, from.port_id, to.node_id,
                                                    to.port_id, &reason, &would_replace);
            if (!ok) {
                ed::RejectNewItem(ImVec4(1.0f, 0.30f, 0.30f, 1.0f), 2.0f);
                if (ImGui::IsMouseReleased(0)) {
                    log::warn("[连线] 被拒绝：" + reason);
                    state.set_status("连线被拒绝：" + reason);
                }
            }
            else {
                if (ed::AcceptNewItem(ImVec4(0.29f, 0.56f, 0.89f, 1.0f), 2.0f)) {
                    state.snapshot(would_replace ? "替换连线" : "新建连线");
                    std::string error;
                    const std::string edge_id = state.graph.addEdge(
                        from.node_id, from.port_id, to.node_id, to.port_id, &error);
                    if (edge_id.empty()) {
                        log::error("[连线] 创建失败：" + error);
                        state.set_status("连线失败：" + error);
                    }
                    else {
                        state.log_summary("[连线] " + from.node_id + "." + from.port_id + " → " +
                                          to.node_id + "." + to.port_id);
                        state.set_status("已连线 " + from.node_id + " → " + to.node_id +
                                         (would_replace ? "（替换了原连线）" : ""));
                    }
                }
            }
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
                if (state.graph.removeEdge(edge_id)) {
                    state.log_summary("[连线] 删除 " + edge_id);
                    state.set_status("已删除连线 " + edge_id);
                }
            }
        }

        ed::NodeId deleted_node;
        while (ed::QueryDeletedNode(&deleted_node)) {
            if (ed::AcceptDeletedItem()) {
                const std::string node_id = "n" + std::to_string(deleted_node.Get());
                state.snapshot("删除节点");
                if (state.delete_node(node_id)) {
                    state.log_summary("[节点] 删除 " + node_id);
                    state.set_status("已删除节点 " + node_id + "（含其连线）");
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
    const int count = ed::GetSelectedObjectCount();
    const std::size_t capacity = static_cast<std::size_t>(std::max(count, 0));

    // 选中的节点
    std::vector<ed::NodeId> node_handles(capacity);
    const int node_count = (count > 0) ? ed::GetSelectedNodes(node_handles.data(), count) : 0;
    state.selected_nodes.clear();
    for (int i = 0; i < node_count; ++i) {
        state.selected_nodes.push_back("n" +
                                       std::to_string(node_handles[static_cast<std::size_t>(i)].Get()));
    }

    // 选中的连线（左键点连线 / 框选都会选中连线 → 可与工具栏「删除选中」联动）
    std::vector<ed::LinkId> link_handles(capacity);
    const int link_count = (count > 0) ? ed::GetSelectedLinks(link_handles.data(), count) : 0;
    state.selected_links.clear();
    for (int i = 0; i < link_count; ++i) {
        state.selected_links.push_back("e" +
                                       std::to_string(link_handles[static_cast<std::size_t>(i)].Get()));
    }

    if (state.selected_nodes.size() == 1) {
        state.selected_node = state.selected_nodes.front();
    }
    else if (state.selected_node.empty() ||
             std::find(state.selected_nodes.begin(), state.selected_nodes.end(),
                       state.selected_node) == state.selected_nodes.end()) {
        state.selected_node.clear();
    }
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
    for (Node& node : state.graph.nodes) {
        const ImVec2 position = ed::GetNodePosition(node_handle(node.id));

        const bool editor_knows =
            std::isfinite(position.x) && std::isfinite(position.y) &&
            std::fabs(position.x) < kPositionLimit && std::fabs(position.y) < kPositionLimit;

        if (!editor_knows) {
            ed::SetNodePosition(node_handle(node.id), ImVec2(node.x, node.y)); // 模型 → 编辑器
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
                                       NodeCategory::Output};
    for (NodeCategory category : categories) {
        if (!ImGui::BeginMenu(engine::categoryName(category))) {
            continue;
        }
        for (const engine::Definition* definition : registry.listByCategory(category)) {
            if (ImGui::MenuItem(definition->display_name.c_str())) {
                state.snapshot(std::string("创建节点 ") + definition->display_name);
                const std::string id = state.create_node(definition->type, spawn.x, spawn.y);
                if (!id.empty()) {
                    state.selected_nodes = {id};
                    state.selected_node  = id;
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
}

} // namespace

void draw_node_canvas(const char* title, CanvasOptions& options)
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

    ImGui::TextDisabled("输入端口在节点左侧、输出端口在右侧 ｜ 拖左侧圆点连线，类型不符会变红被拒");
    ImGui::SameLine();
    ImGui::TextDisabled("｜ 右键：画布新建 / 节点复制·删除 / 连线删除 ｜ 左键点连线可选中，再点工具栏「删除选中」");
    ImGui::Separator();

    ed::SetCurrentEditor(g_context);
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
            state.set_status("已重置画布视图（检测到异常缩放/原点）");
        }
    }

    if (!options.show_grid) {
        ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    }

    // 位置同步：新建 / 粘贴的节点在这里落到 Graph 记录的位置
    for (const Node& node : state.graph.nodes) {
        ed::SetNodePosition(node_handle(node.id), ImVec2(node.x, node.y));
    }
    for (const Node& node : state.graph.nodes) {
        draw_node_body(node);
    }
    for (const Edge& edge : state.graph.edges) {
        const Node* from = state.graph.findNode(edge.from_node);
        const Node* to   = state.graph.findNode(edge.to_node);
        if (from == nullptr || to == nullptr) {
            continue;
        }
        const int from_index = from->portIndex(edge.from_port, PortDirection::Output);
        const int to_index   = to->portIndex(edge.to_port, PortDirection::Input);
        if (from_index < 0 || to_index < 0) {
            continue;
        }
        ed::Link(link_handle(edge.id), pin_handle(*from, PortDirection::Output, from_index),
                 pin_handle(*to, PortDirection::Input, to_index));
    }

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
        state.selected_nodes = {context_node};
        state.selected_node  = context_node;
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
            state.paste_clipboard();
        }
        if (ImGui::IsItemHovered() && !state.has_clipboard()) {
            ImGui::SetTooltip("剪贴板为空：先在节点右键菜单里「复制」");
        }
        if (ImGui::MenuItem("删除选中（节点 / 连线）", nullptr, false, state.has_selection())) {
            state.delete_selected();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##node_menu")) {
        ImGui::TextDisabled("节点 %s", context_node.c_str());
        ImGui::Separator();
        // 设计 §6.1：右键节点 → 复制（粘贴在画布右键菜单）
        if (ImGui::MenuItem("复制", nullptr, false, state.has_selection())) {
            state.copy_selection();
        }
        if (ImGui::MenuItem("改名（右侧参数面板）")) {
            state.request_focus_title = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("删除该节点")) {
            state.delete_selected();
        }
        ImGui::EndPopup();
    }

    ed::Resume();
    // 双击节点标题 → 参数面板聚焦标题（设计 §6.1 重命名）
    const ed::NodeId double_clicked = ed::GetDoubleClickedNode();
    if (double_clicked.Get() != 0) {
        state.selected_nodes      = {"n" + std::to_string(double_clicked.Get())};
        state.selected_node       = state.selected_nodes.front();
        state.request_focus_title = true;
        state.set_status("可在右侧参数面板修改标题与参数：" + state.selected_node);
    }

    sync_selection(state);

    // 视图跟随前先确认坐标正常（历史卡死根因：异常坐标 + NavigateToContent 会让视图落到天文数字区间）
    const bool positions_sane =
        state.graph.nodes.empty() ||
        std::all_of(state.graph.nodes.begin(), state.graph.nodes.end(), [](const Node& node) {
            return std::isfinite(node.x) && std::isfinite(node.y) &&
                   std::fabs(node.x) < kPositionLimit && std::fabs(node.y) < kPositionLimit;
        });

    if (g_first_frame || state.request_navigate_to_content) {
        if (positions_sane) {
            ed::NavigateToContent(0.0f);
        }
        else {
            log::warn("[画布] 跳过视图跟随：存在异常节点坐标（已按上限夹紧）");
            state.set_status("已跳过视图跟随（检测到异常节点坐标）");
        }
        g_first_frame                     = false;
        state.request_navigate_to_content = false;
    }

    ed::End();

    if (!options.show_grid) {
        ed::PopStyleColor();
    }

    sync_positions(state);
    ed::SetCurrentEditor(nullptr);

    options.node_count = static_cast<int>(state.graph.nodes.size());
    options.link_count = static_cast<int>(state.graph.edges.size());

    ImGui::End();
}

std::string canvas_add_node_at_center(const std::string& type)
{
    ensure_context();
    EditorState& state = editor();

    ed::SetCurrentEditor(g_context);
    const ImVec2 center_canvas = ed::ScreenToCanvas(ImGui::GetMainViewport()->GetCenter());
    ed::SetCurrentEditor(nullptr);

    state.snapshot("创建节点");
    const std::string id = state.create_node(type, center_canvas.x, center_canvas.y);
    if (!id.empty()) {
        state.selected_nodes = {id};
        state.selected_node  = id;
    }
    return id;
}

} // namespace aiwrite::ui
