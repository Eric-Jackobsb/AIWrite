#include "node_canvas.h"

#include "composite.h"        // CN：上级节点实例识别 / 创建
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

// 节点宽度：**由「输入/输出标签实测最宽」决定，且全画布所有节点共用同一个值**
//   · 宽度 = max(所有节点里最宽的一行端口标签, 下限 `kNodeContentWidthMin`) —— 见 `update_content_width()`
//   · 因此节点的左边界（输入圆点）与右边界（输出圆点）在**所有节点上对齐**，且**与文本长度无关**
//   · 配套约束：节点内**任何文本都必须收口**（单行 `elide_to_width` / 多行 `wrap_to_width`），
//     否则长标题 / 长参数 / 长错误文案会把节点撑宽，端口边界随之漂移（同一画布上节点宽度不一致）
constexpr float kNodeContentWidthMin = 160.0f;              // 内容区下限（保证标题行 / 参数预览可读）
float           g_content_width      = kNodeContentWidthMin; // 每帧由 update_content_width() 重算
constexpr float kPinRadius        = 5.0f;
constexpr float kPinDotSize       = kPinRadius * 2.0f;
constexpr float kPinTextGap       = 6.0f;
constexpr float kNodeTitleIndent  = 15.0f; // 标题行左侧占位（分类色条 + 状态圆点）
constexpr float kLabelGap         = 4.0f;  // 同一行内文本片段之间的间隙
constexpr int   kNodeSummaryLines = 6;     // 节点卡片上多行文本（运行结果 / 必失败原因）最多几行

// 只裁剪、**不加**省略号：返回不超过 max_width 像素的最长前缀（不切断 UTF-8 多字节序列）。
std::string clip_to_width(const std::string& text, float max_width)
{
    if (text.empty()) {
        return std::string();
    }
    if (ImGui::CalcTextSize(text.c_str()).x <= max_width) {
        return text;
    }
    std::string out = text;
    while (!out.empty()) {
        std::size_t cut = out.size() - 1; // 回退一个 UTF-8 字符（绝不切断多字节序列）
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }
        out.erase(cut);
        if (ImGui::CalcTextSize(out.c_str()).x <= max_width) {
            return out;
        }
    }
    return std::string();
}

// 单行省略 = clip 到「max_width − 省略号宽度」再补 …（**保证结果 ≤ max_width**）。
// 按**像素**而非字符数裁剪 → 中英混排 / 全角半角 / 数字串都不会把节点撑宽。
std::string elide_to_width(const std::string& text, float max_width)
{
    const std::string ellipsis   = "…";
    const float       ellipsis_w = ImGui::CalcTextSize(ellipsis.c_str()).x;
    if (max_width <= ellipsis_w) {
        return ellipsis; // 连省略号都放不下
    }
    if (ImGui::CalcTextSize(text.c_str()).x <= max_width) {
        return text; // 放得下 → 原样返回（零开销路径）
    }
    return clip_to_width(text, max_width - ellipsis_w) + ellipsis;
}

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
        // 端口名按「内容宽度 − 圆点 − 间隙」收口（宽度由端口标签自适应，见 update_content_width）
        ImGui::TextUnformatted(
            elide_to_width(port.display_name, g_content_width - kPinDotSize - kPinTextGap)
                .c_str());
    }
    else {
        // ---- 右列：名称 + 圆点（名称先按可用宽度收口，再用占位 Dummy 推到右边界）----
        const std::string shown_name =
            elide_to_width(port.display_name, g_content_width - kPinDotSize - kPinTextGap);
        const float text_width = ImGui::CalcTextSize(shown_name.c_str()).x;
        const float spacer     = g_content_width - kPinDotSize - kPinTextGap - text_width;
        if (spacer > 0.0f) {
            ImGui::Dummy(ImVec2(spacer, line_height));
            ImGui::SameLine(0.0f, 0.0f);
        }

        ImGui::TextUnformatted(shown_name.c_str());

        ImGui::SameLine(0.0f, kPinTextGap);
        const ImVec2 dot_pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(kPinDotSize, line_height));
        draw_dot(dot_pos);
    }

    ed::EndPin();
}

// 折行（多行文本用）：按**像素**把文本切成若干行 —— **不依赖 ImGui 的自动换行**（它对中文这种
// 「无空格长词」的行为不可靠），所以中文也能在任意字符间断行；每行 ≤ max_width，
// 超过 max_lines 行或字符预算时在末行补 …（节点卡片是摘要，完整正文在输出面板）。
std::string wrap_to_width(const std::string& text, float max_width, int max_lines)
{
    constexpr std::size_t kCharBudget = 2000; // 性能兜底：超长正文只折前 2000 字节
    const std::string     source = (text.size() > kCharBudget) ? text.substr(0, kCharBudget) : text;
    const bool            budget_cut = source.size() != text.size();

    std::vector<std::string> lines;
    std::string              line;
    std::size_t              i = 0;
    while (i < source.size()) {
        std::size_t next = i + 1; // 取一个完整 UTF-8 字符（绝不切断多字节序列）
        while (next < source.size() &&
               (static_cast<unsigned char>(source[next]) & 0xC0u) == 0x80u) {
            ++next;
        }
        const std::string ch = source.substr(i, next - i);
        i = next;

        if (ch == "\n") { // 原文自带换行 → 直接断行
            lines.push_back(line);
            line.clear();
            continue;
        }
        if (!line.empty() && ImGui::CalcTextSize((line + ch).c_str()).x > max_width) {
            lines.push_back(line); // 放不下 → 在此字符前断行
            line = ch;
            continue;
        }
        line += ch;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }

    const bool line_cut = max_lines > 0 && static_cast<int>(lines.size()) > max_lines;
    if (line_cut) {
        lines.resize(static_cast<std::size_t>(max_lines));
    }

    // 收口保证①：任何一行都必须 ≤ max_width（修掉「单字符就超宽」「删行后末行过长」两种情形）
    for (std::string& item : lines) {
        if (ImGui::CalcTextSize(item.c_str()).x > max_width) {
            item = clip_to_width(item, max_width);
        }
    }

    // 收口保证②：要补 … 时，先给末行让出「省略号的宽度」（否则末行会超宽 —— 自检抓到过 220 > 215）
    const bool mark = line_cut || budget_cut;
    if (mark) {
        const float marker_w = ImGui::CalcTextSize("…").x;
        if (lines.empty()) {
            lines.emplace_back();
        }
        lines.back() = clip_to_width(lines.back(), max_width - marker_w);
    }

    std::string out;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) {
            out += "\n";
        }
        out += lines[index];
    }
    if (mark) {
        out += "…";
    }
    return out;
}

// 一行端口标签占用的宽度（左列 = [圆点][间隙][名称]，右列同宽）
float port_row_width(const Port& port)
{
    return kPinDotSize + kPinTextGap + ImGui::CalcTextSize(port.display_name.c_str()).x;
}

// 按当前图重算「全画布共用的内容区宽度」：取所有节点里**最宽的一行端口标签**（下限 kNodeContentWidthMin）。
// 每帧重算（代价 = 节点数 × 端口数 次 CalcTextSize，可忽略）→ 换工作流 / 增删节点即时生效。
void update_content_width(const Graph& graph)
{
    float width = kNodeContentWidthMin;
    for (const Node& node : graph.nodes) {
        for (const Port& port : node.inputs) {
            width = (std::max)(width, port_row_width(port));
        }
        for (const Port& port : node.outputs) {
            width = (std::max)(width, port_row_width(port));
        }
    }
    g_content_width = width;
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
        std::string prefix = "结果： ";
        if (node.type == "TextOutput") { // M_textio P3：最终输出
            std::string label;
            if (const engine::Param* label_param = node.findParam("label")) {
                label = label_param->text();
            }
            prefix = label.empty() ? std::string("最终输出： ") : ("最终输出（" + label + "）： ");
        }
        entry.view.has_content = true;
        entry.view.text        = prefix + preview +
                          (body.size() > preview.size() ? "…" : "") +
                          "（" + std::to_string(body.size()) + " 字符）";
    }
    return entry.view;
}

// P7a-16：节点卡片要显示的图片
//  * 优先**本次运行结果**（RunNodeView.images，任意节点）
//  * 其次「图片输入」节点的参数（解析资源令牌 → 真实路径）
std::string node_card_image(const Node& node)
{
    const engine::RunSnapshot& snapshot = editor().run_snapshot_view();
    if (const engine::RunNodeView* view = snapshot.find(node.id);
        view != nullptr && !view->images.empty()) {
        return view->images.front();
    }
    if (node.type != "ImageInput") {
        return {};
    }
    const engine::Param* path_param = node.findParam("path");
    if (path_param == nullptr) {
        return {};
    }
    const std::vector<std::string> entries = paths::split_path_list(path_param->text());
    if (entries.empty()) {
        return {};
    }
    bool              missing = false;
    std::string       error;
    const std::string local = asset::to_local_path(entries.front(), &missing, &error);
    return missing ? std::string() : local;
}

// 节点内条件分隔线：clamp 到节点内容区宽度（g_content_width），不超出节点边框。
// 不能直接用 ImGui::Separator() —— 节点内容是「画布窗口里的一个 Group」（NodeBuilder::Begin
// 只 SetCursorPos + NodePadding + BeginGroup），Separator 会按**画布窗口**的 Pos/Size 横跨
// 整个画布宽度（远宽于节点），视觉上就是「超出节点框的长灰线」（2026-10-15 修复）。
// 这里在光标当前 y 自绘一条恰好 g_content_width 宽的线，配色与垂直间距和 Separator 完全一致：
// 线画在光标处，随后推进一个 ItemSpacing.y（等价 Separator 的 ItemSize(0,0)）。
void draw_node_separator()
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 p        = ImGui::GetCursorScreenPos();
    draw_list->AddLine(p, ImVec2(p.x + g_content_width, p.y),
                       ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
    ImGui::Dummy(ImVec2(g_content_width, 0.0f)); // 占位推进一行；宽度顺带加固内容区宽度锚点
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
        // 分类色条 + 状态圆点；CN：上级节点用紫色条区分
        const ImU32 stripe = !node.composite_id.empty()
                                 ? IM_COL32(150, 110, 220, 255)
                                 : category_color(category);
        draw_list->AddRectFilled(ImVec2(cursor.x - 4.0f, cursor.y),
                                 ImVec2(cursor.x, cursor.y + height), stripe, 2.0f);
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
        // 宽度约束：标题 + (id) 合计必须落在内容宽度内（超出按像素省略，不撑宽节点）
        const float       title_avail = g_content_width - kNodeTitleIndent;
        const std::string id_text     = "(" + node.id + ")";
        const float       id_w        = ImGui::CalcTextSize(id_text.c_str()).x;
        const std::string shown_title =
            elide_to_width(node.title, title_avail - id_w - kLabelGap);
        ImGui::TextUnformatted(shown_title.c_str());
        ImGui::SameLine(0.0f, kLabelGap);
        const float id_room =
            title_avail - ImGui::CalcTextSize(shown_title.c_str()).x - kLabelGap;
        ImGui::TextDisabled("%s", elide_to_width(id_text, id_room).c_str());
    }

    // 宽度（**唯一**的宽度来源）：这一行占位把内容区宽度钉死为 g_content_width（全画布共用值），
    // 配合节点内所有文本的收口（elide / wrap）→ 画布上**所有节点等宽**。
    // 2026-10-15 确认：标题下方的 Separator 被用户视为「每个节点上面自带的长灰线」→ 移除（网格保留）。
    ImGui::Dummy(ImVec2(g_content_width, 1.0f));

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
        draw_node_separator();
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
            // 宽度约束：整行（参数名 + 值）按像素收口，避免长参数名撑宽节点
            const std::string line =
                param.display_name + ": " + (value.empty() ? std::string("(空)") : value);
            ImGui::TextDisabled("%s", elide_to_width(line, g_content_width).c_str());
            ++shown;
        }
    }

    // ---- P7a-16：图片卡片（缩略图 + 尺寸 + 格式徽标；格式取内容嗅探结果）----
    if (const std::string card_image = node_card_image(node); !card_image.empty()) {
        const TextureInfo texture = texture_for(card_image);
        draw_node_separator();
        if (texture.texture != 0) {
            float width  = static_cast<float>(texture.width);
            float height = static_cast<float>(texture.height);
            if (width > g_content_width && width > 0.0f) {
                const float shrink = g_content_width / width;
                width *= shrink;
                height *= shrink;
            }
            ImGui::Image(
                reinterpret_cast<ImTextureID>(static_cast<std::intptr_t>(texture.texture)),
                ImVec2(width, height));
            ImGui::TextDisabled("%d×%d · %s", texture.width, texture.height,
                                texture.format.empty() ? "未知格式" : texture.format.c_str());
        }
        else {
            // 宽度约束：解码失败文案可能很长（含魔数 / 扩展名建议）→ 按像素收口
            const std::string reason =
                "图片预览失败：" + (texture.error.empty() ? std::string("未知原因") : texture.error);
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                               elide_to_width(reason, g_content_width).c_str());
        }
    }

    // ---- 生效提供商提示（P1-a：provider 输入优先；official 未接线 → 必定失败）----
    if (engine::uses_provider(node.type)) {
        const engine::EffectiveProvider effective =
            engine::resolve_effective_provider(editor().graph, node);
        const std::string reason = engine::unwired_reason(editor().graph, node);

        draw_node_separator();
        if (node.type == "LLMGenerate") {
            // 固定宽度约束：来源节点标题长度不可控 → 按像素收口
            const std::string line =
                effective.from_edge
                    ? ("生效 " + effective.mode + " ← " + effective.source_node)
                    : ("生效 " + effective.mode + "（节点自身）");
            ImGui::TextDisabled("%s", elide_to_width(line, g_content_width).c_str());
        }
        if (!reason.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            // 自绘折行（每行 ≤ 内容宽度），不靠 ImGui 自动换行 → 长文案不撑宽节点
            ImGui::TextUnformatted(
                wrap_to_width(reason + "：必定失败", g_content_width, kNodeSummaryLines).c_str());
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
        draw_node_separator();
        if (result_view.is_error) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        }
        else if (result_view.is_skipped) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.70f, 0.30f, 1.0f));
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.31f, 0.75f, 0.42f, 1.0f));
        }
        // 自绘折行（每行 ≤ 内容宽度）+ 只显示前 kNodeSummaryLines 行 → 长正文不撑宽节点
        ImGui::TextUnformatted(
            wrap_to_width(result_view.text, g_content_width, kNodeSummaryLines).c_str());
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

    // ---- M_node_expansion L3-09：连线交互提示（鼠标旁显示端口类型与兼容性）----
    //  * QueryNewLink 已在上面调用过（end_pin 可能为 0 = 还没拖到目标端口）
    //  * 「类型不兼容变红」由 RejectNewItem 已提供（既有），这里补充文字提示
    if (creating && start_pin.Get() != 0) {
        const PinRef from = resolve_pin(start_pin, state.graph);
        const PinRef to   = resolve_pin(end_pin, state.graph);
        const Node*  fnode = from.valid ? state.graph.findNode(from.node_id) : nullptr;
        const Node*  tnode = to.valid ? state.graph.findNode(to.node_id) : nullptr;
        const Port*  fport = (fnode != nullptr) ? fnode->findPort(from.port_id, from.direction) : nullptr;
        const Port*  tport = (tnode != nullptr) ? tnode->findPort(to.port_id, to.direction) : nullptr;

        std::string tip;
        ImU32       color = IM_COL32(215, 215, 225, 255);
        if (fport != nullptr && tport != nullptr) {
            std::string reason;
            bool        would_replace = false;
            const bool  ok = state.graph.canConnect(from.node_id, from.port_id, to.node_id,
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
                                       NodeCategory::Output, NodeCategory::Auxiliary};
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
                        state.graph.addCompositeNode(spec->id, spawn.x, spawn.y, &error);
                    if (!id.empty()) {
                        state.selected_nodes = {id};
                        state.selected_node  = id;
                        state.set_status("已创建上级节点：" + spec->name);
                    }
                    else {
                        state.set_status("创建上级节点失败：" + error);
                    }
                }
            }
            ImGui::EndMenu();
        }
    }
}

// ------------------------------------------------- 宽度自检（不变量）----
// 画布上**所有节点必须等宽**：宽度 = g_content_width + 节点内边距（g_content_width 由「输入/输出
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
        const ImVec2 size = ed::GetNodeSize(node_handle(node.id));
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
                  " ~ " + px_text(max_w) + " px（内容区 " + px_text(g_content_width) +
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
        const std::string wrapped = wrap_to_width(sample, g_content_width, kNodeSummaryLines);
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
              " px（上限 " + px_text(g_content_width) + " px）→ " +
              (worst <= g_content_width + 0.5f ? "全部收口 ✅" : "有行超宽 ❌"));
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

    const ui::TextureCacheStats stats = ui::texture_cache_stats();
    const auto delta = [&diag, &stats](std::size_t TextureCacheStats::*member) {
        return stats.*member - (diag.has_prev ? diag.prev.*member : 0);
    };
    const std::size_t requests  = delta(&ui::TextureCacheStats::requests);
    const std::size_t hits      = delta(&ui::TextureCacheStats::hits);
    const std::size_t misses    = delta(&ui::TextureCacheStats::misses);
    const std::size_t decodes   = delta(&ui::TextureCacheStats::decodes);
    const std::size_t uploads   = delta(&ui::TextureCacheStats::uploads);
    const std::size_t evictions = delta(&ui::TextureCacheStats::evictions);

    log::info("[诊断] 画布 节点=" + std::to_string(state.graph.nodes.size()) +
              " 连线=" + std::to_string(state.graph.edges.size()) +
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
//       小地图交互由 draw_minimap 用「屏蔽前的快照」自行驱动。
//  废弃：旧版用 ed::CenterNodeOnScreen —— 它在 NodeBuilder::Begin 里会**平移节点
//       世界坐标**（m_Bounds.Translate + MakeDirty(User)），等于在小地图里改布局。
// ----------------------------------------------------------------------------
bool    g_minimap_blocking = false;                 // 本帧鼠标位于小地图区域（已对 ed 屏蔽输入）
bool    g_minimap_clicked  = false;                 // 屏蔽前快照：左键本帧点击
bool    g_minimap_down     = false;                 // 屏蔽前快照：左键按住中
ImVec2  g_minimap_mouse    = ImVec2(0.0f, 0.0f);    // 屏蔽前快照：鼠标屏幕坐标

// 小地图矩形（与 draw_minimap 共用同一套几何，单一来源）
void minimap_rect(ImVec2& out_min, ImVec2& out_max)
{
    const ImVec2 window_pos  = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    const float  margin      = 10.0f;
    out_min = ImVec2(window_pos.x + window_size.x - 190.0f - margin,
                     window_pos.y + window_size.y - 130.0f - margin);
    out_max = ImVec2(out_min.x + 190.0f, out_min.y + 130.0f);
}

bool minimap_hit(const ImVec2& mouse, const ImVec2& min, const ImVec2& max)
{
    return mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
}

// 在 ed::Begin 之前调用：小地图区域的鼠标事件只属于小地图，画布不得参与。
void block_minimap_input()
{
    g_minimap_blocking = false;
    g_minimap_clicked  = false;
    g_minimap_down     = false;
    if (!g_show_minimap) {
        return;
    }

    ImVec2 map_min, map_max;
    minimap_rect(map_min, map_max);

    ImGuiIO&    io    = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    if (!minimap_hit(mouse, map_min, map_max)) {
        return;
    }

    // 快照原始输入（draw_minimap 用；io 随后被清空，画布拿不到这些事件）
    g_minimap_blocking = true;
    g_minimap_mouse    = mouse;
    g_minimap_clicked  = io.MouseClicked[0];
    g_minimap_down     = io.MouseDown[0];

    for (int i = 0; i < 5; ++i) {
        io.MouseClicked[i]  = false;
        io.MouseDown[i]     = false;
        io.MouseReleased[i] = false;
    }
    io.MouseWheel  = 0.0f;
    io.MouseWheelH = 0.0f;
    io.MouseDelta  = ImVec2(0.0f, 0.0f);
}

// 上一帧节点位置（对齐辅助判断「谁在拖动」：imgui-node-editor 无公开拖动查询 API）
std::unordered_map<std::string, ImVec2> g_prev_positions;

// 拖拽中的节点 id：左键按住 + 位置相对上一帧移动 → 判定为拖动中
std::string find_dragged_node(const Graph& graph)
{
    if (g_alt_pan_active || !ImGui::IsMouseDown(kNodeEditorDragButton)) {
        g_prev_positions.clear();
        return {};
    }
    for (const Node& node : graph.nodes) {
        const ImVec2 current = ed::GetNodePosition(node_handle(node.id));
        const auto   it      = g_prev_positions.find(node.id);
        if (it == g_prev_positions.end()) {
            g_prev_positions[node.id] = current;
            continue;
        }
        if (std::fabs(current.x - it->second.x) > 0.5f ||
            std::fabs(current.y - it->second.y) > 0.5f) {
            return node.id;
        }
    }
    return {};
}

// 对齐辅助：拖动中把节点吸附到其它节点的左/右/中心X、上/下/中心Y
//  * 对齐线以屏幕坐标输出（紫色实线，仅拖动中显示）
struct SnapResult {
    bool   applied    = false;
    bool   line_v     = false; // 垂直对齐线
    bool   line_h     = false; // 水平对齐线
    ImVec2 v_a = ImVec2(0.0f, 0.0f);
    ImVec2 v_b = ImVec2(0.0f, 0.0f);
    ImVec2 h_a = ImVec2(0.0f, 0.0f);
    ImVec2 h_b = ImVec2(0.0f, 0.0f);
};
SnapResult apply_alignment_snap(const Graph& graph)
{
    SnapResult result;
    if (!g_snap_enabled || g_alt_pan_active) {
        g_prev_positions.clear();
        return result;
    }
    const std::string dragged = find_dragged_node(graph);
    if (dragged.empty()) {
        return result;
    }
    if (graph.findNode(dragged) == nullptr) {
        return result;
    }

    const float zoom        = ed::GetCurrentZoom();
    const float snap_canvas = (zoom > 1e-6f) ? g_snap_distance / zoom : g_snap_distance;

    const ImVec2 dpos  = ed::GetNodePosition(node_handle(dragged));
    const ImVec2 dsize = ed::GetNodeSize(node_handle(dragged));
    const float  dw    = dsize.x > 1.0f ? dsize.x : 80.0f;
    const float  dh    = dsize.y > 1.0f ? dsize.y : 40.0f;
    const float  dleft  = dpos.x;
    const float  dright = dpos.x + dw;
    const float  dcx    = dpos.x + dw * 0.5f;
    const float  dtop    = dpos.y;
    const float  dbottom = dpos.y + dh;
    const float  dcy     = dpos.y + dh * 0.5f;

    float       dx = 0.0f;
    float       dy = 0.0f;
    bool        have_x = false;
    bool        have_y = false;
    ImVec2      v_a, v_b, h_a, h_b;

    for (const Node& node : graph.nodes) {
        if (node.id == dragged) {
            continue;
        }
        const ImVec2 pos  = ed::GetNodePosition(node_handle(node.id));
        const ImVec2 size = ed::GetNodeSize(node_handle(node.id));
        const float  w    = size.x > 1.0f ? size.x : 80.0f;
        const float  h    = size.y > 1.0f ? size.y : 40.0f;
        const float  left   = pos.x;
        const float  right  = pos.x + w;
        const float  cx     = pos.x + w * 0.5f;
        const float  top    = pos.y;
        const float  bottom = pos.y + h;
        const float  cy     = pos.y + h * 0.5f;

        // X 方向：取 |delta| 最小的那对「拖拽边 ↔ 目标边」
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

        // Y 方向
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
            new_pos.x = dpos.x + dx;
            result.line_v = true;
            result.v_a = v_a;
            result.v_b = v_b;
        }
        if (have_y) {
            new_pos.y = dpos.y + dy;
            result.line_h = true;
            result.h_a = h_a;
            result.h_b = h_b;
        }
        ed::SetNodePosition(node_handle(dragged), new_pos);
        result.applied = true;
    }

    for (const Node& node : graph.nodes) {
        g_prev_positions[node.id] = ed::GetNodePosition(node_handle(node.id));
    }
    return result;
}
// 小地图（左下角缩略图；点击 / 按住拖动 → 视口中心平移到对应画布位置；右上角 ✕ 关闭）
// 纯导航：绝不选中 / 移动 / 框选节点 —— 输入已在 block_minimap_input 中与画布彻底隔离。
void draw_minimap(const Graph& graph)
{
    if (!g_show_minimap) {
        return;
    }
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    ImVec2 map_min, map_max;
    minimap_rect(map_min, map_max);

    draw_list->AddRectFilled(map_min, map_max, IM_COL32(18, 20, 26, 215), 4.0f);
    draw_list->AddRect(map_min, map_max, IM_COL32(130, 130, 145, 210), 4.0f, 0, 1.0f);
    draw_list->AddText(map_min + ImVec2(6.0f, 3.0f), IM_COL32(200, 200, 210, 235), "小地图");

    const ImVec2 close_pos(map_max.x - 18.0f, map_min.y + 2.0f);
    if (g_minimap_blocking && g_minimap_clicked && g_minimap_mouse.x >= close_pos.x &&
        g_minimap_mouse.x <= close_pos.x + 14.0f && g_minimap_mouse.y >= close_pos.y &&
        g_minimap_mouse.y <= close_pos.y + 14.0f) {
        g_show_minimap = false;
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
    for (const Node& node : graph.nodes) {
        const ImVec2 sz = ed::GetNodeSize(node_handle(node.id));
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

    for (const Node& node : graph.nodes) {
        const ImVec2 sz = ed::GetNodeSize(node_handle(node.id));
        const float  w  = std::max((sz.x > 1.0f ? sz.x : 80.0f) * scale, 3.0f);
        const float  h  = std::max((sz.y > 1.0f ? sz.y : 40.0f) * scale, 3.0f);
        const ImVec2 p(area_min.x + (node.x - bbox_min.x) * scale,
                       area_min.y + (node.y - bbox_min.y) * scale);
        draw_list->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + h),
                                 state_color(node.state), 1.0f);
        draw_list->AddRect(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + h),
                           IM_COL32(0, 0, 0, 190), 1.0f);
    }

    // 点击 / 按住拖动 → 视口中心平移到鼠标对应的世界坐标（保持当前缩放，快速挪动）。
    // 只用屏蔽前的快照；ed::NavigateToCenter 只动视口、不移动任何节点（旧版 CenterNodeOnScreen 已废弃）。
    if (g_minimap_blocking && (g_minimap_clicked || g_minimap_down) &&
        minimap_hit(g_minimap_mouse, area_min, area_max)) {
        const float cx = (g_minimap_mouse.x - area_min.x) / scale + bbox_min.x;
        const float cy = (g_minimap_mouse.y - area_min.y) / scale + bbox_min.y;
        ed::NavigateToCenter(ImVec2(cx, cy), 0.0f);
    }
}

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

    // 新增：Alt+左键拖拽平移（Alt 未按下时完全维持原有绑定，见 update_alt_left_pan）。
    // 放在提示行之前 → 提示能立刻反映本帧的平移模式。
    g_alt_pan_active = update_alt_left_pan();

    ImGui::TextDisabled("输入端口在节点左侧、输出端口在右侧 ｜ 拖左侧圆点连线，类型不符会变红被拒");
    ImGui::SameLine();
    ImGui::TextDisabled("｜ 右键：画布新建 / 节点复制·删除 / 连线删除 ｜ 左键点连线可选中，再点工具栏「删除选中」");
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
    block_minimap_input();
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
            state.set_status("已重置画布视图（检测到异常缩放/原点）");
        }
    }

    if (!options.show_grid) {
        ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    }

    diag_zoom = ed::GetCurrentZoom(); // CS-01：视图自愈之后再取，避免拿到被重置前的旧值

    // 位置同步：新建 / 粘贴的节点在这里落到 Graph 记录的位置
    for (const Node& node : state.graph.nodes) {
        ed::SetNodePosition(node_handle(node.id), ImVec2(node.x, node.y));
    }
    // 宽度自适应：按当前图的「输入/输出标签实测最宽」算出**全画布共用的内容区宽度**（每帧重算，代价可忽略）
    const auto diag_t0 = std::chrono::steady_clock::now();
    update_content_width(state.graph);
    const auto diag_t1 = std::chrono::steady_clock::now();
    for (const Node& node : state.graph.nodes) {
        draw_node_body(node);
    }
    const auto diag_t2 = std::chrono::steady_clock::now();
    log_wrap_selftest();                   // 折行自检（一次性：每条输出行都必须放得下）
    check_uniform_node_width(state.graph); // 固定宽度自检（不变量：全部节点等宽）
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
        // CN：上级节点 → 打开独立编辑器
        {
            const engine::Node* ctx_node = state.graph.findNode(context_node);
            if (ctx_node != nullptr && !ctx_node->composite_id.empty()) {
                ImGui::Separator();
                if (ImGui::MenuItem("编辑内部图…")) {
                    composite_editor_open(ctx_node->composite_id);
                }
            }
        }
        ImGui::Separator();
        // PD-06①（FEA-M4-11 · 通用重跑）：复用上游结果，受影响节点先清结果与状态
        {
            const engine::Node* ctx_node = state.graph.findNode(context_node);
            const bool ctx_rerun_ok =
                ctx_node != nullptr && ctx_node->state != engine::NodeState::Waiting &&
                !state.session_active();
            if (ImGui::MenuItem("重跑该节点", nullptr, false, ctx_rerun_ok)) {
                state.start_rerun_async(context_node, engine::RerunScope::NodeOnly);
            }
            if (ImGui::MenuItem("重跑该节点及下游", nullptr, false, ctx_rerun_ok)) {
                state.start_rerun_async(context_node, engine::RerunScope::Downstream);
            }
            if (ImGui::IsItemHovered() && ctx_node != nullptr &&
                ctx_node->state == engine::NodeState::Waiting) {
                ImGui::SetTooltip("该节点尚未运行过（状态为等待），暂不可重跑");
            }
        }
        if (ImGui::MenuItem("删除该节点")) {
            state.delete_selected();
        }
        ImGui::EndPopup();
    }

    ed::Resume();
    // 双击节点标题 → 参数面板聚焦标题（设计 §6.1 重命名）；CN：上级节点改为打开编辑器
    const ed::NodeId double_clicked = ed::GetDoubleClickedNode();
    if (double_clicked.Get() != 0) {
        const std::string node_id = "n" + std::to_string(double_clicked.Get());
        state.selected_nodes      = {node_id};
        state.selected_node       = node_id;
        if (const engine::Node* node = state.graph.findNode(node_id);
            node != nullptr && !node->composite_id.empty()) {
            composite_editor_open(node->composite_id);
            state.set_status("已打开上级节点编辑器：" + node->title + "（" +
                             node->composite_id + "）");
        }
        else {
            state.request_focus_title = true;
            state.set_status("可在右侧参数面板修改标题与参数：" + node_id);
        }
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

    // P7a-17：错误条点击「定位」→ 视图**居中并放大**到选中节点（不是仅跟随全部内容）
    if (state.request_focus_selection) {
        if (positions_sane) {
            ed::NavigateToSelection(true);
        }
        state.request_focus_selection = false;
    }

    ed::End();

    // ---- M_node_expansion L3（§4.4.3）：对齐辅助 ⑤+⑥ + 小地图 ④（ed::End 之后、画布上下文失效前）----
    {
        const SnapResult snap = apply_alignment_snap(state.graph);
        if (snap.applied) {
            // 紫色实线（仅拖动中显示；state_color 之外的专属色）
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (snap.line_v) {
                dl->AddLine(snap.v_a, snap.v_b, IM_COL32(0x9C, 0x6C, 0xFF, 230), 1.6f);
            }
            if (snap.line_h) {
                dl->AddLine(snap.h_a, snap.h_b, IM_COL32(0x9C, 0x6C, 0xFF, 230), 1.6f);
            }
        }
        draw_minimap(state.graph);
    }

    // CS-01：画布诊断（每 2 秒一行；把「帧预算归因 + 纹理请求/解码」写进 app.log）
    // 参数顺序必须与签名一致：(宽度重算 t0→t1, 节点循环 t1→t2, 连线+自检 t2→t3)
    log_canvas_diagnostic(state, diag_zoom, ms_between(diag_t0, diag_t1), ms_between(diag_t1, diag_t2),
                          ms_between(diag_t2, diag_t3));

    // P7a-14：空画布引导（新用户第一眼就知道下一步做什么）
    if (state.graph.nodes.empty()) {
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

// CN（Composition_node）：依据规格在画布可视中心创建上级节点
std::string canvas_add_composite_at_center(const std::string& composite_id)
{
    ensure_context();
    EditorState& state = editor();

    ed::SetCurrentEditor(g_context);
    const ImVec2 center_canvas = ed::ScreenToCanvas(ImGui::GetMainViewport()->GetCenter());
    ed::SetCurrentEditor(nullptr);

    state.snapshot("创建上级节点");
    std::string error;
    const std::string id =
        state.graph.addCompositeNode(composite_id, center_canvas.x, center_canvas.y, &error);
    if (!id.empty()) {
        state.selected_nodes = {id};
        state.selected_node  = id;
        if (const engine::Node* node = state.graph.findNode(id)) {
            state.set_status("已创建上级节点：" + node->title + "（双击进入内部图编辑）");
        }
    }
    else {
        state.set_status("创建上级节点失败：" + error);
    }
    return id;
}

} // namespace aiwrite::ui
